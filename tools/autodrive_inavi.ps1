param(
    [string]$Emulator = ".\x64\Debug\iNavi_Unicorn_Emulator.exe",
    [string]$Target = "D:\INAVI_Emulator\INAVI\INavi\INavi.exe",
    [string]$Registry = ".\regs.json",
    [string]$SerialMap = ".\serial_devices.json",
    [string]$SdmmcPath = "D:\INAVI_Emulator\INAVI",
    [string[]]$DllSearchDir = @(
        "C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Mfc\Lib\Mipsii",
        "C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Mfc\Lib\Mipsii\L.kor",
        "C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Atl\Lib\Mipsii"
    ),
    [UInt64]$Instructions = 50000000000,
    [int]$StartupTimeoutMs = 45000,
    [int]$InitialSettleMs = 18000,
    [int]$AfterTapMs = 4500,
    [AllowEmptyString()]
    [string]$Taps = "",
    [switch]$NoTaps,
    [switch]$RoutePreset,
    [string]$OutputRoot = ".\captures",
    [switch]$KeepAlive
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Drawing

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class AutodriveNative {
    public delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hwnd);

    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    public static extern int GetClassName(IntPtr hwnd, StringBuilder className, int maxCount);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetClientRect(IntPtr hwnd, out RECT rect);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool ClientToScreen(IntPtr hwnd, ref POINT point);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, UInt32 flags);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hwnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hwnd, int commandShow);

    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hwnd, UInt32 message, UIntPtr wParam, UIntPtr lParam);
}
"@

function Quote-Arg([string]$Value) {
    '"' + ($Value -replace '"', '\"') + '"'
}

function Get-WindowClass([IntPtr]$Hwnd) {
    if ($Hwnd -eq [IntPtr]::Zero) { return "" }
    $builder = New-Object Text.StringBuilder 256
    [void][AutodriveNative]::GetClassName($Hwnd, $builder, $builder.Capacity)
    $builder.ToString()
}

function Test-PresenterWindow([IntPtr]$Hwnd) {
    if ($Hwnd -eq [IntPtr]::Zero) { return $false }
    if (-not [AutodriveNative]::IsWindowVisible($Hwnd)) { return $false }
    $className = Get-WindowClass $Hwnd
    if ($className -eq "FakeCEHostPresenterWindow") {
        return $true
    }
    $client = New-Object AutodriveNative+RECT
    if (-not [AutodriveNative]::GetClientRect($Hwnd, [ref]$client)) {
        return $false
    }
    $width = [Math]::Max(0, $client.Right - $client.Left)
    $height = [Math]::Max(0, $client.Bottom - $client.Top)
    return ($width -ge 100 -and $height -ge 100)
}

function Get-VisibleWindowForProcess([Diagnostics.Process]$Process, [int]$TimeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) {
            return [IntPtr]::Zero
        }
        $Process.Refresh()
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero -and
            [AutodriveNative]::IsWindowVisible($Process.MainWindowHandle) -and
            (Get-WindowClass $Process.MainWindowHandle) -eq "FakeCEHostPresenterWindow") {
            return $Process.MainWindowHandle
        }

        $script:autodriveTargetPid = [uint32]$Process.Id
        $script:autodriveFoundHwnd = [IntPtr]::Zero
        $script:autodriveBestHwnd = [IntPtr]::Zero
        $script:autodriveBestArea = 0
        $callback = [AutodriveNative+EnumWindowsProc]{
            param([IntPtr]$hwnd, [IntPtr]$lParam)
            [uint32]$windowPid = 0
            [void][AutodriveNative]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
            if ($windowPid -eq $script:autodriveTargetPid -and [AutodriveNative]::IsWindowVisible($hwnd)) {
                $className = Get-WindowClass $hwnd
                $client = New-Object AutodriveNative+RECT
                [void][AutodriveNative]::GetClientRect($hwnd, [ref]$client)
                $width = [Math]::Max(0, $client.Right - $client.Left)
                $height = [Math]::Max(0, $client.Bottom - $client.Top)
                $area = $width * $height
                if ($className -eq "FakeCEHostPresenterWindow") {
                    $script:autodriveFoundHwnd = $hwnd
                    return $false
                }
                if ($width -ge 100 -and $height -ge 100 -and $area -gt $script:autodriveBestArea) {
                    $script:autodriveBestArea = $area
                    $script:autodriveBestHwnd = $hwnd
                }
            }
            return $true
        }
        [void][AutodriveNative]::EnumWindows($callback, [IntPtr]::Zero)
        if ($script:autodriveFoundHwnd -ne [IntPtr]::Zero) {
            return $script:autodriveFoundHwnd
        }
        if ($script:autodriveBestHwnd -ne [IntPtr]::Zero -and (Test-PresenterWindow $script:autodriveBestHwnd)) {
            return $script:autodriveBestHwnd
        }
        if (Test-PresenterWindow $Process.MainWindowHandle) {
            return $Process.MainWindowHandle
        }
        Start-Sleep -Milliseconds 250
    }
    return [IntPtr]::Zero
}

function Get-ClientInfo([IntPtr]$Hwnd) {
    $client = New-Object AutodriveNative+RECT
    if (-not [AutodriveNative]::GetClientRect($Hwnd, [ref]$client)) {
        throw "GetClientRect failed for HWND $Hwnd"
    }
    $origin = New-Object AutodriveNative+POINT
    $origin.X = 0
    $origin.Y = 0
    if (-not [AutodriveNative]::ClientToScreen($Hwnd, [ref]$origin)) {
        throw "ClientToScreen failed for HWND $Hwnd"
    }
    [pscustomobject]@{
        X = $origin.X
        Y = $origin.Y
        Width = [Math]::Max(1, $client.Right - $client.Left)
        Height = [Math]::Max(1, $client.Bottom - $client.Top)
    }
}

function Save-WindowScreenshot([IntPtr]$Hwnd, [string]$Path) {
    [void][AutodriveNative]::ShowWindow($Hwnd, 9)
    [void][AutodriveNative]::SetForegroundWindow($Hwnd)
    Start-Sleep -Milliseconds 150
    $rect = New-Object AutodriveNative+RECT
    if (-not [AutodriveNative]::GetWindowRect($Hwnd, [ref]$rect)) {
        throw "GetWindowRect failed for HWND $Hwnd"
    }
    $width = [Math]::Max(1, $rect.Right - $rect.Left)
    $height = [Math]::Max(1, $rect.Bottom - $rect.Top)
    $bitmap = New-Object Drawing.Bitmap $width, $height
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $hdc = [IntPtr]::Zero
    try {
        $hdc = $graphics.GetHdc()
        $printed = [AutodriveNative]::PrintWindow($Hwnd, $hdc, 2)
        $graphics.ReleaseHdc($hdc)
        $hdc = [IntPtr]::Zero
        if (-not $printed) {
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        }
        $bitmap.Save($Path, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($hdc -ne [IntPtr]::Zero) {
            $graphics.ReleaseHdc($hdc)
        }
        $graphics.Dispose()
        $bitmap.Dispose()
    }
}

function Send-GuestTap([IntPtr]$Hwnd, [int]$GuestX, [int]$GuestY) {
    $info = Get-ClientInfo $Hwnd
    $hostX = [int][Math]::Round($GuestX * $info.Width / 800.0)
    $hostY = [int][Math]::Round($GuestY * $info.Height / 480.0)
    $lParamValue = (($hostY -band 0xffff) -shl 16) -bor ($hostX -band 0xffff)
    $lParam = [UIntPtr]::new([uint32]$lParamValue)
    [void][AutodriveNative]::SetForegroundWindow($Hwnd)
    [void][AutodriveNative]::PostMessage($Hwnd, 0x0200, [UIntPtr]::Zero, $lParam)
    Start-Sleep -Milliseconds 80
    [void][AutodriveNative]::PostMessage($Hwnd, 0x0201, [UIntPtr]::new(1), $lParam)
    Start-Sleep -Milliseconds 120
    [void][AutodriveNative]::PostMessage($Hwnd, 0x0202, [UIntPtr]::Zero, $lParam)
    [pscustomobject]@{
        GuestX = $GuestX
        GuestY = $GuestY
        HostX = $hostX
        HostY = $hostY
        ClientWidth = $info.Width
        ClientHeight = $info.Height
    }
}

function Parse-TapPlan([string]$Plan) {
    foreach ($tap in ($Plan -split ';')) {
        $trimmed = $tap.Trim()
        if (-not $trimmed) { continue }
        $parts = $trimmed -split ',', 4
        if ($parts.Count -lt 2) {
            throw "Bad tap entry '$trimmed'. Use x,y,label[,delayMs]."
        }
        [pscustomobject]@{
            X = [int]$parts[0]
            Y = [int]$parts[1]
            Label = if ($parts.Count -ge 3 -and $parts[2]) { $parts[2] } else { "tap" }
            DelayMs = if ($parts.Count -ge 4 -and $parts[3]) { [int]$parts[3] } else { $AfterTapMs }
        }
    }
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$runDir = Join-Path $OutputRoot "inavi_autodrive_$timestamp"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$stdoutPath = Join-Path $runDir "emulator.stdout.log"
$stderrPath = Join-Path $runDir "emulator.stderr.log"
$manifestPath = Join-Path $runDir "manifest.json"

$defaultTapPlan = "650,430,safety_ok;725,35,current_position;665,35,menu_grid;135,250,route_search;500,445,recent_destination"
$routeTapPlan = "650,430,safety_ok_1,6000;650,430,safety_ok_2,6000;650,430,safety_ok_3,6000;650,430,safety_ok_4,8000;400,365,serial_popup_ok,5000;650,430,safety_ok_after_serial,12000;760,95,red_search,9000;135,180,destination_tab,7000;500,445,bottom_confirm_1,5000;650,430,confirm_ok_1,5000;675,430,right_confirm_1,5000;650,430,confirm_ok_2,5000"
if ($NoTaps) {
    $Taps = ""
} elseif ($RoutePreset -and -not $Taps) {
    $Taps = $routeTapPlan
} elseif (-not $Taps) {
    $Taps = $defaultTapPlan
}

$argumentList = @($Target, "--registry", $Registry, "--sdmmc-path", $SdmmcPath)
if ($SerialMap) {
    $argumentList += @("--serial-map", $SerialMap)
}
$argumentList += @("--instructions", [string]$Instructions)
$argumentList += $DllSearchDir

$emulatorPath = (Resolve-Path $Emulator).Path
$workingDirectory = (Resolve-Path ".").Path
$argumentString = ($argumentList | ForEach-Object { Quote-Arg $_ }) -join " "
$process = $null

$events = [System.Collections.Generic.List[object]]::new()
$exitCode = $null
$hwnd = [IntPtr]::Zero

try {
    $process = Start-Process -FilePath $emulatorPath `
                             -ArgumentList $argumentString `
                             -WorkingDirectory $workingDirectory `
                             -RedirectStandardOutput $stdoutPath `
                             -RedirectStandardError $stderrPath `
                             -PassThru
    $events.Add([pscustomobject]@{ kind="launch"; pid=$process.Id; args=$argumentList; time=(Get-Date).ToString("o") })

    $hwnd = Get-VisibleWindowForProcess $process $StartupTimeoutMs
    if ($hwnd -eq [IntPtr]::Zero) {
        $events.Add([pscustomobject]@{ kind="window_missing"; processExited=$process.HasExited; time=(Get-Date).ToString("o") })
    } else {
        $events.Add([pscustomobject]@{ kind="window"; hwnd=$hwnd.ToInt64(); time=(Get-Date).ToString("o") })
        Start-Sleep -Milliseconds $InitialSettleMs
        $path = Join-Path $runDir "00_initial.png"
        Save-WindowScreenshot $hwnd $path
        $events.Add([pscustomobject]@{ kind="screenshot"; file=(Split-Path -Leaf $path); time=(Get-Date).ToString("o") })

        if ($Taps) {
            $index = 1
            foreach ($tap in (Parse-TapPlan $Taps)) {
                if ($process.HasExited) { break }
                $tapResult = Send-GuestTap $hwnd $tap.X $tap.Y
                $events.Add([pscustomobject]@{
                    kind="tap"
                    label=$tap.Label
                    guestX=$tapResult.GuestX
                    guestY=$tapResult.GuestY
                    hostX=$tapResult.HostX
                    hostY=$tapResult.HostY
                    clientWidth=$tapResult.ClientWidth
                    clientHeight=$tapResult.ClientHeight
                    time=(Get-Date).ToString("o")
                })
                Start-Sleep -Milliseconds $tap.DelayMs
                if (-not $process.HasExited) {
                    $safeLabel = ($tap.Label -replace '[^A-Za-z0-9_.-]', '_')
                    $path = Join-Path $runDir ("{0:00}_{1}.png" -f $index, $safeLabel)
                    Save-WindowScreenshot $hwnd $path
                    $events.Add([pscustomobject]@{
                        kind="screenshot"
                        file=(Split-Path -Leaf $path)
                        after=$tap.Label
                        time=(Get-Date).ToString("o")
                    })
                }
                $index++
            }
        }
    }

    if (-not $KeepAlive -and -not $process.HasExited) {
        $events.Add([pscustomobject]@{ kind="stop"; method="CloseMainWindowOrKill"; time=(Get-Date).ToString("o") })
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) {
            $process.Kill()
            $process.WaitForExit()
        }
    } elseif ($KeepAlive) {
        $events.Add([pscustomobject]@{ kind="keep_alive"; pid=$process.Id; time=(Get-Date).ToString("o") })
    }

    if ($process.HasExited) {
        $exitCode = $process.ExitCode
    }
} finally {
    $manifest = [pscustomobject]@{
        runDir = (Resolve-Path $runDir).Path
        pid = if ($process) { $process.Id } else { $null }
        exitCode = $exitCode
        keptAlive = [bool]$KeepAlive
        events = $events
    }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -Encoding UTF8 $manifestPath
}

Write-Host "autodrive output: $runDir"
Write-Host "manifest: $manifestPath"
