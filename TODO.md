# TODO

## Immediate

- Continue replacing generic synthetic `coredll.dll` returns with SDK-canonical, host-backed behavior. Trust the Windows CE 4.2 Standard SDK `coredll.lib` COFF import-object headers as the naming source; do not rename an ordinal just because one runtime call shape looks different.
- Keep launch arguments explicit: `iNavi_Unicorn_Emulator.exe <primary.exe> [dll_search_dir ...]`. Do not reintroduce hardcoded local SDK/iNavi paths in the binary.
- Keep real SDK DLLs preferred over synthetic modules. Synthetic `coredll.dll` exists because the installed SDK supplies `coredll.lib` but no real MIPS `COREDLL.dll` PE image.
- Continue from smoke36's clean idle-block stop after the first paint (`GetMessageW` blocking with empty guest queue, `pc=0x70002ae8 ra=0x500245c8`). The previous null-PC teardown was caused by `GetMessageW` reporting WM_QUIT too early and is fixed by the message bridge.
- Add real drawing/paint bridge next. `WM_PAINT` is delivered, but no framebuffer-producing GDI/paint path is implemented yet.

## Weird Ordinals To Verify Later

- Rejected stale `/LINKERMEMBER` labels for menu/memory ordinals: SDK COFF import-object headers confirm `0x0411=malloc`, `0x0416=memmove`, `0x0417=memset`, and `0x0447=operator_new`. Do not restore the old `RemoveMenu`/`CheckMenuItem`/`CheckMenuRadioItem` labels at those ordinals.
- Confirmed menu/window ordinals from SDK COFF import-object headers now implemented or fail-closed by name: `0x005E=LoadAcceleratorsW`, `0x00FB=GetWindow`, `0x0102=SetWindowLongW`, `0x0103=GetWindowLongW`, `0x010D=GetParent`, `0x02DA=LoadImageW`, `0x034B=RemoveMenu`, `0x034E=LoadMenuW`, `0x0350=CheckMenuItem`, `0x0351=CheckMenuRadioItem`.
- `LoadMenuW` has not appeared in smoke31 despite `INavi.exe` having `RT_MENU` id 128. Keep the host-backed implementation, but verify when a caller reaches it.

## Next

- Audit remaining called coredll paths that are still minimal guest-side implementations, especially `_setjmp`/`longjmp` and registry access. Do not invent ABI layouts; preserve SDK names and fail closed or document evidence when exact behavior is not known.
- Implement SDK-backed host behavior for any remaining file APIs such as `GetFileTime`. Existing `CreateFileW`, `ReadFile`, `WriteFile`, `GetFileSize`, `SetFilePointer`, `SetFileTime`, `FindFirstFileW`, and close/find-close paths are host-handle mapped.
- Add cleaner module/load summaries if loader output becomes too noisy.
- Add forwarder export support if a loaded SDK DLL exposes forwarded exports needed by the target.
- Record each ordinal discovery as Confirmed, Likely, Speculative, or Rejected in `PROGRESS.md` or `KNOWN_BUGS.md` with evidence source.

## Later

- Reduce synthetic-call logging once the host-backed API surface is stable enough to launch farther.
- Audit synthetic `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, and `OLEAUT32.dll` for the same SDK-canonical ordinal discipline.
