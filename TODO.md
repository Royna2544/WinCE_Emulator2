# TODO

Current priority order after the 2026-05-24 serial/device cleanup.

1. Route-search UI/process behavior
   - Verify the real `CreateProcessW` path for route helper processes.
   - Replace any remaining placeholder child-process behavior with faithful guest process execution or a clearly failing unsupported path.
   - Confirm the full-screen "searching route" UI is raised before long helper work begins.

2. Popup and modal ordering
   - Ensure topmost/modal guest windows receive touch first.
   - Prevent clicks from reaching underlays while overlays or route-search windows are active.
   - Keep the bottom bar and right-side controls in their guest z-order without host-layer leaks.

3. Serial and stream-device follow-up
   - Test `serial_devices.json` with a host NMEA feeder on `COM21`.
   - Capture logs for `CreateFileW("COM1:")`, `GetCommState`, `SetCommState`, and `ReadFile` to confirm NMEA reaches the app.
   - Add real handlers only when a device protocol is understood; stubs must remain honest no-op devices.

4. Performance work
   - Profile route search, file reads, and redraw frequency before adding more threads.
   - Keep host serial reads nonblocking so missing GPS data cannot freeze the UI.
   - Revisit expensive software floating-point paths after correctness stabilizes.

5. Build hygiene
   - Keep `--serial-map` documented in scripts and examples.
   - Keep logs specific enough to diagnose routing without flooding every frame.
