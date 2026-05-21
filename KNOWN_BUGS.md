# Known Bugs And Theories

## Confirmed Bugs

### Synthetic coredll still incomplete

- Status: Open.
- Evidence: Search found SDK `coredll.lib` import libraries but no real `COREDLL.dll` PE image under the installed Windows CE Tools tree or local iNavi package.
- Effect: Synthetic `coredll.dll` is required unless a real MIPS `COREDLL.dll` is supplied in the command-line search paths.
- Latest result: Synthetic coredll is fail-closed and partially host-backed. Smoke48 maps real SDK/user DLLs, parses resources, creates host-backed icon/menu/socket/COM-adjacent handles where applicable, dispatches the first paint/message path, has no unsupported synthetic calls before idle, and stops cleanly when `GetMessageW` blocks on an empty guest queue.
- Constraint: Do not replace app behavior. Keep coredll as a translate layer: bridge host-compatible APIs, copy guest memory where needed, and map guest handles to host handles for opaque objects.

### App reports unsupported Korean language

- Status: Open.
- Evidence: Smoke48 logs `MessageBoxW caption="INavi" text="This system does not support Korean language.` before the app enters the idle message loop.
- Interpretation: The app is alive, but some real CE locale/NLS/resource/registry input is still wrong or incomplete. Investigate by tracing the app's language check path and implementing the underlying CE APIs or registry data, not by suppressing the message box.

### Message loop blocks without a host event pump

- Status: Open.
- Evidence: Smoke48 no longer stops at null PC or spins on fabricated `WM_NULL`. It dispatches guest window messages through MFC, then stops at `GetMessageW` blocking on an empty guest queue with `UC_ERR_OK pc=0x70002ae8 ra=0x500245c8`.
- Interpretation: The app is alive in the message loop, but the emulator still lacks a host event pump that can resume from a blocked guest message wait.
- Previous null-PC theory: Confirmed. `GetMessageW -> 0` caused normal app teardown, then the directly entered PE returned to `RA=0`.

### COM bridge is IUnknown-only beyond creation

- Status: Open.
- Evidence: Synthetic `ole32.dll` now calls host COM for `CoCreateInstance` and exposes successful interfaces as guest proxy objects with translated `IUnknown::QueryInterface/AddRef/Release` stubs.
- Limitation: Arbitrary COM interface methods still need per-interface guest vtable entries and dispatch implementations before guest code can safely call them.

## Discarded Theories

### The v2 loader must copy SDK DLLs into `runtime_dlls`

- Status: Discarded.
- Reason: The loader now discovers DLLs from the primary EXE directory and caller-provided search directories, then maps those real PE files. Runtime copies are optional evidence only, not the primary workflow.

### Icon and image loading caused the current launch stop

- Status: Discarded.
- Reason: Smoke36 returns mapped host icon handles from `LoadIconW(128)` and `LoadImageW(128, IMAGE_ICON, 16, ...)`, but the app still stops later only because `GetMessageW` correctly blocks on an empty guest queue.
