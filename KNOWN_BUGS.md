# Known Bugs And Theories

## Confirmed Bugs

### Synthetic coredll still incomplete

- Status: Open.
- Evidence: Search found SDK `coredll.lib` import libraries but no real `COREDLL.dll` PE image under the installed Windows CE Tools tree or local iNavi package.
- Effect: Synthetic `coredll.dll` is required unless a real MIPS `COREDLL.dll` is supplied in the command-line search paths.
- Latest result: Synthetic coredll is fail-closed and partially host-backed. Smoke52 maps real SDK/user DLLs, parses resources, creates host-backed icon/menu/socket/COM-adjacent handles where applicable, creates a host framebuffer presenter for the guest top-level HWND, dispatches the first paint/message path, has no unsupported synthetic calls before idle, and stops cleanly when `GetMessageW` blocks on an empty guest queue.
- Constraint: Do not replace app behavior. Keep coredll as a translate layer: bridge host-compatible APIs, copy guest memory where needed, and map guest handles to host handles for opaque objects.

### App reports unsupported Korean language

- Status: Open.
- Evidence: Smoke48 logs `MessageBoxW caption="INavi" text="This system does not support Korean language.` before the app enters the idle message loop.
- Interpretation: The app is alive, but some real CE locale/NLS/resource/registry input is still wrong or incomplete. Investigate by tracing the app's language check path and implementing the underlying CE APIs or registry data, not by suppressing the message box.

### Message loop blocks without a host event pump

- Status: Open.
- Evidence: Smoke52 no longer stops at null PC or spins on fabricated `WM_NULL`. It dispatches guest window messages through MFC, the app hides and destroys its main HWND after the Korean-language warning, then stops at `GetMessageW` blocking on an empty guest queue with `UC_ERR_OK pc=0x70002ae8 ra=0x500245c8`. GUI smoke52 enters a host presenter message loop after the guest stop.
- Interpretation: The host presenter can stay open for inspection, but host input is not yet translated back into guest messages and the emulator cannot resume the blocked guest `GetMessageW` from host events.
- Previous null-PC theory: Confirmed. `GetMessageW -> 0` caused normal app teardown, then the directly entered PE returned to `RA=0`.

### Host presenter is not the full CE GUI yet

- Status: Open.
- Evidence: GUI smoke52 logs `created host presenter HWND=... for guest HWND=0x00010007 800x480`, then `entering host GUI message loop` after the guest destroys the HWND. The retained presenter is an inspection surface over the emulator framebuffer, not a replacement guest window.
- Limitation: Child common controls are guest HWND records, command-bar menus can attach to host `HMENU`, and the framebuffer can be painted by implemented GDI paths, but there is no full host child-control hierarchy or host input-to-guest event translation yet.

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
