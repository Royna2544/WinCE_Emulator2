# Progress

## Confirmed Facts

- This v2 checkout now loads target-side Windows CE/MIPS DLLs as PE images instead of binding every import to a local shim page.
- The loader searches the primary EXE directory plus DLL search directories supplied on the command line; it no longer requires a DLL-copy helper script or hardcoded local SDK paths.
- The loader builds in-memory PE images, maps them into Unicorn, applies base relocations, parses exports, and recursively binds imports. Missing DLLs or unresolved exports are fatal.
- Pre-fallback-removal smoke verification loaded 12 PE modules:
  - `inavi.exe`
  - `AuthLibrary.dll`
  - `mMbcAuth.dll`
  - `tpeg_if_dll.dll`
  - `TpSysAuth.dll`
  - `tw_tpeg_if_dll.dll`
  - `mfcce400.dll`
  - `mfcce400d.dll`
  - `mfcce400i.dll`
  - `atlce400.dll`
  - `olece400.dll`
  - `olece400d.dll`
- Earlier smoke verification bound `inavi.exe` imports with `real=266 fallback=370`, proving MFC imports were going to real SDK DLL exports where available before fallback support was removed.
- After fallback removal, the same explicit SDK search-path smoke run fails fast on missing `COREDLL.dll`; no shim page or fallback import target is installed.
- CLI contract is now `iNavi_Unicorn_Emulator.exe <primary.exe> [dll_search_dir ...]`.
- A search of the installed Windows CE 4.2 Standard SDK under `C:\Program Files (x86)\Windows CE Tools` and the local iNavi tree found `coredll.lib` import libraries but no real `COREDLL.dll` PE image.
- Synthetic `coredll.dll` is now fail-closed for unknown coredll exports. Supported exports are named from Windows CE 4.2 Standard SDK MIPSII `coredll.lib` COFF import-object headers, not from `/LINKERMEMBER` guesses.
- Host-compatible coredll APIs are bridged through translated guest arguments where possible. Opaque handles such as files, events, mutexes, HWNDs, menus, accelerators, resources, and wave input are represented as guest handles mapped to host handles or guest-side records.
- `winmm.dll` is loaded dynamically on the host for wave input bridging. Guest `HWAVEIN` values are mapped handles; guest `WAVEHDR` buffers are copied to host-side `WAVEHDR` storage before host calls.
- The main EXE resource table is parsed from PE resources in `SyntheticDllRuntime::setMainModulePath`. Current verified `INavi.exe` resources: 18 entries, including bitmaps, dialogs, one menu (`RT_MENU` id 128), one group icon (`RT_GROUP_ICON` id 128), version, and CEUX metadata. No `RT_STRING` or `RT_ACCELERATOR` resources were present in the parsed table.
- Resource APIs now use parsed EXE resources: `FindResource/FindResourceW`, `LoadResource`, `SizeofResource`, and `LoadStringW` are resource-table backed. `LoadMenuW` creates a host `HMENU` with `LoadMenuIndirectW`; `RemoveMenu`, `CheckMenuItem`, and `CheckMenuRadioItem` operate on mapped host menu handles.
- `LoadIconW` and `LoadImageW(..., IMAGE_ICON, ...)` translate parsed `RT_GROUP_ICON`/`RT_ICON` resources into host `HICON` objects behind guest handles. `LoadImageW(..., IMAGE_BITMAP, ...)` can translate parsed `RT_BITMAP` DIB data into a host `HBITMAP`; true resource misses still fail closed.
- Guest window state is tracked separately from host HWNDs. `RegisterClassW`, `GetClassInfoW`, `CreateWindowExW`, `FindWindowW`, `GetWindowLongW`, `SetWindowLongW`, `GetParent`, and `GetWindow` use guest HWND records rather than passing guest pointers or fake host HWNDs.
- The message bridge now maps `PostMessageW`, `PostQuitMessage`, `GetMessageW`, `GetMessageWNoWait`, `PeekMessageW`, `TranslateMessage`, `DispatchMessageW`, `SendMessageW`, and `DefWindowProcW`. `DispatchMessageW`/`SendMessageW` transfer into the guest WNDPROC with translated message arguments.
- Guest window visibility is tracked. `ShowWindow` updates visibility and queues `WM_SHOWWINDOW`; `UpdateWindow` queues `WM_PAINT`; `IsWindowVisible` returns guest window state.
- Empty blocking `GetMessageW` no longer fabricates `WM_NULL`; it stops Unicorn with an explicit "blocking with empty guest queue" log entry so the emulator reports the honest idle state.
- Guest heaps and critical sections are tracked as guest-side objects instead of pseudo handles/no-op returns. `WNetGetUserW` is host-backed via `GetUserNameW` and copies through guest memory with Win32-style length/error handling.

## Latest Verification

- MSBuild succeeded after resource/icon, heap, critical-section, WNet, and message-blocking bridge changes.
- Smoke `v2_synth_system_smoke36.log` with explicit SDK DLL directory arguments parsed 18 resources from `INavi.exe`, resolved `FindResourceW(128, RT_GROUP_ICON)`, returned mapped host icon handles from `LoadIconW(128)` and `LoadImageW(128, IMAGE_ICON, 16, ...)`, ran through window creation, `ShowWindow`, `UpdateWindow`, delivered a queued `WM_PAINT`, and dispatched guest WNDPROC calls through MFC.
- The previous `pc=0x00000000 ra=0x0048fa98` shutdown is no longer reproduced. The latest smoke stops cleanly at a blocking empty guest message queue: `UC_ERR_OK pc=0x70002ae8 ra=0x500245c8`.
- Resource misses in the latest smoke match parsed-resource evidence: no `RT_STRING`, no `RT_ACCELERATOR`, and no `RT_GROUP_ICON` id `0x7a02` in `INavi.exe`.
