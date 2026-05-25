# TODO

Last refreshed: 2026-05-25.

## Immediate

1. Route-search stall
   - Capture a fresh route-search run with focused logging for
     `CreateProcessW`, `FindWindowW`, `SetWindowTextW`, `ShowWindow`,
     `SetWindowPos`, `SendMessageW`, `PostMessageW`, and `DeviceIoControl`.
   - Determine why the app polls for `MultiTBT` but no guest launch for
     `\TBT\MultiTBT.exe` appears in logs.
   - Wait for or use real-device evidence if it proves `MultiTBT` is a session
     companion.
   - If needed, design a generic session-companion configuration. Do not
     hardcode `MultiTBT.exe`, `iSearch.exe`, `happyway_win.exe`, or app paths in
     emulator logic.
   - Confirm the expected full-screen route-search/searching UI is raised
     before long helper work begins.

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
   - Keep `README.md`, `PROGRESS.md`, `TODO.md`, `KNOWN_BUGS.md`, and
     `DEVICES.md` synchronized after meaningful fixes.
