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
- Fail fast when a required DLL or export is missing; this project does not
  silently replace DLLs with fallback shims.
- Emit framebuffer PPM captures so execution progress can be inspected.

Build from WSL:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' iNavi_Unicorn_Emulator.vcxproj /p:Configuration=Debug /p:Platform=x64 /p:VcpkgRoot=D:\vcpkg\ /m"
```

Run smoke test:

```bash
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& 'tools\autodrive_inavi.ps1' -NoTaps -KeepAlive -InitialSettleMs 8000 -StartupTimeoutMs 45000"
```

The harness passes the current registry, `--sdmmc-path`, serial map, and CE SDK
DLL search directories used for normal debugging runs. `--sdmmc-path` is the
host directory backing guest `\SDMMC Disk`; it is not a guest path.
