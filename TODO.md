# TODO

## Immediate

- Replace the remaining fallback stubs for missing CE system DLLs (`COREDLL.dll`, `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, `OLEAUT32.dll`) with real PE DLLs if valid MIPS CE binaries are found locally.
- Diagnose the first mapped-DLL execution stop at `PC=0x5004f714`, where `mfcce400.dll` reads from null immediately after startup enters real SDK code.
- Keep launch scripts/docs passing SDK DLL directories explicitly; do not reintroduce hardcoded local SDK/iNavi paths in the binary.

## Next

- Add cleaner module/load summaries if the loader output becomes too noisy.
- Add forwarder export support if a loaded SDK DLL exposes forwarded exports needed by the target.
- Keep fallback stubs loud and measurable; do not silently present them as real DLL execution.
