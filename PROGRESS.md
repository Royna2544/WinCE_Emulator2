# Progress

Last refreshed: 2026-05-25.

## Current State

- The main iNavi map renders again.
- Touch input is more responsive than the earlier frozen/busy-loop state, but
  modal and overlay input routing is still incomplete.
- Route search still does not complete.
- The emulator is using real-ish CE API and device boundaries rather than
  hardcoded app behavior.
- `README.md`, `DEVICES.md`, `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md`
  have been refreshed to match the current investigation state.

## Confirmed Emulator Capabilities

- Loads Windows CE MIPS R4000 PE32 guest images.
- Maps real guest DLLs when available through DLL search paths.
- Uses synthetic DLLs for important OS/API boundaries:
  `coredll.dll`, `commctrl.dll`, `winsock.dll`, `ole32.dll`, and `oleaut32.dll`.
- Runs guest MIPS code through Unicorn.
- Provides host presenter windows for guest UI output.
- Reads registry data from `regs.json`.
- Backs guest `\SDMMC Disk` with the host directory passed through
  `--sdmmc-path`.
- Uses `--serial-map` JSON version `1` for host/guest serial and stream-device
  mapping.
- The old `--fs-root` and GPS-specific comm argument are gone.

## Recent Fixes

- `coredll #0x0100` is correctly treated as `SetWindowTextW`, based on CE 4.2
  MIPSII SDK evidence.
- `SetWindowTextW` updates guest window titles and republishes the shared
  guest-window registry.
- Cross-process guest window discovery/message routing exists for child
  emulator processes.
- `happyway_win.exe` can now resolve the parent `iNavi` window by title in the
  capture where title propagation was verified.
- `UID1:` has a named JSON-selected `NANDUUID_RETURN` backend for the observed
  NAND UUID IOCTLs.
- `serial_devices.json` supports `serial` and `ioctl_device` entries with
  explicit backends.
- `DEVICES.md` now records current COM1 GPS, SMB380, and YAS526B evidence.

## Device Evidence

- Real device report path: `D:\INAVI_Emulator\report_serial.txt`.
- `COM1:` is `Drivers\BuiltIn\VSP`, `VSP.dll`, GPS NMEA output,
  `9600 8N1`.
- `COM3:` is `Drivers\BuiltIn\Serial3`, `au16550.dll`, UART candidate,
  `9600 8N1`, with no automatic RX observed in the report.
- `UID1:` is `NANDUUID.dll`, a NAND UUID custom stream device.
- `SMB1:` is `SMB380.dll`, an accelerometer stream driver. Disassembly shows
  `I2C2:`/optional `SPI1:` internals and a broad `IOCTL_SMB380_*` surface.
- `MFS1:` is `YAS526B.dll`, a magnetic field/e-compass stream driver.
  Disassembly shows two `I2C2:` handles for XY and Z chip paths and IOCTLs
  around `0xb0000000..0xb0000010`.
- `PIC1:`, `BTN1:`, `LSD1:`, `CAM1:`, and `TWV1:` remain known stream devices
  but are still stubs.

## GPS And Profile Selection

- Real hardware GPS is on `COM1:`. This is confirmed by the real-device report
  and passive NMEA RX sample.
- The current SDMMC/profile data can still make the app select `COM7:`.
- `captures/inavi_autodrive_20260525_091237` confirmed that temporary
  `COM7:` -> host `COM21` opens and reads valid `$GPGGA`/`$GPRMC`/`$GPVTG`
  input when the host feeder is live.
- The COM7 selection is not explained by registry. Current evidence points to
  app data/profile flow:
  - `happyway_win.exe` was launched as
    `iNavi|SDMMC Disk\mapdata|SDMMC Disk\inavidata|11|7|0|1`.
  - Disassembly showed the launcher reading setting key `0xc3`.
  - Runtime diagnostics showed key `0xc3=4` immediately before launch.
  - `values.dat` supplied `0xc3=4`.
  - `iNaviData\config.bin` disk offset `0x80` contained `06 00`, which filled
    the parent app GPS-port table as zero-based port `6`, later `COM7:`.
- A/B external data patches showed:
  - changing `values.dat` key `0xc3` from `4` to `1` changes the launch profile,
  - changing `iNaviData\config.bin` offset `0x80` from `06 00` to `00 00`
    makes the parent select `COM1:`.
- These A/B patches are evidence only. The emulator should not rewrite those
  bytes at runtime.

## Route Search Evidence

- `iSearch.exe` can start in-process and post back to the parent app.
- The parent creates a full-screen `TGNaviDlg` on the route-search path.
- Current route-search stall is narrowed to the app repeatedly polling
  `FindWindowW(NULL, L"MultiTBT")`.
- No corresponding guest `CreateProcessW` for `\TBT\MultiTBT.exe` has been
  observed in the current logs.
- Manual launch experiments for `MultiTBT.exe` are diagnostics only, not an
  emulator fix.

## Known False Leads / Constraints

- Do not explain the COM1/COM7 mismatch through registry. The current evidence
  points at SDMMC profile/config data.
- Do not hardcode process names or app paths as final behavior.
- Do not fake route-search success or paint UI state manually.
- Do not invent sensor values for `SMB1:` or `MFS1:` before guest IOCTL usage
  is captured.

## Current Dirty Worktree Note

The working tree may include documentation refreshes and active emulator fixes
from the current debugging session. Do not revert unrelated user or prior
session edits.
