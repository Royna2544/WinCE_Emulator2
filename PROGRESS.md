# Progress

Last refreshed: 2026-05-25.

## Working

- The main iNavi map renders again after earlier white-map regressions.
- Touch delivery is more responsive than the earlier busy-loop/frozen state, though modal routing is still incomplete.
- `report_serial.txt` from the real device has been imported into the emulator model:
  - `COM1:` is `Drivers\BuiltIn\VSP`, `VSP.dll`, GPS NMEA output, 9600 8N1.
  - `COM3:` is `Drivers\BuiltIn\Serial3`, `au16550.dll`, UART candidate, 9600 8N1, no automatic RX observed.
  - `UID1:`, `PIC1:`, `BTN1:`, `LSD1:`, `MFS1:`, `SMB1:`, `CAM1:`, and `TWV1:` are known stream devices. `UID1:` now has a narrow named handler; the others remain stubs.
- `regs.json` now includes the reported `Drivers\BuiltIn` stream-device keys and values.
- Serial devices are configured through `--serial-map`.
- `serial_devices.json` is the default host/guest device map used by `tools/autodrive_inavi.ps1`.
- `serial_devices.json` now supports the `NANDUUID_RETURN` ioctl backend for
  `UID1:`. It returns a compact 8-digit id for the parent app's
  `0xa00100cc` probe and the host-backed `Device.uid` bytes for the
  `0xa00100d0` read instead of hardcoding behavior by guest device name.
- `--sdmmc-path` now names the host directory backing the guest `\SDMMC Disk`
  contents. The guest mount name is fixed inside the emulator; the old
  `--fs-root` argument has been removed.

## Current Runtime Shape

- `--serial-map <json>` accepts document version `1` only.
- `--sdmmc-path <host_dir>` points at the files visible under guest
  `\SDMMC Disk`.
- Serial devices may use `backend: "win32_com"` or `backend: "stub"`.
- IOCTL-style stream devices support `backend: "stub"` and
  `backend: "NANDUUID_RETURN"`.
- Defaults are inherited from `defaults.baud` and `defaults.mode`, with per-device override support.
- The intended real-device map bridges guest `COM1:` to host `COM21` at 9600
  8N1. The working tree currently has a temporary diagnostic override that maps
  guest `COM7:` to host `COM21`, because this SDMMC dump still selects COM7.
- The default map uses `NANDUUID_RETURN` for guest `UID1:`.

## Known Recent Observations

- Route search still lags or appears to do nothing after confirmation.
- The route helper has shown transient white/transparent host windows, then disappears or stalls.
- Popup audio can occur before the matching UI becomes visible, which suggests window ordering or modal presentation is still wrong.
- Under-layer controls can still receive clicks while a searching/overlay state is visible.
- `captures/inavi_autodrive_20260525_091237` confirmed that temporary
  `COM7:` -> host `COM21` opens successfully and reads valid NMEA from the
  host feeder. The parent app consumed `$GPGGA`/`$GPRMC`/`$GPVTG` and posted
  private GPS/update messages afterward.
- `captures/inavi_autodrive_20260525_092830` exposed missing CE SDK ordinals
  on the GPS path: `coredll #2010` (`__ll_to_d`) and `#26`
  (`SetSystemTime`). Those are now implemented, along with `__ull_to_d` and
  `pow` (`#1051`) after the next GPS/math step exposed it.
- `captures/inavi_autodrive_20260525_094818` no longer hits the prior
  `pc=0`/unaligned crash after GPS/private message dispatch. The fix tracks
  guest window owner threads, queues worker-thread `SendMessageW` calls to
  main-owned UI windows, and resumes guest waits at the saved return PC. That
  run completed the route preset without crashing, but the host feeder returned
  zero serial bytes during the verification window, so GPS status UI behavior
  still needs a live-NMEA retest.
- `captures/inavi_autodrive_20260525_100904` added serial queue diagnostics.
  The app opens guest `COM7:` through host `COM21`, sets mask `0x1`, calls
  `SetupComm(4096,4096)`, then immediately calls `PurgeComm(..., 0x0f)`.
  Later `ClearCommError` reports `cbInQue=0` before every read, and reads
  return `transferred=0`. This supports a dry host endpoint or pre-open burst
  getting purged, not a failed `CreateFileW`/baud setup path.
- The same 09:48 run shows a modal `TGNaviDlg` destination/current-position
  information popup remains in front of the map. Later route-preset taps were
  delivered to that modal, so the route-search result path was not actually
  exercised in that capture.
- COM7 selection is coming from app data/profile flow, not registry. In the
  2026-05-24 harness runs, `iNavi` launched `happyway_win.exe` with
  `iNavi|SDMMC Disk\mapdata|SDMMC Disk\inavidata|11|7|0|1`, then the child
  opened `COM7:`.
- A/B patches of obvious `config.bin` dwords at offsets `0x34`, `0x4c`, and
  `0xb4` did not change the `happyway_win.exe` command line, so those offsets
  are not the direct runtime source of the current `11|7` launch values.
- `DeviceParser.exe` launches before the `happyway_win.exe` command line is
  built. On real hardware this likely participates in selecting the device
  profile/config that should make GPS port `1`; in the emulator run the later
  `happyway_win.exe` command line still contains port `7`.
- Returning the SDMMC `Device.uid` value through `UID1:`/`NANDUUID_RETURN` is
  verified, but it is not sufficient to change the `happyway_win.exe` command
  line; the 2026-05-24 23:27 harness run still launched with `11|7|0|1`.
- Disassembly of `iNavi.exe` shows the `happyway_win.exe` launcher reads
  setting key `0xc3` through `0x1d13c`; when that value is `4`, the launcher
  emits `11|7|0|1`. The parent app also probes `UID1:` with ioctl
  `0xa00100cc`, so both the compact probe and full UID read now need to be
  present before re-testing whether hardware/profile detection changes key
  `0xc3`. In the 2026-05-24 23:27 harness run, only the `0xa00100d0`
  path was observed; the disassembled `0xa00100cc` branch did not execute.
- Runtime diagnostic in the 2026-05-24 23:42 harness run confirmed setting
  key `0xc3` returns value `4` immediately before `happyway_win.exe` launch,
  at the five launcher callsites `0x59a28`, `0x59a50`, `0x59a78`,
  `0x59aa0`, and `0x59ac4`.
- Runtime diagnostic in `captures/inavi_autodrive_20260525_002848` restored
  the original SDMMC data and confirmed no process wrote `values.dat` or
  `iNaviData\config.bin` during startup/search. `iNavi.exe` loads
  `\SDMMC Disk\INavi\res\values.dat` before `DeviceParser.exe`, inserts
  setting key `0xc3` with value `4`, launches `DeviceParser.exe`, then still
  reads `0xc3=4` before launching `happyway_win.exe` as
  `...|11|7|0|1`.
- The same run confirmed the parent app fills GPS-port table slot
  `0x0079233c` from `iNaviData\config.bin`: file offset `0x0c` is read into
  guest buffer `0x007922c8`, and payload offset `0x74` contains bytes
  `06 00`, yielding zero-based port value `6` and later `COM7:`.
- Directly running `DeviceParser.exe` under the emulator showed it probes
  `\SDMMC Disk\*.bat` and `\SDMMC Disk\autorun.inf`; it did not write the
  profile/config files before hitting the emulator's current process-return
  gap at `pc=0`. Current evidence does not support treating `DeviceParser.exe`
  as the active COM profile selector in this dump.
- A/B external data patches remain diagnostic evidence only: changing
  `values.dat` kind `47` key `0xc3` from `4` to `1` changes the
  `happyway_win.exe` launch profile to `...|2|1|0|1`; changing
  `iNaviData\config.bin` disk offset `0x80` from `06 00` to `00 00` makes the
  parent select `COM1:`. The real emulator should not rewrite these bytes at
  runtime; the next decision is whether the SDMMC dump is from the wrong
  device/profile or whether another real probe/config source is still missing.
