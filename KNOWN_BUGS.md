# Known Bugs And Theories

## Confirmed Bugs

### Synthetic coredll still incomplete

- Status: Open.
- Evidence: Search found SDK `coredll.lib` import libraries but no real `COREDLL.dll` PE image under the installed Windows CE Tools tree or local iNavi package.
- Effect: Synthetic `coredll.dll` is required unless a real MIPS `COREDLL.dll` is supplied in the command-line search paths.
- Latest result: Synthetic coredll is fail-closed and partially host-backed. Smoke36 maps real SDK/user DLLs, parses resources, creates host-backed icon handles, dispatches the first paint, and stops cleanly when `GetMessageW` blocks on an empty guest queue.
- Constraint: Do not replace app behavior. Keep coredll as a translate layer: bridge host-compatible APIs, copy guest memory where needed, and map guest handles to host handles for opaque objects.

### Message loop blocks without drawing

- Status: Open.
- Evidence: Smoke36 no longer stops at null PC or spins on fabricated `WM_NULL`. It delivers `WM_PAINT`, dispatches guest WNDPROC through MFC, then stops at `GetMessageW` blocking on an empty guest queue with `UC_ERR_OK pc=0x70002ae8 ra=0x500245c8`.
- Interpretation: The app is alive in the message loop, but the emulator still lacks a drawing/paint bridge and a host event pump that can resume from a blocked guest message wait.
- Previous null-PC theory: Confirmed. `GetMessageW -> 0` caused normal app teardown, then the directly entered PE returned to `RA=0`.

## Discarded Theories

### The v2 loader must copy SDK DLLs into `runtime_dlls`

- Status: Discarded.
- Reason: The loader now discovers DLLs from the primary EXE directory and caller-provided search directories, then maps those real PE files. Runtime copies are optional evidence only, not the primary workflow.

### Icon and image loading caused the current launch stop

- Status: Discarded.
- Reason: Smoke36 returns mapped host icon handles from `LoadIconW(128)` and `LoadImageW(128, IMAGE_ICON, 16, ...)`, but the app still stops later only because `GetMessageW` correctly blocks on an empty guest queue.
