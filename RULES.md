# RULES.md - FakeCE / iNavi SE G3 Emulator

## Project

Repository: `/mnt/d/GitHub/WinCE_Emulator_v2` or `D:\GitHub\WinCE_Emulator_v2`

Main target:

`iNavi_Unicorn_Emulator.exe`

The NMEA Feeder:

`NMEASender.exe`

Target app:

- iNavi SE G3
- Windows CE GUI application
- MIPS R4000 PE
- Uses COREDLL.dll, mfcce400.dll, CRT-like exports, WINSOCK.dll

This project is worked from WSL, but the emulator itself is a Windows executable.

Windows paths may need `/mnt/c/...` or `/mnt/d/...` translation when invoked from WSL.

Windows `.exe` files can be launched from WSL.

Use the installed Windows CE 4.2 Standard SDK import libraries for ordinal and
decorated-name evidence:

- `C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Lib`
- `C:\Program Files (x86)\Windows CE Tools\wce420\STANDARDSDK_420\Mfc\Lib`

For this target, use `Mipsii` as the primary SDK directory.

The Visual Studio installation is at: `C:\Program Files\Microsoft Visual Studio\18\Community`

The application targets latest Windows-only with x86 target at Zen5. You can add CPU specific-optimizations.

---

## Hard User Rule

This is an emulator.

Do not fake custom app behavior just to make a screenshot look correct/make it work.

Prefer real-ish emulation:

Guest app behavior → MIPS CPU execution → PE imports / COREDLL shims → host-backed file, drawing, audio, registry, timer, and device behavior.

Allowed:

- Targeted tracing
- Targeted guards
- Known-address diagnostics
- Temporary breakpoints/watchpoints
- Host-backed shims for real API semantics

Dangerous unless explicitly justified:

- Manually painting to match app behavior
- Forcing app-specific callbacks
- Replacing app state with guessed values
- Hardcoding iNavi behavior as the final fix
- Pretending success while bypassing the real app path
- Specifying special behavior based on hardcoded strings or file names.

IMPORTANT: DO. NOT. INVENT BEHAVIOR, ADD HARDCODED SPECIFIC FIX, QUIT INVESTIGATION UNLESS THE USER ASKS TO. KEEP RUNNING AUTO TESTS

---

## Development Discipline

Before making changes:

1. Read this file.
2. Read `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md`.
3. Check current git status.
4. Understand whether the current state is a committed fix, diagnostic experiment, broken regression, or untracked artifact.

After meaningful changes:

1. Build.
2. Run a bounded test.
3. Inspect logs/output.
4. Commit real fixes separately from diagnostics. Use git with proper commit message.
5. Clean up / Update `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md`. So there will be no stale entries.
6. Update `README.md` if it contains any stale entries after the change. Always verify against the latest sources.

Do not mix these in one commit:

- Real emulator fix
- Diagnostic hook
- Temporary fallback
- Speculative app-specific workaround
- Formatting-only cleanup

Good commit examples:

- `fix: implement MIPS branch-likely delay-slot annulment`
- `fix: decode indexed DIB byte-plane lower splash slice`
- `trace: add PNG cleanup saved-register watch`
- `diag: log MFC CFile ordinal arguments around mapinfo.bin`
- `revert: remove broad DIB fallback experiment`

---

## Persistent Project Memory Files

Maintain these files as durable memory across Codex sessions:

- `RULES.md`
- `PROGRESS.md`
- `TODO.md`
- `KNOWN_BUGS.md`

`PROGRESS.md` is for confirmed facts only. Include what works, what was fixed, what was a false lead, what regressed, current last-known state, and important commit hashes.

`TODO.md` is for active next steps. Keep sections like Immediate, Next, Later, Parked.

`KNOWN_BUGS.md` is for reproducible failures. Include symptom, current hypothesis, evidence, relevant addresses/ordinals/logs, and status.

---

## MIPS / CPU Context

Do not assume Unicorn alone handles every edge correctly once we intentionally stop/resume around hooks.

Pay attention to:

- delay slots
- branch-likely annulment
- jal / jalr
- jr ra
- saved registers `$s0-$s7`
- stack frame save/restore
- return-bounce handling
- resume PC after intentional hook stops
- setjmp / longjmp behavior

Important MIPS rules:

- Normal branch/jump: delay slot executes.
- Branch-likely not taken: delay slot is annulled and must not execute.

If PC becomes `0x0`, it is not a normal guest exit. Treat it as an
emulator control-flow/resume/return-address bug unless proven otherwise, and
make runtime code fail the run instead of reporting success.

---

## Running / Logging

Prefer bounded runs.

Build:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' iNavi_Unicorn_Emulator.vcxproj /p:Configuration=Release /p:Platform=x64 /m"
```

For active running and testing, use the ps1 script at tools/

---

## WSL / Windows Notes

Expected pattern:

- Use WSL for grep/sed/rg/tail/log inspection.
- Use Windows MSBuild / Visual Studio toolchain for building.
- Run the Windows emulator executable from WSL when useful.

Possible path forms:

- Windows: `D:\INAVI_Emulator\INAVI\\INavi\INavi.exe`
- WSL: `/mnt/d/INAVI_Emulator/INAVI/INavi/INavi.exe`

Do not assume path spelling until verified in logs.
