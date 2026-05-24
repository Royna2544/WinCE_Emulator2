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
