# Known Bugs

Last refreshed: 2026-05-25.

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

## Worker Thread SendMessage Routing Was Crashing GPS Update Path

The GPS worker path reached a synchronous `SendMessageW` to the main UI window
and previously entered the UI wndproc on the worker thread. That exposed a
`pc=0` / unaligned-read crash after `msg=0x577d`.

Status: partially fixed. Guest windows now track owner thread, cross-thread
`SendMessageW` to main-owned windows is queued/yielded, and resumed guest waits
restore the saved return PC. `captures/inavi_autodrive_20260525_094818`
completed without the prior crash, but this needs more live-input testing.

## Stub Device Behavior Is Incomplete

Known real stream devices are now present as stubs or named JSON-selected
handlers, but most device-specific `DeviceIoControl` behavior is not
implemented yet.

Affected devices:
- `UID1:` NAND UUID now has a narrow `NANDUUID_RETURN` backend for ioctls
  `0xa00100cc` and `0xa00100d0`, but any other UID1 ioctl remains unsupported,
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
- A/B patches of obvious `config.bin` candidate dwords at `0x34`, `0x4c`, and
  `0xb4` did not change the launch command, so those offsets are not the
  direct runtime source of the current `11|7` values.
- `DeviceParser.exe` runs earlier in startup, before the `happyway_win.exe`
  command line is built, so the emulator may be missing the hardware/profile
  detection path that should produce GPS port `1`.
- Disassembly shows `iNavi.exe` reads setting key `0xc3`; value `4` maps to
  `happyway_win.exe ...|11|7|0|1`.
- Runtime diagnostic confirms `0xc3` returns `4` at the launcher callsites
  immediately before the `happyway_win.exe` `11|7|0|1` command line is built.
- `captures/inavi_autodrive_20260525_002848` confirms the original SDMMC data
  already contains the bad selection: `values.dat` kind `47` key `0xc3` loads
  as value `4`, and `iNaviData\config.bin` disk offset `0x80` contains
  `06 00`, which fills the parent app's GPS-port table slot as zero-based port
  `6`.
- The same run saw no `WriteFile` to `values.dat` or `iNaviData\config.bin`.
  `DeviceParser.exe` launches after `values.dat` has already supplied
  `0xc3=4`, and direct execution only showed `.bat`/`autorun.inf` probing
  before the emulator hit a process-return gap.
- `UID1:` with backend `NANDUUID_RETURN` successfully returns the SDMMC
  `Device.uid` value (`022794806836GN`) for ioctl `0xa00100d0`. The parent
  app also probes `UID1:` with ioctl `0xa00100cc`; that path is now implemented
  but was not exercised in the 2026-05-24 23:27 harness run.

Registry-based explanations are rejected. Next fix should come from correcting
the SDMMC config/profile data or finding the real device-profile source that
should override this value.
