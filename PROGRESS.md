# Progress

## Confirmed Facts

- This v2 checkout now loads target-side Windows CE/MIPS DLLs as PE images instead of binding every import to a local shim page.
- The loader searches the primary EXE directory plus DLL search directories supplied on the command line; it no longer requires a DLL-copy helper script or hardcoded local SDK paths.
- The loader builds in-memory PE images, maps them into Unicorn, applies base relocations, parses exports, recursively binds imports, and uses fallback stubs only for missing DLLs or unresolved exports.
- Smoke verification loaded 12 PE modules:
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
- The same smoke run bound `inavi.exe` imports with `real=266 fallback=370`, proving MFC imports are going to real SDK DLL exports where available.
- CLI contract is now `iNavi_Unicorn_Emulator.exe <primary.exe> [dll_search_dir ...]`.

## Latest Verification

- MSBuild succeeded after adding recursive DLL PE loading and eager DLL preload.
- `git diff --check` passed.
- Smoke run stopped after entering mapped SDK DLL code at `PC=0x5004f714`, not at the old import-stub page.
