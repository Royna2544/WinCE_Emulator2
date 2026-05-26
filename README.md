# iNavi Unicorn Emulator v2

Windows CE/MIPS emulator for the iNavi SE G3 navigation application.

This is a real-emulation project, not a screenshot faker. The goal is to run
the guest application through a MIPS CPU emulator, load its PE imports, and
provide Windows CE API/device behavior through host-backed shims where the real
system boundary is understood.

## Target

- Application family: iNavi SE G3 navigation UI
- Main guest executable: `INavi.exe`
- Guest OS model: Windows CE 6.0 user-mode environment
- Guest CPU/PE format: MIPS R4000, little-endian PE32
- Host target: Windows x64, latest local Visual Studio toolchain
- Current host tuning target: x86_64, Zen5-class CPU

Common local target path:

```text
D:\INAVI_Emulator\INAVI\INavi\INavi.exe
```

The real-device evidence dump currently lives under:

```text
D:\INAVI_Emulator
```

## What Works Now

- Loads MIPS Windows CE PE images and maps imports/exports.
- Uses Unicorn for MIPS execution.
- Uses real target DLLs when available through DLL search directories.
- Provides synthetic modules for key CE DLL boundaries:
  `coredll.dll`, `commctrl.dll`, `winsock.dll`, `ole32.dll`, and `oleaut32.dll`.
- Renders the main iNavi map again after earlier white-map regressions.
- Touch delivery is improved compared with the previous busy-loop/frozen state.
- Host presenter windows show guest UI surfaces.
- Registry values are loaded from `regs.json`.
- Guest `\SDMMC Disk` file access is backed by `--sdmmc-path`.
- Serial and stream devices are configured through `--serial-map`.
- `COM1:` is confirmed by real-device report as the GPS NMEA port at `9600 8N1`.
- `DEVICES.md` indexes current device evidence for GPS and sensor stream devices.

## Important Current Limits

- Route search is still incomplete, but with a headless diagnostic
  `MultiTBT.exe` companion the app can now advance into route-result/map
  control states. The final guidance/completion handoff is still under
  investigation, and no guest `CreateProcessW` launch for `MultiTBT.exe` has
  been observed.
- Modal/window ordering is still incomplete. Popups can lag behind audio, and
  under-layer buttons can still receive touches in some overlay states.
- The current SDMMC/profile data can still select `COM7:` for GPS even though
  the real device GPS stream is `COM1:`. This is treated as a profile/config
  data problem, not a registry explanation.
- Most custom stream devices are honest stubs. Do not invent sensor, MCU, or
  button behavior until the guest IOCTL protocol is observed.

## Source Layout

Core entry and loader:

- `src/main.cpp` parses command-line arguments, loads PE images, maps modules,
  applies relocations, binds imports, starts Unicorn, and owns current
  targeted diagnostics.
- `src/synthetic_dll.h` declares the synthetic CE runtime surface.
- `src/synthetic_dll.cpp` implements the main synthetic runtime, host presenter,
  guest handles, device map loading, module registration, cross-process shared
  guest-window behavior, and generic dispatch helpers.

COREDLL and API shims:

- `src/coredll_audio.cpp` audio and sound-related handlers.
- `src/coredll_comm.cpp` serial communication APIs such as comm state, masks,
  timeouts, purge, wait, and clear-error behavior.
- `src/coredll_crt.cpp` CRT-style helpers, math/format/security exports.
- `src/coredll_fs.cpp` guest file/device open/read/write/find behavior and
  host path translation.
- `src/coredll_gui.cpp` non-window GUI helpers.
- `src/coredll_math.cpp` floating-point/math exports.
- `src/coredll_memory.cpp` heap/local/global memory exports.
- `src/coredll_named_dispatch.cpp` named ordinal dispatch for many CE APIs,
  including message routing and process/window helpers.
- `src/coredll_paint.cpp` paint, DIB, DC, and bitmap helpers.
- `src/coredll_rect.cpp` rectangle helpers.
- `src/coredll_registry.cpp` registry API behavior backed by `regs.json`.
- `src/coredll_res.cpp` resource, menu, string, bitmap, icon, and dialog helpers.
- `src/coredll_sync.cpp` sync helpers.
- `src/coredll_system.cpp` system and error-state helpers.
- `src/coredll_thread.cpp` guest thread/event/wait helpers.
- `src/coredll_time.cpp` time, tick, sleep, and system-time helpers.
- `src/coredll_window.cpp` window APIs including title propagation.

Other synthetic DLLs:

- `src/synthetic_dll_commctrl.cpp`
- `src/synthetic_dll_ole32.cpp`
- `src/synthetic_dll_oleaut32.cpp`
- `src/synthetic_dll_winsock.cpp`

Tools and data:

- `tools/autodrive_inavi.ps1` launches bounded interactive/debug runs, captures
  logs/screenshots, and can inject taps.
- `regs.json` contains current modeled registry data.
- `serial_devices.json` contains current host/guest serial and stream-device
  mapping.
- `DEVICES.md` records current real-device stream-device evidence.
- `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md` are durable project memory.
- `NMEASender.cpp` is the host NMEA feeder utility source.

## Build

From WSL:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' iNavi_Unicorn_Emulator.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:VcpkgRoot=D:\vcpkg\ /m"
```

Release:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' iNavi_Unicorn_Emulator.vcxproj /p:Configuration=Release /p:Platform=x64 /p:VcpkgRoot=D:\vcpkg\ /m"
```

Dependencies are declared in `vcpkg.json`:

- `unicorn`
- `spdlog`
- `nlohmann-json`

The host 4K upscaler also uses Windows SDK Direct3D libraries
(`d3d11.lib`, `dxgi.lib`, and `d3dcompiler.lib`). NVIDIA Image Scaling is not
pulled from vcpkg; the MIT-licensed SDK shader/config files are vendored under
`third_party/NVIDIAImageScaling`.

## Run

Normal bounded debug harness:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& 'tools\autodrive_inavi.ps1' -NoTaps -KeepAlive -InitialSettleMs 8000 -StartupTimeoutMs 45000"
```

Manual emulator shape:

```text
iNavi_Unicorn_Emulator.exe <primary.exe>
  --registry regs.json
  --sdmmc-path <host_dir_backing_guest_sdmmc_disk>
  --serial-map serial_devices.json
  --instructions <count>
  [--guest-command-line text]
  [--host-upscale 4k|WxH|off]
  [--headless]
  [dll_search_dir ...]
```

Guest `CreateProcessW` children default to separate child emulator processes
and are launched with `--headless` plus hidden/no-console host startup flags.
Shared in-runtime child EXE launch is a diagnostic opt-in only through
`INAVI_EMU_INPROC_CHILD_PROCESS`.

The autodrive harness also supports generic diagnostic companions:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/autodrive_inavi.ps1 -CompanionTarget 'D:\INAVI_Emulator\INAVI\TBT\MultiTBT.exe'
```

This shares the parent run's guest-window registry and keeps the companion
headless. It is a diagnostic launcher, not an emulator-side app-name shortcut.

Important path rule:

- `--sdmmc-path` is a host directory whose contents appear as guest
  `\SDMMC Disk`.
- Do not pass a guest path like `\SDMMC Disk` to `--sdmmc-path`.

## Logging

Set `INAVI_EMU_LOG` to control spdlog verbosity:

```text
trace, debug, info, warn, error, off
```

Debug builds default to `info`; Release builds default to `error`.

Set `INAVI_EMU_DUMPS=1` in Debug builds to emit simple framebuffer PPM dumps.
The autodrive harness writes run artifacts under `captures/`.

## Host Presenter Scaling

The guest framebuffer and CE-reported screen metrics remain at the emulated
device size. For host-only 4K presentation, launch with:

```text
--host-upscale 4k
```

`4k` maps to a 3840x2160 host client area. Custom `WxH` values are also
accepted. When host upscaling is enabled, the presenter tries the Direct3D 11
NVIDIA Image Scaling backend first and falls back to the GDI presenter if D3D11
or shader compilation is unavailable. The presenter preserves the guest aspect
ratio inside the host client area and maps host mouse coordinates back through
that displayed image rectangle before queuing guest mouse messages.

Set `INAVI_EMU_DISABLE_D3D_NIS=1` to force the GDI presenter while keeping the
same host window size and coordinate mapping.

## Device Knowledge

The real-device serial/stream report is `D:\INAVI_Emulator\report_serial.txt`.
Current highlights:

- `COM1:` / `VSP.dll`: real GPS NMEA serial path, `9600 8N1`.
- `COM3:` / `au16550.dll`: UART candidate, `9600 8N1`, no automatic RX observed.
- `UID1:` / `NANDUUID.dll`: NAND UUID custom stream device. The emulator has
  a narrow `NANDUUID_RETURN` backend for observed UID probes.
- `SMB1:` / `SMB380.dll`: accelerometer stream driver. Disassembly shows
  I2C/SPI sensor internals and many SMB380 IOCTLs.
- `MFS1:` / `YAS526B.dll`: magnetic/e-compass stream driver. Disassembly shows
  two `I2C2:` handles for XY and Z chip paths and a small IOCTL range.

See `DEVICES.md` for indexed evidence and emulator implications.

## Current Investigation Threads

- Route search process/UI behavior after the verified route-result transition,
  especially the missing `MultiTBT` companion window/process launch path and
  final guidance/completion handoff.
- Modal/topmost window ordering and input capture.
- The COM profile data path that makes this dump choose `COM7:` despite real
  hardware reporting GPS on `COM1:`.
- Narrow stream-device IOCTL tracing for `SMB1:` and `MFS1:` before writing
  non-stub handlers.

Keep temporary diagnostics separated from real emulator fixes.
