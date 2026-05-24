# Known Bugs

Last refreshed: 2026-05-24.

## Route Search Does Not Complete

Pressing the route-search path can show delayed popups or transient helper windows, then stall without presenting the expected full-screen route-search/result UI.

Likely areas:
- `CreateProcessW` guest child process handling.
- Modal/window z-order routing.
- Blocking helper/file/serial work on the UI thread.

## Modal And Overlay Routing Is Still Wrong

Top-level or full-screen overlays do not always own input. Underlying buttons can still receive touch when an overlay says the app is searching or waiting.

Likely areas:
- hit-test target selection,
- topmost/modal guest window tracking,
- host presenter ownership for child/full-screen windows.

## Popup UI Can Lag Behind Audio

The app can play the confirmation/error sound without the popup becoming visible immediately. Sometimes the popup appears only after other windows are dismissed.

Likely areas:
- synchronous message delivery,
- `ShowWindow`/activation ordering,
- paint invalidation timing.

## Bottom Bar Can Disappear After Search/Overlay Flows

The map and right-side controls can remain visible while the bottom bar disappears or is hidden behind another guest surface.

Likely areas:
- z-order composition,
- child window clipping,
- stale presenter surface after modal transitions.

## Stub Device Behavior Is Incomplete

Known real stream devices are now present as stubs, but device-specific `DeviceIoControl` behavior is not implemented yet.

Affected devices:
- `UID1:` NAND UUID,
- `PIC1:` MCU/PIC candidate,
- `BTN1:` buttons,
- `LSD1:` light sensor,
- `MFS1:` e-compass candidate,
- `SMB1:` accelerometer candidate,
- `CAM1:` camera,
- `TWV1:` video decoder/input candidate.

## GPS Port Selection Still Chooses COM7

The real device report identifies GPS NMEA output as `COM1:` via `VSP.dll`, but
the current app run can still open `COM7:`.

Evidence:
- Runtime launched `happyway_win.exe` with command line
  `iNavi|SDMMC Disk\mapdata|SDMMC Disk\inavidata|11|7|0|1`.
- The child opened `COM7:` immediately afterward.
- `iNaviData\config.bin` contains the same GPS port value `7` as a dword at
  offset `0x34`.
- `DeviceParser.exe` runs earlier in startup, before the `happyway_win.exe`
  command line is built, so the emulator may be missing the hardware/profile
  detection path that should produce GPS port `1`.

Registry-based explanations are rejected. Next fix should come from correcting
the SDMMC config/profile data or finding the real device-profile source that
should override this value.
