# Virtual CE Kernel/GWE/MGDI Refactor Checklist

Last refreshed: 2026-05-30.

This is the active checklist for moving the emulator from flattened
`SyntheticDllRuntime` ownership toward:

```text
coredll export -> CE API-set/ordinal boundary -> virtual CE kernel/GWE/MGDI services -> host backing
```

Keep this checklist generic and CE-shaped. Do not add app-specific behavior,
file-name shortcuts, route-screen shortcuts, or coordinate-specific fixes.

## Phase 0: Documentation Anchor

- [x] Create root-level `PLAN.md` with checkbox-based phases.
- [x] Add one short pointer near the top of `TODO.md`.
- [x] Keep `PLAN.md` references out of `RULES.md`, `PROGRESS.md`, and
  `KNOWN_BUGS.md`.

## Phase 1: Build-Only Subsystem Scaffolding

- [x] Add `src/ce_kernel.h` and `src/ce_kernel.cpp` as the future owner for
  CE handles, processes, threads, waits, sleeps, TLS-facing state, and encoded
  kernel-call dispatch.
- [x] Add `src/ce_gwe.h` and `src/ce_gwe.cpp` as the future owner for GWE
  message queues, windows, input routing, timers, foreground/focus/capture,
  visible/update regions, and send-message transactions.
- [x] Add `src/ce_mgdi.h` and `src/ce_mgdi.cpp` as the future owner for DCs,
  selected GDI objects, app/system clip, window bitmaps, and paint-region
  clipping.
- [x] Wire the new source/header files into `iNavi_Unicorn_Emulator.vcxproj`.
- [x] Build Release after scaffolding with the MSBuild command from
  `RULES.md`.
- [x] Confirm scaffold files do not change runtime behavior.

## Phase 2: Virtual Kernel Handle/Thread/Wait Migration

- [x] Move `GuestHandle`, `guestHandles_`, `nextHandle_`, `makeGuestHandle`,
  `lookupGuestHandle`, and `closeGuestHandle` behind `CeKernel`.
  Current source anchors: `src/synthetic_dll.h`, `src/synthetic_dll.cpp`.
- [x] Move `GuestThreadState`, `GuestCpuContext`, `guestThreads_`,
  `activeGuestThread_`, `mainThreadContext_`, and thread IDs behind
  `CeKernel`, keeping Unicorn register capture/restore in the runtime until a
  clean adapter exists.
  Current source anchors: `src/synthetic_dll.h`,
  `src/coredll_thread_runtime.cpp`.
  - [x] Move `GuestThreadState`, `GuestCpuContext`, `GuestThreadRunState`, and
    `guestThreads_` storage behind `CeKernel`.
  - [x] Move active-thread, parked main-thread context, scheduler cursor, and
    thread/process ID counters behind `CeKernel`.
- [x] Move wait/sleep bookkeeping and readiness checks behind `CeKernel`.
  Current source anchors: `refreshSignaledGuestWaits`,
  `switchToRunnableGuestThread`, `cooperateGuestThreadsAfterCall`.
  - [x] Move message-wait wakeup and runnable-thread detection into
    `CeKernel`.
  - [x] Move host-backed wait polling, sleep expiry, and wait result
    assignment into `CeKernel`.
- [ ] Keep host handles as backing values inside CE handles; never expose host
  handles as guest-visible truth.
  - [x] Route immediate `WaitForMultipleObjects` readiness through `CeKernel`
    with a runtime-supplied host wait probe.
  - [x] Route cooperative `WaitForSingleObject` readiness through `CeKernel`
    with a runtime-supplied host wait probe.
  - [x] Route named-dispatch `WaitForSingleObject` guest thread/process
    readiness through `CeKernel`.
  - [ ] Audit remaining direct `hostValue` uses so host APIs remain backing
    services and guest-visible decisions stay in the virtual CE subsystems.

## Phase 3: GWE Message Queue And `SendMessageW` Migration

- [ ] Replace global `guestMessages_` access with a `CeGwe` message-queue API.
  Current source anchors: `src/synthetic_dll.h`,
  `src/coredll_window_runtime.cpp`, `src/coredll_named_dispatch.cpp`.
  - [x] Move the `GuestMessage` record and backing deque storage into
    `CeGwe` while preserving the existing runtime alias.
  - [x] Add `CeGwe::postMessage` and route simple focus/foreground posts
    through it.
  - [ ] Replace direct runtime deque operations with named `CeGwe` queue
    methods in small behavior-preserving batches.
- [ ] Add per-thread or per-owner queues aligned with CE `MsgQueue`
  behavior. CE source anchors:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:23`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
- [ ] Route `PostMessageW`, thread messages, input messages, timers,
  `GetMessageW`, `PeekMessageW`, `DispatchMessageW`, and
  `MsgWaitForMultipleObjectsEx` through `CeGwe`.
- [ ] Model cross-thread `SendMessageW` as a sender-blocked queue transaction
  with receiver-context execution and result transfer back to the sender.
  Current source anchor: `src/synthetic_dll.cpp` send-message inline path.

## Phase 4: Window Visible/Update/Client Region Migration

- [ ] Move `GuestWindow`, `windows_`, window class maps, z-order, parent/owner
  relationships, focus, capture, and hit testing behind `CeGwe`.
  Current source anchors: `src/synthetic_dll.h`,
  `src/coredll_window_runtime.cpp`, `src/coredll_named_dispatch.cpp`.
- [ ] Add CE-shaped visible, update, client-visible, and client-update regions
  before changing paint behavior. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
- [ ] Replace popup/backing/z-order heuristics with region-owned visibility
  decisions where possible. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`.

## Phase 5: MGDI DC/Window Clipping Migration

- [ ] Move `GuestDc`, `GuestBitmap`, brush/pen/font maps, stock objects, DC
  creation, and selected-object state behind `CeMgdi`.
  Current source anchors: `src/synthetic_dll.h`, `src/coredll_bitmap.cpp`,
  `src/coredll_paint.cpp`, `src/coredll_named_dispatch.cpp`.
- [ ] Add app clip, system clip, and actual device clip state before adding
  more blit optimizations. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
- [ ] Add a CE-shaped internal window-bitmap model before treating current
  saved backing layers as faithful. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
- [ ] Keep host framebuffer and host windows as final rendering/presentation
  backends, not as owners of guest-visible clipping semantics.

## Phase 6: Encoded MIPS CE Kernel-Call Dispatch

- [ ] Add decode helpers for high-address MIPS CE API-call thunks such as the
  observed `0xfffff3fa` path.
- [ ] Dispatch decoded process/thread termination calls through `CeKernel`
  instead of reporting a bad guest PC when the target is a valid CE API-call
  encoding.
- [ ] Preserve the project rule that `PC == 0` is fatal unless a specific
  decoded CE kernel-call path proves a legitimate guest exit.

## Phase 7: Regression Testing And Memory-Doc Refresh

- [ ] After every code phase, run the Release MSBuild command from `RULES.md`.
- [ ] After behavior migrations, run a bounded emulator smoke test through the
  existing `tools/` runner.
- [ ] Inspect logs for new unsupported coredll ordinal regressions, false
  `PC == 0` success, startup frame regressions, message-queue deadlocks, and
  modal/input routing regressions.
- [ ] After GWE/MGDI phases, capture at least one startup frame and one
  interactive/modal UI path for visual comparison.
- [ ] Refresh `PROGRESS.md`, `TODO.md`, and `KNOWN_BUGS.md` after each
  completed behavior phase, keeping CE source references beside behavior
  changes.
