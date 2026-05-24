# Progress

Last refreshed: 2026-05-24.

## Working

- The main iNavi map renders again after earlier white-map regressions.
- Touch delivery is more responsive than the earlier busy-loop/frozen state, though modal routing is still incomplete.
- `report_serial.txt` from the real device has been imported into the emulator model:
  - `COM1:` is `Drivers\BuiltIn\VSP`, `VSP.dll`, GPS NMEA output, 9600 8N1.
  - `COM3:` is `Drivers\BuiltIn\Serial3`, `au16550.dll`, UART candidate, 9600 8N1, no automatic RX observed.
  - `UID1:`, `PIC1:`, `BTN1:`, `LSD1:`, `MFS1:`, `SMB1:`, `CAM1:`, and `TWV1:` are known stream devices and are currently modeled as stubs.
- `regs.json` now includes the reported `Drivers\BuiltIn` stream-device keys and values.
- Serial devices are configured through `--serial-map`.
- `serial_devices.json` is the default host/guest device map used by `tools/autodrive_inavi.ps1`.
- `--sdmmc-path` now names the host directory backing the guest `\SDMMC Disk`
  contents. The guest mount name is fixed inside the emulator; the old
  `--fs-root` argument has been removed.

## Current Runtime Shape

- `--serial-map <json>` accepts document version `1` only.
- `--sdmmc-path <host_dir>` points at the files visible under guest
  `\SDMMC Disk`.
- Serial devices may use `backend: "win32_com"` or `backend: "stub"`.
- IOCTL-style stream devices currently support `backend: "stub"` only.
- Defaults are inherited from `defaults.baud` and `defaults.mode`, with per-device override support.
- The default map bridges guest `COM1:` to host `COM21` at 9600 8N1.

## Known Recent Observations

- Route search still lags or appears to do nothing after confirmation.
- The route helper has shown transient white/transparent host windows, then disappears or stalls.
- Popup audio can occur before the matching UI becomes visible, which suggests window ordering or modal presentation is still wrong.
- Under-layer controls can still receive clicks while a searching/overlay state is visible.
- COM7 selection is coming from app data/config flow, not registry. In the
  2026-05-24 harness run, `iNavi` launched `happyway_win.exe` with
  `iNavi|SDMMC Disk\mapdata|SDMMC Disk\inavidata|11|7|0|1`, then the child
  opened `COM7:`. The same `7` is present in `iNaviData\config.bin` as a
  little-endian dword at offset `0x34`.
- `DeviceParser.exe` launches before the `happyway_win.exe` command line is
  built. On real hardware this likely participates in selecting the device
  profile/config that should make GPS port `1`; in the emulator run the later
  `happyway_win.exe` command line still contains port `7`.
