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

## Current Runtime Shape

- `--serial-map <json>` accepts document version `1` only.
- Serial devices may use `backend: "win32_com"` or `backend: "stub"`.
- IOCTL-style stream devices currently support `backend: "stub"` only.
- Defaults are inherited from `defaults.baud` and `defaults.mode`, with per-device override support.
- The default map bridges guest `COM1:` to host `COM21` at 9600 8N1.

## Known Recent Observations

- Route search still lags or appears to do nothing after confirmation.
- The route helper has shown transient white/transparent host windows, then disappears or stalls.
- Popup audio can occur before the matching UI becomes visible, which suggests window ordering or modal presentation is still wrong.
- Under-layer controls can still receive clicks while a searching/overlay state is visible.
