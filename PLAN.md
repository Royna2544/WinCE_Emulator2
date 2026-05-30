# Virtual CE Kernel/GWE/MGDI Refactor Checklist

Last refreshed: 2026-05-30.

This is the active checklist for moving the emulator from flattened
`SyntheticDllRuntime` ownership toward:

```text
coredll export -> CE API-set/ordinal boundary -> virtual CE kernel/GWE/MGDI services -> host backing
```

Keep this checklist generic and CE-shaped. Do not add app-specific behavior,
file-name shortcuts, route-screen shortcuts, or coordinate-specific fixes.
Prefer named constants, enums, or narrow `constexpr` values over raw special
numbers when touching CE ordinals, message IDs, flags, queue priorities, wait
results, or timing thresholds.

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
  - [x] Route remaining append-style message producers through
    `CeGwe::postMessage`.
  - [x] Route front/priority message insertion through named `CeGwe` queue
    helpers.
  - [x] Route simple queued-message pruning through `CeGwe::eraseIf`.
  - [x] Route first-match dequeue/peek operations through
    `CeGwe::firstMatching`.
  - [x] Replace direct runtime deque operations with named `CeGwe` queue
    methods in small behavior-preserving batches.
- [ ] Add per-thread or per-owner queues aligned with CE `MsgQueue`
  behavior. CE source anchors:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:23`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
  - [x] Add a GWE-side thread-queue/window-owner registry scaffold and
    register created/destroyed windows without changing delivery behavior.
    Current source anchors: `src/ce_gwe.h`, `src/coredll_window_runtime.cpp`,
    `src/coredll_named_dispatch.cpp`.
  - [x] Add message IDs and mirrored owner-thread lanes for posted, sent,
    input, timer, and thread messages while keeping the existing flat queue as
    the dispatch source. Current source anchors: `src/ce_gwe.h`,
    `src/synthetic_dll.cpp`, `src/coredll_named_dispatch.cpp`, and
    `src/coredll_window_runtime.cpp`.
  - [x] Add owner/lane query helpers and owner-filtered first-match selection
    in `CeGwe` as the staging API for migrating `GetMessageW`/`PeekMessageW`
    off flat global matching.
- [ ] Route `PostMessageW`, thread messages, input messages, timers,
  `GetMessageW`, `PeekMessageW`, `DispatchMessageW`, and
  `MsgWaitForMultipleObjectsEx` through `CeGwe`.
  - [x] Add semantic `CeGwe` posting entry points for posted, thread, input,
    and timer messages while preserving the current backing queue.
    Current source anchors: `src/ce_gwe.h`, `src/coredll_named_dispatch.cpp`,
    `src/coredll_window_runtime.cpp`, `src/remote_server.cpp`.
  - [x] Route `GetMessageW`/`PeekMessageW` first-match selection through the
    owner-filtered `CeGwe` query API while preserving the existing flat queue
    ordering inside the selected owner queue.
    Current source anchor: `src/coredll_named_dispatch.cpp`.
  - [x] Make message-wait wakeup owner-aware: waiting guest threads now wake
    only when their `CeGwe` owner queue has work, and active-thread
    `GetMessageW` parks based on its own queue instead of the global queue.
    Current source anchors: `src/ce_kernel.cpp`,
    `src/coredll_thread_runtime.cpp`, and `src/synthetic_dll.cpp`.
  - [x] Register and route `MsgWaitForMultipleObjectsEx` through the virtual
    kernel/GWE boundary: handle readiness stays in `CeKernel`, message
    readiness uses the caller's `CeGwe` owner queue, and blocking guest
    threads can resume from either source. CE source anchors:
    `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:1843`
    and
    `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:802`.
    Current source anchors: `src/synthetic_dll.cpp`,
    `src/ce_kernel.cpp`, and `src/synthetic_dll_modules.cpp`.
- [ ] Model cross-thread `SendMessageW` as a sender-blocked queue transaction
  with receiver-context execution and result transfer back to the sender.
  Current source anchor: `src/synthetic_dll.cpp` send-message inline path.
  - [x] Generalize cross-thread `SendMessageW` routing from worker-to-main
    only to any different window-owner queue: enqueue a sent message, park the
    sender as `WaitingForSendMessage`, prefer the receiver's guest thread when
    it is not the main pseudo-thread, and return the window-proc result to the
    sender on completion. CE source anchor:
    `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:897`.
    Current source anchor: `src/synthetic_dll.cpp`.

## Phase 4: Window Visible/Update/Client Region Migration

- [ ] Move `GuestWindow`, `windows_`, window class maps, z-order, parent/owner
  relationships, focus, capture, and hit testing behind `CeGwe`.
  Current source anchors: `src/synthetic_dll.h`,
  `src/coredll_window_runtime.cpp`, `src/coredll_named_dispatch.cpp`.
- [ ] Add CE-shaped visible, update, client-visible, and client-update regions
  before changing paint behavior. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
  - [x] Add a GWE-side window-region shadow with window, client, visible,
    update, client-visible, and client-update rectangles. Feed it from current
    window publication and invalidate/validate calls without changing
    `BeginPaint` behavior yet. Current source anchors: `src/ce_gwe.h`,
    `src/coredll_named_dispatch.cpp`, and `src/coredll_paint.cpp`.
  - [x] Make `BeginPaint` consume the GWE client-update rectangle when present
    and make `EndPaint` validate the GWE update region after painting. Current
    source anchor: `src/coredll_paint.cpp`.
- [ ] Replace popup/backing/z-order heuristics with region-owned visibility
  decisions where possible. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`.
  - [x] Feed GWE window-region state with absolute rectangles for all
    non-external guest windows whenever window state is published, and use
    GWE visible-region queries for input hit testing and pointer capture
    validity. Current source anchors: `src/ce_gwe.h`,
    `src/coredll_named_dispatch.cpp`, and `src/coredll_window_runtime.cpp`.

## Phase 5: MGDI DC/Window Clipping Migration

- [ ] Move `GuestDc`, `GuestBitmap`, brush/pen/font maps, stock objects, DC
  creation, and selected-object state behind `CeMgdi`.
  Current source anchors: `src/synthetic_dll.h`, `src/coredll_bitmap.cpp`,
  `src/coredll_paint.cpp`, `src/coredll_named_dispatch.cpp`.
  - [x] Add a `CeMgdi` DC-state shadow and mirror DC create/destroy,
    selected objects, text state, current drawing position, and initial
    system clip from GWE visible rectangles without changing drawing
    enforcement yet. Current source anchors: `src/ce_mgdi.h`,
    `src/coredll_bitmap.cpp`, `src/coredll_paint.cpp`, and
    `src/coredll_named_dispatch.cpp`.
  - [x] Add a `CeMgdi` bitmap-state shadow and mirror stock/default bitmap,
    `CreateBitmap`, `CreateDIBSection`, `CreateCompatibleBitmap`, palette-size
    changes, and bitmap deletion while keeping pixel storage in the current
    runtime bitmap map. Current source anchors: `src/ce_mgdi.h`,
    `src/coredll_bitmap.cpp`, and `src/coredll_named_dispatch.cpp`.
  - [x] Route `GetObjectW` bitmap metadata through `CeMgdi::BitmapState`
    instead of reading `GuestBitmap` directly. Current source anchor:
    `src/coredll_bitmap.cpp`.
  - [x] Route `SetBitmapBits` storage-size and guest-bits lookup through
    `CeMgdi::BitmapState` while leaving pixel bytes in guest memory. Current
    source anchors: `src/ce_mgdi.h` and `src/coredll_bitmap.cpp`.
  - [x] Route `SetDIBColorTable` bitmap validation and palette bounds through
    `CeMgdi::BitmapState` while the palette bytes still live in `GuestBitmap`.
    Current source anchor: `src/coredll_bitmap.cpp`.
  - [x] Route `DeleteObject` stock-bitmap protection through
    `CeMgdi::BitmapState` before host/guest bitmap storage teardown. Current
    source anchor: `src/coredll_named_dispatch.cpp`.
  - [x] Route `SelectObject` bitmap selection validation through
    `CeMgdi::BitmapState` before updating the DC selected-bitmap state.
    Current source anchor: `src/coredll_named_dispatch.cpp`.
- [ ] Add app clip, system clip, and actual device clip state before adding
  more blit optimizations. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  - [x] Make central framebuffer drawing paths consult the MGDI system clip:
    rectangle fills and bitmap blits intersect their output bounds, while
    line, `StretchDIBits`, and transparent image writes point-check against
    the clip. Current source anchors: `src/ce_mgdi.h`,
    `src/synthetic_dll.h`, and `src/coredll_bitmap.cpp`.
- [ ] Add a CE-shaped internal window-bitmap model before treating current
  saved backing layers as faithful. CE source anchor:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  - [x] Add a `CeMgdi` window-bitmap scaffold with per-window viewport,
    system clip, and live DC count, fed from GWE window-state publication.
    Current source anchors: `src/ce_mgdi.h` and
    `src/coredll_named_dispatch.cpp`.
- [ ] Keep host framebuffer and host windows as final rendering/presentation
  backends, not as owners of guest-visible clipping semantics.

## Phase 6: Encoded MIPS CE Kernel-Call Dispatch

- [x] Add decode helpers for high-address MIPS CE API-call thunks such as the
  observed `0xfffff3fa` path.
  CE source anchors:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/INC/nkmips.h:95`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/KERNEL/process.c:356`.
  Current source anchors: `src/main.cpp`, `src/ce_kernel.cpp`.
- [x] Dispatch decoded process/thread termination calls through `CeKernel`
  instead of reporting a bad guest PC when the target is a valid CE API-call
  encoding.
  Current source anchors: `src/main.cpp`, `src/synthetic_dll.cpp`,
  `src/ce_kernel.cpp`.
- [x] Preserve the project rule that `PC == 0` is fatal unless a specific
  decoded CE kernel-call path proves a legitimate guest exit.
  Current source anchor: `src/main.cpp`.

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

## Cross-Cutting: Audio Wait Fairness

- [ ] Preserve CE's asynchronous wave-output behavior while fixing wait
  fairness. CE `WODM_WRITE` queues a prepared `WAVEHDR` and returns without
  playing the whole buffer inline:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/wavemain.cpp:396`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.cpp:130`.
- [ ] Model audio completion as an asynchronous virtual-kernel/device event,
  not as a reason to stall global guest progress. CE driver output work is
  performed by separate rendering/output threads and completion marks
  `WHDR_DONE`, clears `WHDR_INQUEUE`, and reports `WOM_DONE`:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/hwctxt.cpp:1198`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/hwctxt.cpp:1575`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.h:93`.
- [ ] Audit the current host-audio shim and wait loop before changing
  behavior. Current `waveOutWrite` already returns immediately for the observed
  11.2s buffer, but `WaitForSingleObject` on the audio event blocks inside the
  coredll handler, which can serialize initialization that CE would let other
  guest threads/GWE work continue around. Current source anchors:
  `src/coredll_host_audio.cpp:539`,
  `src/coredll_named_dispatch.cpp:1849`, and
  `src/coredll_thread_runtime.cpp:252`.
- [ ] Do not fake success by completing audio immediately or skipping playback.
  The target direction is scheduler-aware guest waiting: the waiting guest
  thread blocks, while the virtual CE kernel/GWE scheduler continues runnable
  guest work until the audio completion event becomes signaled.

## Cross-Cutting: Named CE Constants

- [ ] Replace touched raw special numbers with existing named constants,
  enums, or local `constexpr` values. Keep source references beside behavior
  migrations so later work can tell CE-defined values apart from emulator-local
  thresholds.
  - [x] Name the touched GDI stock-object IDs and `BITMAP` metadata byte count
    in the MGDI bitmap/DC path. Current source anchors:
    `src/coredll_bitmap.cpp` and `src/coredll_named_dispatch.cpp`.
- [ ] Prioritize naming constants in wait/scheduler, GWE message queue,
  encoded-kernel-call, audio callback, and MGDI clipping code as those areas
  are migrated.
