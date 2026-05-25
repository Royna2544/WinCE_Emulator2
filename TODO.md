# TODO

Last refreshed: 2026-05-26.

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
   - If the "existing route" modal is visible, use the actual first method
     button area around `(405,296)`. The old `(390,220)` coordinate hits modal
     explanatory text, not the button.

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
   - Re-test with the NMEA feeder continuously writing after guest open,
     because the app calls `PurgeComm(..., 0x0f)` after serial setup.
   - Capture `CreateFileW`, `GetCommState`, `SetCommState`, `ClearCommError`,
     and `ReadFile` evidence for the active guest COM path.
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
