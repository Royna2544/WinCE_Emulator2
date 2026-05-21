# Known Bugs And Theories

## Confirmed Bugs

### Synthetic coredll still incomplete

- Status: Open.
- Evidence: Search found SDK `coredll.lib` import libraries but no real `COREDLL.dll` PE image under the installed Windows CE Tools tree or local iNavi package.
- Effect: Synthetic `coredll.dll` is required unless a real MIPS `COREDLL.dll` is supplied in the command-line search paths.
- Latest result: Synthetic coredll is fail-closed and partially host-backed. Smoke71 for `INavi.exe` maps real SDK/user DLLs, parses resources, creates host-backed icon/menu/socket/COM-adjacent handles where applicable, creates a host framebuffer presenter for the guest top-level HWND, has no unsupported synthetic calls before idle, and stops cleanly when `GetMessageW` blocks on an empty guest queue. iSearch smoke05 also launches to idle, but still reports unresolved COREDLL ordinal `#1875` once during CRT startup.
- Constraint: Do not replace app behavior. Keep coredll as a translate layer: bridge host-compatible APIs, copy guest memory where needed, and map guest handles to host handles for opaque objects.

### iSearch unresolved COREDLL ordinal `#1875`

- Status: Open.
- Evidence: `v2_synth_isearch_smoke05_final.log` shows `synthetic coredll.dll!#1875 call 1 a0=0x00010000 a1=0x00000000 a2=0x00000001 a3=0x00000001 ra=0x0004fa14`, then the app continues to create the `iSearch` window and reaches idle. Local SDK searches did not find a MIPSII `coredll.lib` import object name for ordinal 1875.
- Effect: It is currently a warning, not a launch blocker.
- Constraint: Do not assign a guessed name or behavior until confirmed by SDK/export evidence or disassembly.

### CE_MANAGER missing `WININET.dll`

- Status: Open.
- Evidence: `v2_synth_cemanager_smoke01.log` maps `C:\Users\royna\Downloads\INAVI\CE_MANAGER\CE_Manager.exe`, parses 20 resources, then fails before `starting Unicorn` with `required DLL not found: WININET.dll`. A local search found only Standard SDK `wininet.lib/.exp` import libraries, not a real MIPS `WININET.dll` PE.
- Effect: `CE_Manager.exe` cannot reach guest code with the current available DLL set.
- Constraint: Keep fail-fast DLL binding. Do not add a fake `WININET.dll` just to pass import binding unless explicitly requested.

### App reports unsupported Korean language

- Status: Superseded by the current HWInfoDB failure.
- Evidence: Smoke48 logs `MessageBoxW caption="INavi" text="This system does not support Korean language.` before the app enters the idle message loop.
- Interpretation: The app is alive, but some real CE locale/NLS/resource/registry input is still wrong or incomplete. Investigate by tracing the app's language check path and implementing the underlying CE APIs or registry data, not by suppressing the message box.

### App reports `can't read HWInfoDB`

- Status: Open.
- Evidence: Smoke71 logs successful external-registry reads for diagnostic `ModelID`, `DeviceName`, and `SystemParametersInfoW(0x102)` values, then opens and reads `C:\Users\royna\Downloads\INAVI\INavi\res\values.dat` before `MessageBoxW caption="INavi" text="can't read HWInfoDB"`. Assembly diagnostic smoke `v2_synth_inavi_hwinfo_asm_diag.log` shows `0x000594a4` stores HWInfo id `0`, subId `0`, and an empty name; `0x00059764` then calls the DB lookup with that empty object; `0x0006bd18` scans all 118 records in `values.dat` for requested id `0` and misses.
- Interpretation: The previous stale ordinal bug (`COREDLL #89` mislabeled as `wcslen`) is fixed; `#89` is SDK-confirmed `SystemParametersInfoW`. The remaining abort is before `values.dat` record lookup: the profile matcher path `0x129204 -> 0x299544` rejects the current diagnostic identity and leaves the selected HWInfo id at zero. Do not hardcode accepted model strings in `.cpp`; use the external registry dump and targeted parser diagnostics.

### Message loop blocks without a host event pump

- Status: Open.
- Evidence: Smoke52 no longer stops at null PC or spins on fabricated `WM_NULL`. It dispatches guest window messages through MFC, the app hides and destroys its main HWND after the Korean-language warning, then stops at `GetMessageW` blocking on an empty guest queue with `UC_ERR_OK pc=0x70002ae8 ra=0x500245c8`. GUI smoke52 enters a host presenter message loop after the guest stop. Later clean smoke `v2_synth_inavi_destroy_sync_clean_500m.log` also exits with `UC_ERR_OK` after serial-failure dialog teardown instead of crashing during MFC destroy.
- Interpretation: The host presenter can stay open for inspection, but host input is not yet translated back into guest messages and the emulator cannot resume the blocked guest `GetMessageW` from host events.
- Previous null-PC theory: Confirmed. `GetMessageW -> 0` caused normal app teardown, then the directly entered PE returned to `RA=0`.
- Previous MFC destroy crash: Fixed by dispatching `DestroyWindow` messages synchronously and using the current WNDPROC for `WM_NCDESTROY`. The old failure was `unmapped memory addr=0 pc=0x50024e8c` after an asynchronously queued destroy reached a torn-down MFC window object.

### GPS serial port not connected by default

- Status: Open.
- Evidence: `v2_synth_inavi_destroy_sync_clean_500m.log` shows the app opening guest device `COM7:`, then calling `GetCommState`, `SetCommState`, `SetCommTimeouts`, and `SetCommMask` on a disconnected guest serial handle. The app then follows its serial-port failure dialog path.
- Effect: iNavi can launch and tear down the failure UI cleanly, but cannot proceed into live GPS behavior without a host serial bridge.
- Constraint: Do not fabricate GPS data in the emulator. When a host virtual COM producer exists, bridge the guest COM handle to `--gps-comm` and let guest reads consume the host stream.

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
