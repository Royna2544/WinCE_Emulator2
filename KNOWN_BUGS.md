# Known Bugs And Theories

## Confirmed Bugs

### Missing CE system DLLs block launch

- Status: Open.
- Evidence: Search found SDK `coredll.lib` import libraries but no real `COREDLL.dll` PE image under the installed Windows CE Tools tree or local iNavi package.
- Effect: With fallback stubs removed, launch fails until real required MIPS CE system DLLs are supplied in the command-line search paths.
- Latest result: Explicit SDK search-path smoke maps `inavi.exe`, then fails with `fatal: required DLL not found: COREDLL.dll`.
- Constraint: Do not replace these with fake app behavior; use real MIPS CE DLLs.

### Real MFC execution currently reads null

- Status: Open.
- Evidence: Earlier smoke run with fallback imports still enabled mapped `mfcce400.dll`, started `iNavi.exe`, then stopped at `PC=0x5004f714` with a null read and `RA=0x5004ec70`.
- Interpretation: This is progress from all-shim execution into actual SDK DLL code, but MFC likely needs loader/runtime initialization, TLS/global state, or better system DLL behavior before it can run farther.

## Discarded Theories

### The v2 loader must copy SDK DLLs into `runtime_dlls`

- Status: Discarded.
- Reason: The loader now discovers DLLs from the primary EXE directory and caller-provided search directories, then maps those real PE files. Runtime copies are optional evidence only, not the primary workflow.
