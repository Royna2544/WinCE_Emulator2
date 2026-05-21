# TODO

## Immediate

- Provide real MIPS CE PE DLLs in the command-line search paths for required system modules such as `COREDLL.dll`, `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, and `OLEAUT32.dll`.
- Keep launch scripts/docs passing SDK DLL directories explicitly; do not reintroduce hardcoded local SDK/iNavi paths in the binary.

## Next

- After real CE system DLLs are present, continue diagnosing the first mapped-DLL execution stop at `PC=0x5004f714`, where `mfcce400.dll` previously read from null immediately after startup entered real SDK code.
- Add cleaner module/load summaries if the loader output becomes too noisy.
- Add forwarder export support if a loaded SDK DLL exposes forwarded exports needed by the target.
- Keep missing DLL/export failures fatal; do not silently present fallback behavior as real DLL execution.
