# RULES.md - FakeCE / iNavi SE G3 Emulator

This is the primary project memory and workflow file. `AGENTS.md` is only a
short entry-point reference back to this file.

## Project

Repository: `/mnt/d/GitHub/WinCE_Emulator`

Main target:

`.\x64\Debug\WinCE_Emulator.exe "C:\Users\royna\Downloads\INAVI\INavi\INavi.exe"`

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

For this target, use `Mipsii` as the primary SDK directory. Use `Mipsii_fp` only
as an A/B comparison if floating-point behavior diverges; checked COREDLL and
MFC ordinals matched for the inspected target surfaces.

The Visual Studio installation is at: `C:\Program Files\Microsoft Visual Studio\18\Community`

---

## Hard User Rule

This is an emulator.

Do not fake custom iNavi app behavior just to make a screenshot look correct.

Prefer real-ish emulation:

Guest app behavior → MIPS CPU execution → PE imports / COREDLL shims → host-backed file, drawing, audio, registry, timer, and device behavior.

Allowed:

- Targeted tracing
- Targeted guards
- Known-address diagnostics
- Temporary breakpoints/watchpoints
- Host-backed shims for real API semantics

Dangerous unless explicitly justified:

- Manually painting the splash/progress bar
- Forcing app-specific callbacks
- Replacing app state with guessed values
- Hardcoding iNavi behavior as the final fix
- Pretending success while bypassing the real app path

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
4. Commit real fixes separately from diagnostics.

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

Track ordinal discoveries in the durable memory files above. Mark each entry as
Confirmed, Likely, Speculative, or Rejected, and include the evidence source:
SDK import library, target import table, runtime call shape, or disassembly.

---

## SDK / Ordinal Policy

The SDK import libraries are evidence for symbol names and ordinals. They are
not proof that a stub's behavior is complete.

Use SDK evidence to:

- rename stale or speculative ordinal labels
- correct direct ordinal mismatches
- identify MFC decorated names and vtable candidates
- compare `Mipsii` against `Mipsii_fp` when needed

Use runtime evidence to:

- determine call shape
- decide whether an MFC ordinal is acting as a constructor, method, data export,
  or compatibility path
- decide whether a host-backed shim should write memory, return an object, or
  fail like CE would

Do not mass-replace behavior from names alone when the app has already shown a
different call shape. Record the conflict in `PROGRESS.md` or `KNOWN_BUGS.md`.

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

If PC becomes `0x0`, treat it as a control-flow/resume/return-address bug unless proven otherwise.

---

## Registry / Device Context

Do not brute-force registry model values as a final fix.

Allowed:

- log RegOpenKey / RegQueryValue / RegSetValue
- record keys and values requested by the app
- provide plausible CE/device answers only when backed by call evidence

The real device registry is unknown unless dumped from hardware.

---

## Audio Context

Do not fabricate audio playback.

The real milestone is first reaching calls such as:

- sndPlaySound
- PlaySound
- waveOutOpen
- waveOutPrepareHeader
- waveOutWrite
- mciSendString

If no audio-capable APIs are called, the app probably has not passed the real startup gate yet.

---

## Running / Logging

Prefer bounded runs.

Build:

```bash
powershell.exe -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe' WinCE_Emulator.vcxproj /p:Configuration=Debug /p:Platform=x64 /m"
```

Convert framebuffer for inspection:

```bash
ffmpeg -y -loglevel error -i fakece_gui.ppm /tmp/fakece_gui.png
```

PowerShell output files may be UTF-16LE. Use `iconv -f UTF-16LE -t UTF-8`
before searching if needed.

Use targeted logs instead of massive all-trace logs.

Environment-variable diagnostics are allowed, but keep them documented.

Useful diagnostic scopes:

- DIB / GDI
- MFC file ordinals
- PNG state / cleanup
- branch-likely / delay-slot
- audio calls
- registry calls
- private window messages
- heap allocation

Avoid broad hooks that perturb startup timing unless explicitly needed.

If a trace changes behavior, mark it as perturbing.

---

## WSL / Windows Notes

Expected pattern:

- Use WSL for grep/sed/rg/tail/log inspection.
- Use Windows MSBuild / Visual Studio toolchain for building.
- Run the Windows emulator executable from WSL when useful.

Possible path forms:

- Windows: `C:\Users\royna\Downloads\INAVI\INavi\INavi.exe`
- WSL: `/mnt/c/Users/royna/Downloads/INAVI/INavi/INavi.exe`

Do not assume path spelling until verified in logs.

---

## Definition of Progress

Progress is not only “the screenshot looks better.”

Real progress includes:

- A crash moves later
- A wrong dummy API becomes real-ish
- A CPU semantic bug is fixed generally
- A bad pointer source is narrowed
- A diagnostic is separated from a production fix
- A regression becomes reproducible
- A false theory is explicitly retired

Always prefer the smallest real emulator correction that explains the observed app behavior.
