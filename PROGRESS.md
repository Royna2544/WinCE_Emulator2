# Progress

Last refreshed: 2026-05-28.

## Current State

- The main iNavi map renders again.
- Touch input and route-result transitions are more responsive than the
  earlier frozen/busy-loop state, but modal and overlay input routing is still
  incomplete.
- Route search can advance into route-result/map states with a headless
  `MultiTBT.exe` companion, but full route completion/guidance is still under
  investigation.
- In the current diagnostic route runs, a companion `MultiTBT.exe` joined the
  same shared guest-window registry and the parent resolved
  `FindWindowW(NULL, L"MultiTBT")`.
- The emulator is using real-ish CE API and device boundaries rather than
  hardcoded app behavior.
- The host presenter can upscale the unchanged guest framebuffer to a 4K host
  client area through a Direct3D 11 NVIDIA Image Scaling path, with GDI fallback,
  aspect-preserving letterboxing, and inverse mouse-coordinate mapping back to
  guest framebuffer pixels.
- `README.md`, `DEVICES.md`, `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md`
  have been refreshed to match the current investigation state.
- Source organization is split further: host-backed audio lives in
  `src/coredll_host_audio.cpp`, shared mappings in `src/coredll_mapping.cpp`,
  registry backing/helpers in `src/coredll_registry.cpp`, bitmap/DC drawing in
  `src/coredll_bitmap.cpp`, resource parsing/loading in `src/coredll_res.cpp`,
  guest string helpers in `src/coredll_strings.cpp`,
  guest allocation/memory helpers in `src/coredll_memory_runtime.cpp`,
  host presenter/window runtime in `src/coredll_window_runtime.cpp`,
  cooperative thread scheduling in `src/coredll_thread_runtime.cpp`, and
  synthetic module/export setup in `src/synthetic_dll_modules.cpp`.

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

- Release bitmap drawing now avoids the previous per-pixel window ownership,
  descendant, z-order, and full-screen popup checks on the hot
  `BitBlt -> bitBltToFramebuffer -> writeFramebufferTargetPixel` path.
  `bitBltToFramebuffer` precomputes overlapping backing layers once per blit,
  synthetic dispatch stores resolved ordinal member-function handlers in
  export entries, and Release builds now target `/arch:AVX512` for the
  project-standard Zen5 host.
- The next route/profile sample moved the hot bitmap path into
  `bitBltToBitmap` 16-bit pixel decode helpers. The common CE RGB565/RGB555
  masks now use direct encode/decode paths instead of the generic masked
  helpers, and `SRCCOPY` bitmap-to-bitmap blits no longer decode the
  destination pixel.
- `bitBltToBitmap` now has an AVX2 row fast path for 1:1 `SRCCOPY`
  conversion from RGB565 source bitmaps into 32-bit BGRA destination bitmaps.
  Debug builds also request `/arch:AVX2`, while Release remains `/arch:AVX512`.
- RGB565 expansion now uses bit replication for scalar and SIMD paths, avoiding
  float math in the AVX2 converter. `bitBltToFramebuffer` also has a guarded
  AVX2 RGB565-to-framebuffer fast path for uncluttered 1:1 `SRCCOPY` rows.
- `captures/inavi_autodrive_20260528_133142` verified the Debug SIMD build
  still produces a valid startup frame. The known `DeviceParser.exe` child
  `PC == 0` remains separate from the parent render path.
- The cross-process guest message queue poller now caches the queue path and
  skips JSON parsing/rewrite work when the shared message file's timestamp and
  size have not changed, with a small bounded poll interval to reduce host-loop
  overhead without app-specific behavior.
- Synthetic dispatch now keeps registered ordinal handlers on the direct
  member-function pointer path, caches any late ordinal-handler lookup on the
  export entry, and avoids copying export names on every call.
- Registered synthetic handlers now also have an early dispatch fast path that
  bypasses the large coredll window/message slow path unless the ordinal is one
  of the scheduler or synchronous window/message cases that must stay inline.
- Hot guest path normalization now trims leading slashes with a single prefix
  skip instead of repeatedly erasing the first character during dispatch-time
  path translation.
- `coredll.dll` ordinals `0x0587`/`0x0588` are implemented as `_strlwr` and
  `_strupr` from CE 4.2 MIPSII SDK evidence. This fixes the observed route
  crash where unsupported `#1416` returned `0` and the guest used that as a
  string pointer.
- `captures/inavi_autodrive_20260528_130053` verified the Debug parent stays
  alive and responsive after the `_strupr` fix: `MultiTBT` resolves, no parent
  `#1416` unsupported warning appears, and no parent interactive crash appears
  through the route-result capture.
- `captures/inavi_autodrive_20260528_132322` verified route/dialog z-order is
  now behaving as expected in Debug after owned-popup backing protection was
  applied to owned-popup framebuffer targets too. The "route method" dialog is
  explicitly destroyed by the guest later, and a live `PrintWindow` capture
  showed the app advanced back to quick search rather than being visually
  trapped behind the old dialog.
- `captures/inavi_autodrive_20260528_120552` showed the route-result UI did
  appear, but the small "Searching route" popup window stayed visible and the
  partial map did not advance to the drawn-route state. The log shows popup
  hwnd `0x00016754` created/painted and no later hide/destroy, while the
  scheduler sits on exactly eight queued guest messages with repeated
  `queued-message` watchdog yields. Eight queued messages now enters the
  backlogged queued-work budget path.
- `captures/inavi_autodrive_20260528_120140` verified the optimized Release
  build launches to a valid non-black initial frame with the project autodrive
  runner.
- Emulator runs now treat final guest `PC == 0` as a fatal
  control-flow/resume bug, not a normal successful exit. The runtime still
  flushes registry/host state before returning failure.
- Interactive guest-thread slices also stop treating `PC == 0` as a successful
  thread exit. They now report a hard error and preserve the crash context for
  follow-up diagnosis.
- `captures/inavi_autodrive_20260528_134015` changed `PC == 0` reporting from
  a generic warning to a hard error with register/caller evidence. The current
  `DeviceParser.exe` child failure reaches zero through a generic MIPS/CE CRT
  exit thunk at `0x00013d64` targeting `0xfffff3fa`, not through an app-specific
  route crash.
- Host presentation deferral now also covers normal queued
  `WM_ERASEBKGND`/`WM_PAINT` dispatch, not only synchronous `UpdateWindow`.
  The GDI host presenter now clears only letterbox bands instead of blanking
  the whole client before each stretch blit, avoiding visible app-to-black
  flashes between valid guest frames.
- `captures/inavi_autodrive_20260528_102413` verified the expanded presenter
  fix in Release: the app window launched, produced a valid non-black initial
  frame, and was kept alive for live review.
- Host presentation is now deferred during the `WM_ERASEBKGND` half of
  synchronous `UpdateWindow` dispatch and released for the paired `WM_PAINT`.
  This keeps long guest erase-to-paint gaps from exposing an intermediate black
  frame on the host presenter without bypassing guest drawing.
- `captures/inavi_autodrive_20260528_084215` verified the paint-order change
  in Debug: startup still showed real guest erase-to-paint gaps of hundreds of
  milliseconds, but the bounded capture produced a valid non-black frame while
  preserving the normal guest `WM_PAINT` path.
- `win32_com` serial device opens now retry short-lived
  `ERROR_ACCESS_DENIED`, `ERROR_FILE_NOT_FOUND`, and `ERROR_PATH_NOT_FOUND`
  failures before falling back to a disconnected guest device stub. This makes
  virtual COM startup/teardown races less likely to drop GPS input for the
  whole run.
- `captures/inavi_autodrive_20260528_083115` verified the clean post-fix
  Debug run: guest `COM7:` opened host `\\.\COM21`, serial setup calls
  succeeded, `ClearCommError` reported queued bytes, and repeated
  `ReadFile host serial` calls delivered `$GPGGA`/`$GPRMC`/`$GPVTG` data into
  the guest buffer.
- `GetMessageW` no longer returns `0` for an empty non-quit queue on the main
  UI context. Empty blocking `GetMessageW` now parks the main context and lets
  runnable guest worker threads execute instead of telling MFC that `WM_QUIT`
  occurred.
- Cooperative guest-thread `Sleep` / wait handling no longer abuses the saved
  MIPS `$ra` register as the scheduler resume PC. The blocked context already
  stores the API return PC in `PC`; wake-up now preserves the guest call-return
  chain and only updates the API return value.
- `coredll #0x0100` is correctly treated as `SetWindowTextW`, based on CE 4.2
  MIPSII SDK evidence.
- `SetWindowTextW` updates guest window titles and republishes the shared
  guest-window registry.
- Cross-process guest window discovery/message routing exists for child
  emulator processes.
- Guest `CreateProcessW` now defaults to launching real child emulator
  processes. The old shared in-runtime EXE launch path is opt-in only through
  `INAVI_EMU_INPROC_CHILD_PROCESS`, because separate CE EXEs need isolated
  process/MFC/CRT state.
- Child emulator processes are launched with `--headless` by the generic
  `CreateProcessW` fallback and use hidden/no-console host startup flags.
- The autodrive diagnostic runner no longer kills companion processes when
  `-KeepAlive` is set. This fixed a false route-search stall where the parent
  stayed alive but `MultiTBT.exe` was terminated by the runner cleanup.
- Cross-process guest-window discovery/posting now skips external windows whose
  host process has already exited, preventing stale registry entries from
  receiving route/private messages.
- `happyway_win.exe` can now resolve the parent `iNavi` window by title in the
  capture where title propagation was verified.
- `UID1:` has a named JSON-selected `NANDUUID_RETURN` backend for the observed
  NAND UUID IOCTLs.
- `serial_devices.json` supports `serial` and `ioctl_device` entries with
  explicit backends.
- `DEVICES.md` now records current COM1 GPS, SMB380, and YAS526B evidence.
- `coredll.dll` ordinal `0x0419` is implemented as `_msize` from CE 4.2 MIPSII
  SDK evidence and returns the tracked guest allocation size.
- The host GUI scheduler now gives backlogged queued-message work a larger
  bounded slice even when pending input or cross-thread `SendMessageW` work is
  present. This reduced route-result backlog lag without faking guest state.
- `captures/inavi_autodrive_20260528_123302` showed the later route crash was
  triggered by the diagnostic runner's stale final `route_method_first` tap:
  `04_direct_route.png` already had the route drawn on the map, then the
  script tapped `(405,296)` on the main map and the guest dereferenced a null
  route/image object at `inavi.exe+0x002219a8`. This is evidence for improving
  generic input/modal fidelity and diagnostic timing, not for adding
  app-specific emulator behavior.
- The host presenter accepts `--host-upscale 4k`/`WxH` for host-only scaled
  display. Guest screen metrics and framebuffer dimensions remain unchanged.
  The D3D11 backend uploads the guest framebuffer, runs chained NVIDIA Image
  Scaling passes when the 4K target exceeds NIS's 2x single-pass range, blits
  the final texture into the host image rectangle, and maps host mouse events
  through that rectangle before queuing them to the guest.
- A D3D/GDI paint-order issue in the NIS presenter path was addressed by
  validating `WM_PAINT` before issuing the D3D present, instead of presenting
  while a paint HDC is live. `INAVI_EMU_DISABLE_D3D_NIS=1` can force the GDI
  fallback for comparison.

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

- `iSearch.exe` now starts as a separate headless child emulator process and
  posts/broadcasts back through the cross-process guest-window registry.
- The parent creates a full-screen `TGNaviDlg` on the route-search path.
- Without a companion process, route search stalls on repeated
  `FindWindowW(NULL, L"MultiTBT")`.
- In `captures/inavi_autodrive_20260525_164010`, manually starting
  `\SDMMC Disk\TBT\MultiTBT.exe` with the parent
  `INAVI_EMU_WINDOW_REGISTRY` made the parent import the external
  `MultiTBT` guest window.
- `captures/inavi_autodrive_20260525_173452` confirmed the same with the
  generic autodrive companion launcher: a headless `MultiTBT.exe` created a
  `#32770` window titled `MultiTBT`, the parent resolved it, and the route UI
  advanced past the previous discovery stall.
- `captures/inavi_autodrive_20260525_173931` showed that after the final
  search tap the app created additional route/result child windows, but the
  captured UI was still in a destination/current-position information dialog.
- `captures/inavi_autodrive_20260525_191308` proved a bad emulator
  `GetMessageW` semantic: an empty queue returned `0`, the MFC pump interpreted
  that as quit, and `INavi.exe` entered CRT cleanup before crashing at `pc=0`.
- `captures/inavi_autodrive_20260525_191809` verified the `GetMessageW` fix
  and the guest-thread `$ra` preservation fix. GPS worker reads and
  cross-thread sends continued past the previous `pc=0` point. The route path
  returned to the known `FindWindowW(NULL, L"MultiTBT") -> 0` stall.
- `coredll #1049` no longer reports unsupported on this path after the
  `_msize` fix.
- `captures/inavi_autodrive_20260525_170736` showed the old shared in-process
  `iSearch.exe` path crashing at `pc=0` after a synchronous message returned.
  The raw return address was inside `iSearch.exe`, not MFC.
- `captures/inavi_autodrive_20260525_171500` confirmed that default separate
  child launch avoids that `iSearch` crash. The run returned to the known
  `MultiTBT` discovery stall instead.
- Interactive crash diagnostics now use each PE module's real `SizeOfImage`
  when resolving `pc`/`ra`, so adjacent child/DLL ranges are not misidentified.
- No corresponding guest `CreateProcessW` for `\TBT\MultiTBT.exe` has been
  observed in the current logs.
- Manual launch experiments for `MultiTBT.exe` are diagnostics only, not an
  emulator fix.
- `captures/inavi_autodrive_20260525_200812` identified a diagnostic-runner
  bug: `MultiTBT` PID `12588` was killed by `autodrive_inavi.ps1` even under
  `-KeepAlive`, while its shared-window registry entry remained. Route/private
  messages such as `0x06ee` then queued to the dead PID.
- `captures/inavi_autodrive_20260525_201627` is the fresh post-fix run with
  parent PID `35580` and companion PID `36440` both alive/responding.
- `captures/inavi_autodrive_20260526_073116` identified the new route re-search
  lag source: pending input anywhere in a large queue capped queued-message
  execution to tiny slices while the route UI backlog drained.
- `captures/inavi_autodrive_20260526_073822` visually confirmed the larger
  backlogged-queue slice improves the route screen transition in Release.
- `captures/inavi_autodrive_20260526_074154` showed the route/result
  `SendMessageW(0x57cc)` path does run and creates route child windows; later
  `PostMessageW(0x57ed)` arrived during the re-search flow.
- `captures/inavi_autodrive_20260526_074755` showed the old
  `route_method_first` coordinate hit above the real first route-method button
  when the "existing route" modal was visible.
- `captures/inavi_autodrive_20260526_075134` verified the updated route preset:
  after the `(405,296)` tap, `SendMessageW(0x57cc)` completed and the UI
  advanced into the route-result/map control view.

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
