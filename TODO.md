# TODO

## Immediate

- Continue replacing generic synthetic `coredll.dll` returns with SDK-canonical, host-backed behavior. Trust the Windows CE 4.2 Standard SDK `coredll.lib` names/ordinals as the naming source; do not rename an ordinal just because one runtime call shape looks different.
- Keep launch arguments explicit: `iNavi_Unicorn_Emulator.exe <primary.exe> [dll_search_dir ...]`. Do not reintroduce hardcoded local SDK/iNavi paths in the binary.
- Keep real SDK DLLs preferred over synthetic modules. Synthetic `coredll.dll` exists because the installed SDK supplies `coredll.lib` but no real MIPS `COREDLL.dll` PE image.

## Weird Ordinals To Verify Later

- `0x0411` / decimal `1041`: SDK says `RemoveMenu`. Earlier runtime call shape looked allocation-like from `INavi.exe` and was temporarily handled as an allocator. Re-verify with target import table, caller disassembly, and SDK header/lib evidence before keeping any non-`RemoveMenu` behavior.
- `0x0416` / decimal `1046`: SDK says `CheckMenuItem`, not `memcpy`. Earlier MFC/app call shape looked copy-like. Re-verify before treating it as memory copy.
- `0x0417` / decimal `1047`: SDK says `CheckMenuRadioItem`, not `memset`. Earlier MFC/app call shape looked fill-like. Re-verify before treating it as memory fill.
- `0x0447` / decimal `1095`: SDK says `RegisterTaskBar`. Earlier app call at `RA=0x00011e10` passed `a0=0x110` and used the return as a pointer. Re-verify with disassembly and SDK declarations before keeping pointer-allocation behavior.
- `0x0045` / decimal `69`: SDK says `HeapAlloc`. A later MFC path around `0x50050404` looked like path/string handling after `GetModuleFileNameW`; verify whether the bad behavior was caused by an incomplete `GetModuleFileNameW` buffer write rather than by ordinal identity.
- `0x00FB` / decimal `251`: SDK says `SetFileTime`; `mfcce400.dll` imports/calls it repeatedly near `0x500277c8`/`0x50027838`. Verify whether generic success is causing the current loop/early `UC_ERR_OK` stop.
- `0x0537` / decimal `1335`: SDK says scalar delete with `nothrow_t`. Generic success is wrong; implement delete/free semantics and confirm callers do not consume a success value as a pointer.

## Next

- Implement SDK-backed host behavior for file APIs: `CreateFileW`, `ReadFile`, `WriteFile`, `GetFileSize`, `SetFilePointer`, `GetFileTime`, `SetFileTime`, and close/free semantics. Preserve real host-backed file behavior; do not return dummy success with fake lengths.
- Add cleaner module/load summaries if loader output becomes too noisy.
- Add forwarder export support if a loaded SDK DLL exposes forwarded exports needed by the target.
- Record each ordinal discovery as Confirmed, Likely, Speculative, or Rejected in `PROGRESS.md` or `KNOWN_BUGS.md` with evidence source.

## Later

- Reduce synthetic-call logging once the host-backed API surface is stable enough to launch farther.
- Audit synthetic `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, and `OLEAUT32.dll` for the same SDK-canonical ordinal discipline.
