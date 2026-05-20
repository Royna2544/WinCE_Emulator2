# Known Bugs And Theories

## Confirmed Bugs

### Missing CE system DLLs still fall back to stubs

- Status: Open.
- Evidence: Latest smoke run reports missing `COREDLL.dll`, `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, and `OLEAUT32.dll`.
- Effect: App and SDK DLL code can enter real mapped DLLs, but calls into those missing CE system modules still return through fallback stubs.
- Constraint: Do not replace these with fake app behavior; use real MIPS CE DLLs if found, or implement generic API semantics explicitly and keep them logged as fallback behavior.

### Real MFC execution currently reads null

- Status: Open.
- Evidence: Latest smoke run maps `mfcce400.dll`, starts `iNavi.exe`, then stops at `PC=0x5004f714` with a null read and `RA=0x5004ec70`.
- Interpretation: This is progress from all-shim execution into actual SDK DLL code, but MFC likely needs loader/runtime initialization, TLS/global state, or better system DLL behavior before it can run farther.

## Discarded Theories

### The v2 loader must copy SDK DLLs into `runtime_dlls`

- Status: Discarded.
- Reason: The loader now discovers DLLs from the primary EXE directory and caller-provided search directories, then maps those real PE files. Runtime copies are optional evidence only, not the primary workflow.
