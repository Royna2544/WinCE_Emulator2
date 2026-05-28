# Known Bugs

Last refreshed: 2026-05-28.

## Route Search Does Not Complete

Symptom:

- Pressing route search can show delayed dialogs or transient helper windows,
  then stalls without the expected full-screen route-search/result flow.

Current evidence:

- `iSearch.exe` can start and post back to the parent app.
- `captures/inavi_autodrive_20260525_171500` shows `iSearch.exe` launching as
  a separate headless child emulator process and receiving cross-process guest
  messages.
- The parent app creates a full-screen `TGNaviDlg`.
- Logs show repeated `FindWindowW(NULL, L"MultiTBT") -> 0`.
- If `MultiTBT.exe` is manually started with the parent
  `INAVI_EMU_WINDOW_REGISTRY`, the parent imports the external `MultiTBT`
  guest window and broadcasts/messages reach the helper.
- `captures/inavi_autodrive_20260525_173452` confirms the generic headless
  companion diagnostic resolves `FindWindowW(NULL, L"MultiTBT")`.
- `captures/inavi_autodrive_20260525_200812` showed a false stall caused by
  the diagnostic runner, not the guest app: `MultiTBT` was killed despite
  `-KeepAlive`, leaving stale registry and queued route messages to a dead PID.
- The runner now preserves companions under `-KeepAlive`, and the emulator
  skips stale external guest windows whose host process has exited.
- `captures/inavi_autodrive_20260525_173931` shows route/result child windows
  being created after the final search tap, but the screenshot remains on a
  destination/current-position information dialog.
- `captures/inavi_autodrive_20260526_075134` shows the updated route preset
  reaching the route-result/map control view after the corrected
  `route_method_first` tap at `(405,296)`.
- The old route preset coordinate `(390,220)` was a diagnostic-script bug for
  the "existing route" modal: it hit explanatory text above the first route
  method button.
- `captures/inavi_autodrive_20260525_191308` showed a now-fixed emulator
  regression where empty blocking `GetMessageW` returned `0` and caused MFC to
  enter shutdown/CRT cleanup.
- `captures/inavi_autodrive_20260525_191809` showed no repeat of that route
  `pc=0` crash after fixing `GetMessageW` empty-queue blocking and preserving
  guest `$ra` across cooperative `Sleep`/wait wake-ups.
- No guest `CreateProcessW` for `\TBT\MultiTBT.exe` has been observed in the
  current logs.
- `coredll #1049` was verified as `_msize` in the CE 4.2 MIPSII SDK and is now
  implemented; this removed one unsupported CRT call from the route path.
- The previous shared in-runtime `iSearch.exe` path could crash at `pc=0` after
  a synchronous message returned. Defaulting guest `CreateProcessW` to real
  separate child emulator processes avoids that specific crash in the latest
  bounded run.

Current hypothesis:

- The route stack expects a companion/session window or process that the
  emulator is not starting or discovering.
- The emulator may need a generic companion-process configuration if real-device
  evidence confirms `MultiTBT` is normally launched outside the observed
  `CreateProcessW` path.
- After `MultiTBT` is present and the corrected tap reaches route-result/map
  controls, the remaining lead is the final route-completion/guidance handoff
  plus any paint/z-order issues around modal overlays.
- Re-test from `captures/inavi_autodrive_20260526_075134`, where parent and
  companion are both launched by the runner and the route-result transition is
  observed.

Status:

- Still not complete, but the route `pc=0` crash, diagnostic companion-kill
  false stall, route-result backlog lag, and wrong modal tap coordinate are
  fixed or narrowed in the current working tree. Do not hardcode a `MultiTBT`
  launch. Use real-device evidence or a generic external companion
  configuration if one is justified.

## Modal And Overlay Routing Is Still Wrong

Symptom:

- Under-layer controls can receive clicks while a popup, safety screen, or
  searching overlay is visible.
- Some overlays can remain in front after they should dismiss.
- Bottom bar/right-side controls can disappear or appear in the wrong order
  after transitions.

Current hypothesis:

- Guest z-order, topmost/modal ownership, host presenter activation, and
  hit-test target selection are not yet faithful enough.

Status:

- Partially improved, not fixed.

## Popup UI Can Lag Behind Audio

Symptom:

- The app can play the confirmation/error sound while the matching popup UI is
  delayed, hidden, or only appears after another window is dismissed.
- The host presenter could briefly flash a black frame while the guest was in a
  long erase-to-paint gap or between host GDI presents.

Current hypothesis:

- Audio is not the broken path. The likely problem is synchronous message
  delivery, activation, `ShowWindow`/`SetWindowPos`, or paint invalidation
  ordering.
- The black-frame flash was caused by host presentation during
  `WM_ERASEBKGND` before the paired `WM_PAINT` completed. A second host-side
  contributor was the GDI presenter clearing the whole client black before
  stretching the guest framebuffer.

Status:

- Popup/audio lag is not fixed. The black-frame flash has targeted paint-order
  fixes in `captures/inavi_autodrive_20260528_084215` and
  `captures/inavi_autodrive_20260528_102413`: host presentation is deferred
  across erase/paint pairs, and GDI host repaint clears only letterbox bands.

## GPS Profile Still Selects COM7 In This Dump

Symptom:

- Real hardware report confirms GPS NMEA on `COM1:`, but the current app data
  can still launch/use `COM7:`.

Current evidence:

- Real device `COM1:` is `VSP.dll`, serial API yes, `9600 8N1`, with passive
  NMEA RX.
- Runtime command line has been observed as
  `iNavi|SDMMC Disk\mapdata|SDMMC Disk\inavidata|11|7|0|1`.
- Disassembly/runtime diagnostics show setting key `0xc3=4` before
  `happyway_win.exe` launch.
- `values.dat` supplies `0xc3=4`.
- `iNaviData\config.bin` offset `0x80` contains `06 00`, yielding zero-based
  port `6` and later `COM7:`.
- A/B external data patches can make the app choose `COM1:`, but those patches
  are evidence only.

Rejected explanation:

- Registry is not the current explanation for COM7. The current evidence points
  at app data/profile flow.

Status:

- Not fixed. Temporary `COM7:` host mapping is allowed only for diagnostics.
  `captures/inavi_autodrive_20260528_083115` verified that, when the host port
  is not held by stale emulator processes, `COM7:` -> `\\.\COM21` opens and
  streams NMEA into the guest. The remaining issue is still the profile source
  that selects `COM7:` in this dump instead of the real-device `COM1:` path.

## Custom Stream Devices Are Mostly Stubs

Symptom:

- Real stream devices exist in registry/report but the emulator does not yet
  implement most device-specific `DeviceIoControl` protocols.

Affected devices:

- `UID1:` has a narrow `NANDUUID_RETURN` backend for observed NAND UUID ioctls.
- `SMB1:` / `SMB380.dll` is accelerometer-related and internally I2C/SPI.
- `MFS1:` / `YAS526B.dll` is magnetic/e-compass-related and internally I2C.
- `PIC1:`, `BTN1:`, `LSD1:`, `CAM1:`, and `TWV1:` are known but still stubs.

Status:

- Expected limitation. Add real handlers only after callsite/ioctl evidence is
  captured.

## Cooperative Cross-Thread SendMessage Is Still Risky

Symptom:

- Earlier GPS/private-message paths crashed when worker-thread `SendMessageW`
  entered a main-owned UI wndproc directly.

Current evidence:

- Owner-thread tracking and queued/yielded cross-thread `SendMessageW` reduced
  the previous `pc=0`/unaligned crash.
- Cross-thread `SendMessageW` now blocks the sender until the owner-thread
  wndproc returns, instead of letting the sender continue immediately.
- A separate scheduler bug was fixed: blocked guest-thread waits now preserve
  `$ra`; wake-up resumes from the saved `PC` return slot instead of overwriting
  the guest call-return register.
- The model is still cooperative and may contribute to UI lag or dead waits in
  edge cases.

Status:

- Partially fixed. Keep watching for lag, dead waits, and wrong return timing.

## Performance Is Still Not Representative

Symptom:

- Startup, GPS/map updates, and route search can lag badly compared with real
  hardware expectations.
- Host mouse input can be queued quickly but not delivered until much later.

Likely areas:

- Long guest slices while input is pending.
- Synchronous `SendMessageW`/wndproc work that cannot be interrupted without
  breaking call semantics. Backlogged synchronous queued work now gets a larger
  bounded slice, but this still needs more route/re-search coverage.
- Large startup/route UI message bursts, including child window creation and
  cross-process companion messages.
- Route/file helper work on the UI thread.
- Excessive file I/O or small reads.
- Redraw/present frequency and invalidation behavior.
- Software floating-point overhead for map math.
- Diagnostic logging volume.
- The bitmap hot path has a targeted optimization in the current working tree:
  backing-layer/window checks are now precomputed per blit and synthetic
  ordinal handlers are cached on export entries. This still needs profiler
  comparison on the route/map workload.

Status:

- Not optimized. Correctness and faithful blocking/message behavior remain the
  priority before broad threading changes.
