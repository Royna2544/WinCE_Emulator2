# TODO

Last refreshed: 2026-05-28.

## Immediate

1. Route-search stall
   - Continue from `captures/inavi_autodrive_20260525_191809`, not the older
     `pc=0` route crash. The empty-queue `GetMessageW` false-quit and
     guest-thread `$ra` corruption are fixed in the current working tree.
   - Continue live route testing from `captures/inavi_autodrive_20260525_201627`.
     The previous `200812` run was invalid for route-search diagnosis because
     the autodrive runner killed the `MultiTBT` companion during `-KeepAlive`.
   - Determine why the app polls for `MultiTBT` but no guest launch for
     `\TBT\MultiTBT.exe` appears in logs.
   - Current diagnostic run may manually start `\SDMMC Disk\TBT\MultiTBT.exe`
     beside `INavi.exe` to test whether the route-search poll is waiting for
     a companion process/window. This is not a final emulator behavior.
   - When manually starting `MultiTBT.exe`, pass the parent run's
     `INAVI_EMU_WINDOW_REGISTRY` and `--headless`; otherwise it publishes to a
     private registry and/or opens an unnecessary host presenter while the
     parent still sees `FindWindowW(NULL, L"MultiTBT") -> 0`.
   - Use the autodrive `-CompanionTarget` option for bounded generic companion
     diagnostics instead of adding emulator app-name special cases.
   - Keep normal `CreateProcessW` children as real separate child emulator
     processes. Use `INAVI_EMU_INPROC_CHILD_PROCESS` only as a diagnostic
     opt-in for shared in-runtime launch.
   - Wait for or use real-device evidence if it proves `MultiTBT` is a session
     companion.
   - If needed, design a generic session-companion configuration. Do not
     hardcode `MultiTBT.exe`, `iSearch.exe`, `happyway_win.exe`, or app paths in
     emulator logic.
   - Continue from `captures/inavi_autodrive_20260526_075134`: with
     `MultiTBT.exe` launched through `-CompanionTarget`, the updated route
     preset reaches the route-result/map control view and `0x57cc` completes.
     Next, determine what remains before full guidance/route completion.
   - Continue from `captures/inavi_autodrive_20260528_120552`: the
     route-result UI is visible, but popup hwnd `0x00016754` remains visible
     after "Searching route" and the map does not redraw with the route line.
     Re-test after the queued-message backlog threshold change; if it still
     stalls, dump the eight queued guest messages and identify which thread is
     expected to re-enable/destroy the modal popup.
   - For diagnostics, only use the `route_method_first` tap when the existing
     route modal is actually visible. Do not add emulator behavior based on
     route-screen state, coordinates, or app-specific strings.
   - Investigate the separate `DeviceParser.exe` `PC == 0` child exit from
     `captures/inavi_autodrive_20260528_130053`; the parent route process now
     survives the `_strupr` crash point, but the child control-flow bug should
     still be explained under the project `PC == 0` rule.
   - Continue `DeviceParser.exe` from
     `captures/inavi_autodrive_20260528_134015`: `PC == 0` now hard-errors and
     points at a generic MIPS/CE CRT exit thunk
     (`addiu t0,zero,-3078; jalr ra,t0`) after the `autorun.inf` miss. Add a
     generic CE kernel-call/termination handler; do not special-case
     `DeviceParser.exe`.
   - If any interactive guest-thread slice reaches `PC == 0`, investigate it
     as a control-flow/kernel-call issue. The runtime no longer converts null
     PC into a synthetic successful thread exit.
   - Continue from `captures/inavi_autodrive_20260528_132322`: route/dialog
     ordering is expected after the owned-popup backing fix, and the live app
     advanced to quick search. The remaining route work is now later guidance
     handoff/state progress, not that dialog being visually covered.
   - Continue from `captures/inavi_autodrive_20260528_185552`: the current
     Debug run includes generic `atan2` (`coredll #992`) and `Ellipse`
     (`coredll #934`) implementations after the previous re-search null-deref
     at `inavi.exe+0x002219a8`. Manually exercise re-search and confirm those
     unsupported ordinal warnings and that crash signature are gone before
     moving on to remaining route-completion waits.

2. Modal and overlay input
   - Make topmost/modal guest windows receive touch first.
   - Prevent under-layer controls from receiving clicks while a popup,
     safety screen, or searching overlay is active.
   - Keep bottom bar/right-side controls in correct guest z-order after UI
     transitions.
   - Diagnose popup-audio-without-popup as a window activation/paint/message
     ordering issue, not an audio issue.

3. GPS retest
   - Keep using the temporary `COM7:` -> host feeder mapping only while this
     SDMMC dump selects COM7.
   - `captures/inavi_autodrive_20260528_083115` confirmed the host serial
     bridge can open `COM7:` -> `\\.\COM21` and deliver NMEA after stale
     emulator processes are stopped. Continue watching whether the app consumes
     the stream correctly during live UI/input testing.
   - After the profile/config source is settled, restore the intended real map:
     guest `COM1:` -> host NMEA feeder at `9600 8N1`.

## Next

4. COM profile source
   - Continue from the confirmed app-data/profile evidence:
     `values.dat` key `0xc3=4` and `iNaviData\config.bin` offset `0x80=06 00`.
   - Decide whether the current SDMMC dump is from the wrong device/profile or
     whether a still-missing hardware/config probe should rewrite/select those
     files before launch.
   - Keep A/B byte patches as evidence only. Do not add runtime byte rewriting
     to the emulator.

5. Stream-device tracing
   - Use `DEVICES.md` as the current evidence index.
   - Add bounded `DeviceIoControl` tracing for `SMB1:` and `MFS1:`:
     control code, input size, output size, return value, transferred count,
     and small buffer previews.
   - Implement sensor handlers only after real guest callsites are observed.
   - Keep `SMB1:` and `MFS1:` as ioctl/stream devices, not serial ports.
   - Leave stubs honest: unsupported device behavior should fail/no-op
     consistently instead of inventing app-specific state.

6. Performance
   - Re-profile the optimized Release build after
     `captures/inavi_autodrive_20260528_120140`. The previous Visual Studio
     profile showed most sampled time under
     `bitBltToFramebuffer -> writeFramebufferTargetPixel`, with repeated
     popup/window/z-order checks and tree iterator overhead; that blit path now
     precomputes backing layers once per call.
   - Re-profile after the 16-bit bitmap fast paths. The next profile showed
     `bitBltToBitmap -> decodeBitmap16 -> decodeMasked16/expandMaskedChannel`;
     normal RGB565/RGB555 bitmap formats should now avoid the generic masked
     helpers, and `SRCCOPY` should avoid destination reads.
   - Re-profile 1:1 RGB565-to-32-bit `SRCCOPY` blits after the AVX2 conversion
     helper. If `bitBltToBitmap` remains hot, inspect whether the remaining
     cases are scaling, transparency, palette, 16-bit destination, or
     non-`SRCCOPY` ROPs before adding more SIMD variants.
   - Re-profile after the new RGB565-to-framebuffer SIMD path. If framebuffer
     blits are still hot, check whether backing layers, scaling, negative blit
     dimensions, or non-`SRCCOPY` ROPs are keeping the path scalar.
   - Re-profile `runHostMessageLoopUntilClosed` after the cross-process queue
     metadata cache. If `pollCrossProcessGuestMessages` remains hot, consider
     replacing the shared JSON queue with a generic append-only or memory-mapped
     transport instead of adding app-specific shortcuts.
   - Re-profile synthetic dispatch after the direct handler/name-copy cleanup.
     If `SyntheticDllRuntime::dispatch` remains high in samples, split out the
     coredll registered-handler fast path from the window/message transfer path
     instead of adding app-specific cases.
   - Continue live input/UI lag testing after
     `captures/inavi_autodrive_20260528_102413`, where Release launched and
     stayed alive after expanding host erase/paint deferral and removing the
     full-client black clear from GDI host presentation.
   - Re-test route-result/re-search lag after the backlogged queued-message
     scheduler change from 2026-05-26. It improves the large initial backlog,
     but medium-sized modal/UI bursts and synchronous wndproc waits may still
     lag.
   - Re-check input lag after the wait/sleep `$ra` preservation fix, because
     GPS worker threads now survive repeated serial read/message cycles instead
     of eventually poisoning the scheduler state.
   - Profile route search, file reads, serial reads, and redraw frequency before
     adding more threads.
   - Keep host serial reads nonblocking.
   - Exercise `--host-upscale 4k` in a live presenter run and compare D3D11/NIS
     frame pacing against the GDI fallback.
   - Revisit heavy software floating-point paths after correctness stabilizes.
   - Avoid broad logging that floods every frame or every tight polling loop.

## Later

7. Device backends
   - Consider an `I2C2:` model if `SMB380.dll`/`YAS526B.dll` behavior becomes
     necessary through real guest calls.
   - Add narrow named handlers for `PIC1:`, `BTN1:`, `LSD1:`, `CAM1:`, or
     `TWV1:` only after their protocol is identified.
   - Keep `UID1:`/`NANDUUID_RETURN` narrow and evidence-based.

8. Cleanup
   - Remove or demote temporary diagnostics once each investigation is settled.
   - Keep CE SDK ordinal evidence near code changes when adding coredll exports.
   - If the `MultiTBT` companion requirement is confirmed, replace the manual
     launch experiment with a generic configured companion-process mechanism.
   - Keep `README.md`, `PROGRESS.md`, `TODO.md`, `KNOWN_BUGS.md`, and
     `DEVICES.md` synchronized after meaningful fixes.
