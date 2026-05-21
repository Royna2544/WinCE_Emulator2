# TODO

## Immediate

- Continue replacing generic synthetic `coredll.dll` returns with SDK-canonical, host-backed behavior as new callers appear. Trust the Windows CE 4.2 Standard SDK `coredll.lib` COFF import-object headers as the naming source, but record conflicts when real loaded SDK DLL runtime call shapes prove a compatibility ordinal.
- Keep launch arguments explicit: `iNavi_Unicorn_Emulator.exe <primary.exe> [dll_search_dir ...]`. Do not reintroduce hardcoded local SDK/iNavi paths in the binary.
- Keep real SDK DLLs preferred over synthetic modules. Synthetic `coredll.dll` exists because the installed SDK supplies `coredll.lib` but no real MIPS `COREDLL.dll` PE image.
- Continue from smoke71's clean idle-block stop after the app-level `can't read HWInfoDB` message (`GetMessageW` blocking with empty guest queue, `pc=0x70002ae8 ra=0x500245c8`). The app opens and reads `INavi\res\values.dat` before aborting.
- Use the user's real-device dump at `C:\Users\royna\Downloads\DUMPPLZ` as the current source of truth. Keep `dumpplz_regs.json` synced from `DUMPPLZ\REGISTRY.TXT` and do not hardcode device/OEM/HWInfo identity in `.cpp`; keep `SystemParametersInfoW`, registry, and `KernelIoControl` identity inputs external JSON-backed.
- Continue the app-level HWInfoDB investigation at the profile matcher before `values.dat` lookup. Current assembly diagnostics show `0x129204 -> 0x299544` fails to select a profile, so `0x000594a4` stores id `0`/empty name and `0x0006bd18` later scans 118 `values.dat` records for missing id `0`. Do not bypass the warning by hardcoding iNavi state.
- TODO: Verify the real-device contract for `KernelIoControl(0x01012ef4)` (`device=0x0101`, `function=0xbbd`). Current diagnostic bridge writes configured string results as raw NUL-terminated bytes from external registry JSON entries under `hklm\system\emulator\kernelioctl\<entry>` where `ioctlcmd` is the command and `return` is the result; keep this table synced with the real device dump once available.
- Continue the real drawing/paint bridge. A host presenter now displays the framebuffer and survives guest teardown for inspection, but the synchronous `UpdateWindow` experiment was reverted after it made the interactive startup path worse; richer common controls, complete host input-to-guest message translation, and remaining color-format edge cases still need work.
- Prioritize the map rendering failure in the next user-driven run. Current route-search crash log proves map payload files are found and read (`INavi\mapdata\mapinfo.bin`, `cross\FullData.dat`, and multiple `MRData` files), so the next investigation should focus on why loaded map data does not reach the framebuffer/GDI path rather than treating it as a missing-file problem.
- Use `tools/autodrive_inavi.ps1` for unattended touch/screenshot probes. Keep tap plans as external script arguments; do not encode app-specific success states in the emulator. The current default roots match the last map-loading evidence, including `C:\Users\royna\Downloads`, `...\INAVI\mapdata`, and `...\INAVI\iNaviData`.
- Implement or design a real guest-thread bridge. Current map evidence shows `CreateThread(... start=0x000e6cd0 ...)` and `CreateThread(... start=0x000e513c ...)` followed by `ResumeThread`, but those entry points never execute because `GuestThread` is only a handle record. This is now the strongest non-fake explanation for initialized map files not becoming a rendered map.
- Continue verifying iNavi settings/menu art after the bitmap-mask fix. Current runtime evidence shows the target's 16-bit DIB sections use `BI_BITFIELDS` masks `f800/07e0/001f` (RGB565), and the obvious green/patterned icon blocks are fixed in `v2_synth_inavi_ui565_text.log`; if a specific asset still has a cast, trace that bitmap path instead of changing conversion globally.
- Implement the real host serial/GPS bridge for `--gps-comm`. Current iNavi dump-backed smoke opens guest `COM7:` and calls normal comm APIs; without an explicit host COM port it remains disconnected and the app follows its serial-port failure UI path.
- Fix CE-style filesystem root enumeration/volume merging so `--fs-root C:\Users\royna\Downloads\DUMPPLZ\FILES` and the INAVI payload can coexist without ordering hacks. Current `\*` enumeration only reports the first resolved host root, which can hide `iNaviData`/`mapdata` from the app.

## Weird Ordinals To Verify Later

- Rejected stale `/LINKERMEMBER` labels for menu/memory ordinals: SDK COFF import-object headers confirm `0x0411=malloc`, `0x0416=memmove`, `0x0417=memset`, and `0x0447=operator_new`. Do not restore the old `RemoveMenu`/`CheckMenuItem`/`CheckMenuRadioItem` labels at those ordinals.
- Confirmed menu/window ordinals from SDK COFF import-object headers now implemented or fail-closed by name: `0x005E=LoadAcceleratorsW`, `0x00FB=GetWindow`, `0x0102=SetWindowLongW`, `0x0103=GetWindowLongW`, `0x010D=GetParent`, `0x02DA=LoadImageW`, `0x034B=RemoveMenu`, `0x034E=LoadMenuW`, `0x0350=CheckMenuItem`, `0x0351=CheckMenuRadioItem`.
- Confirmed `commctrl.dll` MIPSII SDK import-object ordinals for current MFC imports: `#1=InitCommonControls`, `#2=InitCommonControlsEx`, `#5=CommandBar_AddBitmap`, `#6=CommandBar_InsertComboBox`, `#9=CommandBar_GetMenu`, `#10=CommandBar_AddAdornments`, `#18=PropertySheetW`, `#19=CreatePropertySheetPageW`, `#20=DestroyPropertySheetPage`, `#42=CommandBar_InsertMenubarEx`, and `#43=CommandBar_DrawMenuBar`. Do not use `/LINKERMEMBER` columns for these names.
- `LoadMenuW` has not appeared in smoke31 despite `INavi.exe` having `RT_MENU` id 128. Keep the host-backed implementation, but verify when a caller reaches it.
- COREDLL `#255=ScreenToClient` is confirmed by SDK COFF import-object headers and implemented. Runtime also shows loaded MFC calling COREDLL `#256` with the same `ScreenToClient(HWND, POINT*)` shape; keep the compatibility mapping documented and do not silently convert that runtime path to `SetWindowTextW` without stronger target evidence.
- COREDLL `#682=SetCursor`, `#693=GetDlgCtrlID`, `#887=AdjustWindowRectEx`, and `#97=EqualRect` are confirmed by SDK COFF import-object headers and are implemented in the translate layer.
- COREDLL `#89=SystemParametersInfoW` is confirmed by SDK COFF import-object headers and runtime call shape. The stale `#89=wcslen` label is rejected; `wcslen` remains at SDK-confirmed `#63`.
- COREDLL `#1875` is imported by `iSearch.exe` and called once at CRT startup with `a0=0x00010000 a1=0 a2=1 a3=1`, but it was not found in the installed Windows CE 4.2 Standard SDK MIPSII `coredll.lib`. Keep it unresolved until confirmed by a real SDK/export source, disassembly, or stronger runtime evidence.
- COREDLL `#179=DeviceIoControl` is confirmed by Windows CE 4.2 Standard SDK MIPSII `coredll.lib` COFF import-object headers. Implemented as a real handle bridge for host file/serial handles and as an honest fail-closed result for disconnected guest devices such as `UID1:`.
- COREDLL `#1023=_hypot`, `#2046=__gts`, and `#2052=__gtd` are confirmed by Windows CE 4.2 Standard SDK MIPSII `coredll.lib` COFF import-object headers and implemented for the route-search/map path.

## Next

- Reverse or trace the profile predicates inside `0x299544` far enough to identify which real registry/ioctl/file values select a nonzero HWInfo id. Known predicate inputs include `HKLM\SOFTWARE\TubeNavi\PRODUCT\ModelID`, `KernelIoControl(0x0101207c)`, and `KernelIoControl(0x01012ef4)`.
- Continue using `iSearch.exe` as a second launch target. Next useful bridge work is host input/event delivery into the blocked `GetMessageW` loop and any file-mapping view paths if `MapViewOfFile` appears beyond the initial `CreateFileMappingW`.
- Re-run `INavi.exe` with a real/virtual host GPS serial port via `--gps-comm` once available. Confirm that guest `COM7:` or the configured CE port maps to the host port and that `ReadFile` receives NMEA-like data from the host stream rather than fabricated test data.
- CE_MANAGER launch is blocked by missing real `WININET.dll`. Do not synthesize `WININET.dll` unless explicitly requested; if a real CE/MIPS `WININET.dll` is supplied, rerun `CE_Manager.exe` with that directory in the explicit DLL search path.
- Audit remaining called coredll paths that are still minimal guest-side implementations, especially `_setjmp`/`longjmp`, `__ehvec_ctor`, and locale/NLS APIs. Do not invent ABI layouts; preserve SDK names and fail closed or document evidence when exact behavior is not known.
- Extend COM proxying only from real callers: current bridge supports host COM creation and `IUnknown` proxy stubs, but arbitrary interface methods require per-interface guest vtables and dispatch methods.
- Extend `commctrl.dll` only from real callers. Command bar/status/toolbar/updown creation and menu attachment are guest-side/host-menu backed; property sheet display currently fails closed, and DSA/DPA/list/tree/header details should be implemented when runtime calls prove the needed ABI.
- Implement SDK-backed host behavior for any remaining file APIs such as `GetFileTime`. Existing `CreateFileW`, `ReadFile`, `WriteFile`, `GetFileSize`, `SetFilePointer`, `SetFileTime`, `FindFirstFileW`, and close/find-close paths are host-handle mapped.
- Add cleaner module/load summaries if loader output becomes too noisy.
- Add forwarder export support if a loaded SDK DLL exposes forwarded exports needed by the target.
- Record each ordinal discovery as Confirmed, Likely, Speculative, or Rejected in `PROGRESS.md` or `KNOWN_BUGS.md` with evidence source.

## Later

- Reduce synthetic-call logging once the host-backed API surface is stable enough to launch farther.
- Audit synthetic `commctrl.dll`, `WINSOCK.dll`, `ole32.dll`, and `OLEAUT32.dll` for the same SDK-canonical ordinal discipline.
