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

## Latest Verification

- MSBuild succeeded after removing fallback import shims.
- Smoke run with explicit SDK DLL directory arguments maps `inavi.exe`, then stops with `fatal: required DLL not found: COREDLL.dll`.
- The failing search paths were the primary iNavi directory plus the supplied MFC/ATL SDK MIPSII library directories. None contained a real `COREDLL.dll`.
