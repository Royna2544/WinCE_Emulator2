# TODO

Current priority order after the 2026-05-25 window-title and route-helper
diagnostics.

1. Route-search UI/process behavior
   - Wait for the real-device dump that may identify how `\TBT\MultiTBT.exe`
     is launched or registered. Current logs show the app polling for a
     top-level `MultiTBT` window but do not show a guest `CreateProcessW` for
     it.
   - If the real dump proves `MultiTBT` is a session companion, add a generic
     external session-companion manifest/argument. Do not hardcode
     `MultiTBT.exe`, app names, or paths in emulator logic.
   - Keep the manual `MultiTBT` launch as diagnostic evidence only; remove any
     temporary launch scripts/log assumptions before committing a real fix.
   - Confirm the full-screen "searching route" UI is raised before long helper work begins.

2. Popup and modal ordering
   - Ensure topmost/modal guest windows receive touch first.
   - Prevent clicks from reaching underlays while overlays or route-search windows are active.
   - Keep the bottom bar and right-side controls in their guest z-order without host-layer leaks.

3. Serial and stream-device follow-up
   - Re-test `serial_devices.json` with a live host NMEA feeder on `COM21`
     after the `__ll_to_d`, `SetSystemTime`, `pow`, cross-thread
     `SendMessageW`, and wait-resume fixes. The 09:48 verification run opened
     COM7 successfully but read zero bytes, so GPS status UI behavior was
     inconclusive.
   - During GPS retests, keep the NMEA feeder continuously writing after the
     emulator opens the port. The guest calls `PurgeComm(..., 0x0f)` after
     setup, so pre-open bursts can be cleared before the first `ReadFile`.
   - Use `tools/autodrive_inavi.ps1 -NoTaps -KeepAlive` for human-driven
     logging runs so scripted taps do not accidentally drive the wrong modal.
   - Decide whether the serial read cap of 128 bytes should remain as a
     faithful UART-style behavior or become a configurable diagnostic limit.
   - Capture logs for `CreateFileW("COM1:")`, `GetCommState`, `SetCommState`, and `ReadFile` to confirm NMEA reaches the app.
   - Continue tracing the device-profile source that makes `iNavi.exe` choose
     profile code `4`, which produces `happyway_win.exe ...|11|7|0|1`.
     Disassembly shows this comes from setting key `0xc3`; the full UID ioctl
     `0xa00100d0` succeeds, and the compact ioctl `0xa00100cc` is now
     implemented but was not exercised in the latest harness run.
   - Decide the non-fake COM1 path from evidence: either correct the external
     SDMMC profile/config data to the real-device COM1 profile, or identify the
     missing real hardware/config probe that should rewrite/select those files.
     Do not add runtime byte rewriting in the emulator.
   - Temporary diagnostic hook: `main.cpp` logs entry/return of the
     `iNavi.exe` key `0xc3` getter at `0x1d13c..0x1d170`, plus config load
     entry at `0x6bd18` and key `0xc3` insertions at `0x6c1a8`. Remove it
     after the profile source is identified.
   - Temporary diagnostic file trace: `coredll_fs.cpp` logs opens/reads/writes
     for `INavi\res\values.dat` and `iNaviData\config.bin`, including caller
     RA and file offsets. Remove or demote after the COM profile source is
     settled.
   - Temporary diagnostic serial mapping: `serial_devices.json` maps guest
     `COM7:` to host `COM21` while this dump selects COM7. Restore the real
     `COM1:` map after the profile/config source is settled.
   - Add real handlers only when a device protocol is understood; stubs must remain honest no-op devices.

4. Performance work
   - Profile route search, file reads, and redraw frequency before adding more threads.
   - Keep host serial reads nonblocking so missing GPS data cannot freeze the UI.
   - Revisit expensive software floating-point paths after correctness stabilizes.

5. Build hygiene
   - Keep `--serial-map` documented in scripts and examples.
   - Keep `--sdmmc-path` documented as the host directory backing guest `\SDMMC Disk`.
   - Keep logs specific enough to diagnose routing without flooding every frame.
   - Keep SDK ordinal evidence in code comments or progress notes when adding
     new coredll handlers. The `SetWindowTextW` fix was based on the CE 4.2
     MIPSII SDK import library, not a guessed ordinal.
