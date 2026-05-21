# iNavi Unicorn Emulator v2

Fresh-from-scratch Visual C++ project. It does not reuse the old emulator source tree.

Goals:
- Load the Windows CE/MIPS PE for iNavi.
- Load available target-side CE/MIPS DLLs as PE images from the primary EXE
  directory and caller-provided DLL search directories.
- Use Unicorn for MIPS execution.
- Use spdlog for deterministic logging.
- Treat `runtime_dlls/` as optional local evidence only; the loader can use the
  installed SDK/iNavi DLLs directly when their directories are passed on the
  command line.
- Emit framebuffer PPM captures so execution progress can be inspected.

Build from WSL:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' iNavi_Unicorn_Emulator.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:VcpkgRoot=D:\vcpkg\ /m"
```

Run smoke test:

```bash
powershell.exe -NoProfile -Command "& .\x64\Debug\iNavi_Unicorn_Emulator.exe 'C:\Users\royna\Downloads\INAVI\INavi\INavi.exe' 'C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Mfc\Lib\Mipsii' 'C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Mfc\Lib\Mipsii\L.kor' 'C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Atl\Lib\Mipsii'"
```
