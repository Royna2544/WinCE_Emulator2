# Progress

Last refreshed: 2026-05-31.

## Current Baseline: CE Coredll/GWE Behavior Versus Emulator Behavior

This file was reset to capture the current modeling gap against the Windows CE
6.x source tree at `/home/royna/WinCE-src_20201004`, especially
`PRIVATE/WINCEOS/COREOS`. Older route, GPS, remote-server, and performance
history was intentionally removed so this memory tracks the present GWE/coredll
modeling work.

## CE Coredll Boundary

- CE coredll is a client/runtime boundary into kernel and GWE API sets, not the
  owner of USER/GDI behavior. `UpdateAPISetInfo` loads the Win32, window
  manager, GDI, filesys, and device-manager API function tables from the system
  API set table: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/coredll.cpp:41`.
- The GWE API set exposes classic window/message calls including
  `CreateWindowExW`, `PostMessageW`, `SendMessageW`, `GetMessageW`,
  `TranslateMessage`, and `DispatchMessageW`: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:71`.
- The same GWE API set exposes paint/window lifecycle calls such as
  `BeginPaint`, `EndPaint`, `GetDC`, `ReleaseDC`, `DestroyWindow`,
  `ShowWindow`, and `UpdateWindow`: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:350`.

Current emulator difference:

- The emulator implements coredll, GWE-like window/message behavior, GDI/DC
  state, scheduling, host presentation, and several device shims inside
  `SyntheticDllRuntime`. This is acceptable for emulation, but it means CE's
  subsystem boundaries are currently flattened.
- Initial build-only `CeKernel`, `CeGwe`, and `CeMgdi` scaffolds were added
  and wired into `iNavi_Unicorn_Emulator.vcxproj` before the staged runtime
  migrations below. The 2026-05-30 Release build passed with one pre-existing
  Boost Beast warning from `remote_server.cpp`.
- `GuestHandle`, the guest handle table, and the next-handle counter now live
  behind `CeKernel`. `SyntheticDllRuntime` still orchestrates handle cleanup
  for windows, DCs, host resources, mappings, and guest threads while later
  migration phases move those owners into their CE-shaped subsystems. The
  2026-05-30 Release build passed after this migration with the same Boost
  Beast warning from `remote_server.cpp`.
- `GuestCpuContext`, `GuestThreadRunState`, `GuestThreadState`, and guest
  thread-record storage now live behind `CeKernel`. Active-thread scheduling
  state, parked main-thread context, and thread/process ID counters are still
  owned by `SyntheticDllRuntime` pending the next kernel migration step. The
  2026-05-30 Release build passed after this storage move with the same Boost
  Beast warning from `remote_server.cpp`.
- Active guest thread, parked main-thread context, scheduler cursor,
  pseudo-handles, main-thread TLS, and guest process/thread ID counters now
  also live behind `CeKernel`. `SyntheticDllRuntime` still owns the scheduling
  algorithms and Unicorn register capture/restore. The 2026-05-30 Release
  build passed after this state move with the same Boost Beast warning from
  `remote_server.cpp`.
- `CeKernel` now owns message-wait wakeup, runnable-thread detection, sleep
  expiry, wait-handle readiness decisions, and guest wait return-value
  assignment. `SyntheticDllRuntime` still refreshes host audio completion first
  and supplies the host wait-handle probe/logging adapter. The 2026-05-30
  Release build passed after this wait-refresh migration with the same Boost
  Beast warning from `remote_server.cpp`.
- Immediate `WaitForMultipleObjects` readiness now also routes through
  `CeKernel::queryWaitObjects`, so guest wait result selection uses guest
  handles and CE wait constants while the runtime supplies only a zero-time
  host wait probe. The 2026-05-30 Release build passed after this
  immediate-wait migration with the same Boost Beast warning from
  `remote_server.cpp`.
- The cooperative `WaitForSingleObject` scheduler path now routes immediate
  readiness through `CeKernel::queryWaitObject`, preserving sender/thread
  preference and host wait probing as runtime-supplied backing behavior.
  The 2026-05-30 Release build passed after this single-wait migration.
- The named-dispatch `WaitForSingleObject` path now also uses
  `CeKernel::queryWaitObject` for host-backed wait polling in its bounded wait
  loop and for guest thread/process readiness. The 2026-05-30 Release build
  passed after this named-dispatch wait migration.
- The named-dispatch `WaitForSingleObject` host polling/sleep loop was removed
  after interactive evidence showed startup audio/event waits could block the
  emulator. Host-backed waits are now zero-probed through
  `CeKernel::queryWaitObject`; normal guest waits park through the inline
  virtual scheduler. Parked waits now carry an explicit timeout result so
  finite `WaitForSingleObject`, `WaitForMultipleObjects`, and
  `MsgWaitForMultipleObjectsEx` waits resume with `WAIT_TIMEOUT`, while
  `Sleep` still resumes with zero. Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:1879`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1902`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:112`. The 2026-05-30
  Release build passed with zero warnings after this change.
- Named shared mappings now follow a more CE-shaped map/unmap model:
  `UnmapViewOfFile` syncs only changed named-view bytes, while explicit
  `FlushViewOfFile` remains the force-write path. This targets the observed
  `iNavi_sharedMem_traffic_static` map/unmap storm without special-casing that
  mapping name. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1273`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1333`.
  Current source anchor:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_mapping.cpp:180`. Bounded
  Release smoke runs wrote `captures/inavi_autodrive_20260530_235343` and
  `captures/inavi_autodrive_20260530_235445`; both captured startup without
  high-signal fatal/unsupported-ordinal log hits, but neither run exercised
  the full interactive remote-server button-stall path.
- Remote host input now preserves pointer-stream pairing when the emulator
  applies input backpressure. If a new host/remote `WM_LBUTTONDOWN` is rejected
  because an older pointer sequence is still queued, the matching release is
  dropped as part of that rejected host sequence instead of queuing an
  unmatched guest `WM_LBUTTONUP`. Queued-message preemption also now checks
  whether the oldest GWE message owner can actually be scheduled before
  yielding an active worker. Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1691` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:415`.
  The 2026-05-31 Release and Debug builds passed with the same Boost Beast
  warning from `remote_server.cpp`; Debug interactive validation is next.
  The latest remote run still suggests map UI update slowness is tied to long
  guest `message-transfer`/paint spans plus high-frequency
  `iNavi_sharedMem_traffic_static` map/unmap churn, not to forced unmap
  writes.
- Successful named-shared mapping hot-path diagnostics are now debug-level
  instead of info-level: repeated `CreateFileMappingW`, `MapViewOfFile`, and
  unchanged `UnmapViewOfFile` calls no longer flood the interactive log during
  map/traffic polling. Explicit `FlushViewOfFile`, failed mapping calls, and
  actual sync writes remain visible. Current source anchor:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_mapping.cpp:278`. The
  2026-05-31 Release and Debug builds passed with zero warnings after this
  logging-hygiene change.
- Remote-server audio is enabled by default for `tools/autodrive_inavi.ps1`
  remote runs unless `-NoRemoteAudio` is passed. Remote PCM is now queued only
  while at least one audio websocket client is connected; the queue is cleared
  when the first client connects and again when the last client disconnects, so
  late clients do not hear delayed startup audio. Audio websocket delivery is
  also event-driven instead of sleep-poll-only, and the audio websocket socket
  disables Nagle so short click sounds are not coalesced into delayed TCP
  bursts. The live websocket audio queue is capped to a small latency window,
  so long guest wave buffers cannot become a multi-second delay line ahead of
  later click sounds. Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/tools/autodrive_inavi.ps1:20` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:759`. The
  2026-05-31 Release builds passed with the existing Boost Beast warning from
  `remote_server.cpp`.
- Guest-visible `waveOut` timing now has a virtual CE-shaped owner. `CeAudio`
  queues copied guest `WAVEHDR` PCM, computes buffer completion from
  `nAvgBytesPerSec`, and marks headers done/signals guest events from the
  virtual timeline instead of letting host WinMM callbacks own CE completion.
  The websocket audio endpoint now taps the active `CeAudio` timeline; a
  client connecting mid-buffer starts near the current playback offset instead
  of hearing stale startup PCM or waiting for a future `waveOutWrite`. Host
  WinMM is opened with `CALLBACK_NULL`, so local playback remains a backend
  rather than the guest-visible clock. Local host playback is now fed through
  a small backend chunk queue instead of one whole guest buffer submission, so
  a long startup sound should not sit inside WinMM as a single giant backend
  buffer. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:944`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/wavemain.cpp:396`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.cpp:130`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.h:93`.
  Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_audio.h:10`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_host_audio.cpp:135`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:1022`.
  A follow-up live report found the backend/tap refactor could produce
  distorted buzzing. Two source-aligned transport bugs were fixed: the local
  WinMM backend now keeps each copied backend buffer alive until WinMM marks
  the `WAVEHDR` done/unpreparable instead of freeing it after an estimated
  sleep, and the websocket tap no longer falls back to sending raw guest PCM
  while advertising the configured remote format if miniaudio conversion
  fails. A later live run still had audible pops, which pointed to gaps from
  stitching many small local WinMM chunks. Local host playback now submits one
  copied backend buffer per guest `waveOutWrite` while still waiting for
  `WHDR_DONE` before freeing it; websocket audio remains a live `CeAudio`
  timeline tap. The 2026-05-31 Release and Debug builds passed with zero
  warnings after this adjustment, and Debug interactive run
  `captures/inavi_autodrive_20260531_105702` was relaunched on
  `192.168.0.39:8765` for validation.
- CE GWE/MGDI source comparison for the route-guide visual artifacts found
  missing region/rounded drawing exports in the emulator boundary:
  `RoundRect`, `FillRgn`, `SetRectRgn`, `SelectClipRgn`, `PtInRegion`, and
  `RectInRegion`. The coredll export table now exposes those CE ordinals, the
  dispatcher implements bounds-backed region/clip behavior plus polygonal
  `RoundRect`, and framebuffer polygon fills now honor the MGDI effective clip.
  This targets the observed rectangular black backing behind rounded route
  overlays without adding route-specific drawing. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:2006`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:2026`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:2716`.
  Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll_modules.cpp:146`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:151`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:877`.
  The 2026-05-31 Release build passed with zero warnings before the later
  audio transport edit, and the combined Release build passed afterward with
  the existing Boost Beast warning.
- A route-guide run from the old Debug process showed a source-alignment bug
  in the cooperative blocking-wait continuation: while pumping paint around
  `WaitForSingleObject(..., INFINITE)`, the continuation re-dispatched the
  host coredll wait shim and returned `WAIT_TIMEOUT` even though CE would keep
  the wait blocked until the object signaled. The same log immediately showed
  `waveOutUnprepareHeader` failing while `WHDR_INQUEUE` was still set, and
  dialog/window transitions later desynced between the host presenter and the
  remote framebuffer. The continuation now records finite deadlines, only
  returns `WAIT_TIMEOUT` for zero/expired finite waits, and keeps infinite
  waits parked while host messages/other runnable guest threads continue.
  Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1510` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:718`. The 2026-05-31
  Release build passed with zero warnings after this fix.
- Remote input press/release now wakes a window-owner thread parked in a
  non-`waitAll` multi-object wait when that owner has pending GWE messages.
  This models CE `MsgQueue::m_hNewEvents` enough to avoid button-up messages
  sitting behind unrelated worker/event pulses. Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:148` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:345`. CE
  reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:590`.
- A later remote run showed `WM_LBUTTONUP` could be retrieved quickly but the
  scheduler still spun through repeated `queued-message-preempt` worker yields
  while pending input/paint work remained. The active-thread yield path now
  keeps the active worker visible to `switchToRunnableGuestThread`, letting the
  existing owner-priority scheduler save that worker and choose the actual GWE
  message owner instead of restoring a parked main context first. This follows
  CE's owner-thread `MsgQueue` model (`m_hthdOwner`, `m_hNewEvents`, and
  input queues). CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:568`.
  Current source anchor:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:569`.
  The 2026-05-31 Release build passed with zero warnings.
- Host-backed serial open no longer retries a missing/unavailable Win32 COM
  port inside the guest `CreateFileW` path. A mapped `win32_com` backend gets
  one immediate host-open probe; if unavailable, the guest still receives the
  virtual CE serial device and its no-data wait semantics. This removes the
  observed ~1.75s startup delay around unavailable `COM7:`/host `COM21`
  without special-casing that device. Current source anchor:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:781`. The
  2026-05-31 Release build passed with zero warnings.
- `CeGwe` now shadows enough CE window-stack state for modal/owner input
  routing: parent, inferred non-child owner, root, z-order, visible, enabled,
  and destroyed state. `Create`/`ShowWindow`/`SetWindowPos` publication feeds
  that state, host pointer hit testing now asks `CeGwe::hitTest`, and hide,
  destroy, and disable paths purge queued pointer messages/captures for the
  whole owner stack. Owner/root repaint is queued after stack members are
  hidden or destroyed, visible popups are repainted above their owner/root,
  and owner-stack popup hides no longer restore stale saved backing over newer
  owner/root paint. After live observation showed lower map UI could still
  paint above a fullscreen popup, CE source comparison confirmed the remaining
  difference: `TopOwnedWindows`/z-order is coupled to `CalcVisRgn`, so lower
  owner/root windows should not retain drawable visible regions while higher
  popups cover them. A quick full-rectangle visible-clip experiment was backed
  out after a transition/not-responding regression; the follow-up needs real
  region subtraction before MGDI/DC system clips use it as paint truth.
  This follows CE GWE top-owned-window and visible/enabled filtering plus MFC
  modal owner disabling/drop-mouse behavior. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:906`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:930`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1131`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1142`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1352`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:1594`.
  MFC references:
  `/mnt/c/Program Files (x86)/Microsoft Visual Studio 8/VC/ce/atlmfc/src/mfc/dlgcore.cpp:694`
  and
  `/mnt/c/Program Files (x86)/Microsoft Visual Studio 8/VC/ce/atlmfc/src/mfc/wincore.cpp:4771`.
  Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:56`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:808`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:823`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1311`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1693`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window.cpp:126`. The
  2026-05-31 Release and Debug builds passed with zero warnings. Fresh
  interactive Debug runs were launched at
  `captures/inavi_autodrive_20260531_093953` and
  `captures/inavi_autodrive_20260531_094420` with remote server listening on
  `192.168.0.39:8765`. Live observation still reports weird z-order/layering
  and possible black-fill flashes on hide/destroy, so this is a partial
  owner-stack migration rather than a completed visible/update-region model.
  The click-sound websocket burst is also still open and needs a focused audio
  publish/send-boundary pass.
- Follow-up interactive evidence showed the newer symptom was late map/nearby
  UI paint rather than pure z-order: host input could be queued quickly, but
  the guest did not retrieve the target focus/down/up for about 2.6 seconds
  while same-window paint/lifecycle work was ahead of input. The queueing path
  now keeps create/size/show ahead of input but no longer places pointer input
  behind `WM_ERASEBKGND`/`WM_PAINT`, matching CE's model where paint is driven
  by update regions after higher-priority queue work. The older fullscreen
  popup retirement path also no longer restores its saved backing before
  hiding it under a newer fullscreen popup, because backing is a cache rather
  than CE z-order truth. Current source anchors:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1436` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1907`.
  The 2026-05-31 Release and Debug builds passed with zero warnings, and a
  fresh Debug interactive run was launched at
  `captures/inavi_autodrive_20260531_100056` with the remote server listening
  on `192.168.0.39:8765`.
- `CeGwe` now owns the `GuestMessage` record type and backing message deque.
  `SyntheticDllRuntime::guestMessages_` remains a compatibility alias to that
  deque, so this Phase 3 scaffold step should preserve delivery behavior while
  later commits replace direct deque operations with named `CeGwe` queue APIs.
  The 2026-05-30 Release build passed after this GWE message-storage migration
  with the same Boost Beast warning from `remote_server.cpp`.
- `CeGwe::postMessage` now wraps append posting, and simple focus/foreground
  message producers in `coredll_gui.cpp` and `coredll_window.cpp` use it while
  preserving the existing queue order. The 2026-05-30 Release build passed
  after this queue-API batch with the same Boost Beast warning from
  `remote_server.cpp`.
- Remaining append-style message producers now call `CeGwe::postMessage`,
  including window runtime repaint/show/input posts, named-dispatch posts, and
  remote input posts. Direct deque insert/erase/filter/dequeue operations remain
  for later named queue APIs. The 2026-05-30 Release build passed after this
  append-post migration with the same Boost Beast warning from
  `remote_server.cpp`.
- Front and priority queue insertion now routes through `CeGwe::postFront`,
  `postAfterLeadingMatches`, and `postBeforeFirstMatch`. This preserves the
  current popup-priority, input-priority, and synchronous-send insertion order
  while moving insertion ownership behind GWE. The 2026-05-30 Release build
  passed after this insertion migration with the same Boost Beast warning from
  `remote_server.cpp`.
- Simple queued-message pruning now routes through `CeGwe::eraseIf`, including
  popup mouse-move pruning, retired-popup message pruning, focus/destroy input
  pruning, and pending-message cleanup during destroy. The 2026-05-30 Release
  build passed after this erase migration with the same Boost Beast warning
  from `remote_server.cpp`.
- First-match queue peek/dequeue operations now route through
  `CeGwe::firstMatching`, covering blocking paint dispatch and
  `GetMessageW`/`PeekMessageW` filtering while preserving the current match
  predicates and remove flags. The 2026-05-30 Release build passed after this
  dequeue migration with the same Boost Beast warning from `remote_server.cpp`.
- MIPS CE old encoded kernel-call thunks now decode through `CeKernel` before
  the `PC == 0` fatal path. The observed target `0xfffff3fa` matches CE's
  old directly-linked `TerminateProcess` encoding for `SH_CURPROC` method 2,
  so this path now terminates the current virtual CE process instead of
  reporting an invalid guest PC. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/INC/nkmips.h:95`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/KERNEL/process.c:356`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/main.cpp:139`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/main.cpp:1105`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:195`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:385`.
  The 2026-05-30 Release build passed with the same Boost Beast warning from
  `remote_server.cpp`. Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_183949`; `DeviceParser.exe` logged the
  decoded CE kernel exit instead of the previous fatal zero-PC crash, and the
  log scan found no new zero-PC fatal or unsupported coredll ordinal lines.
  A later Debug disassembly of `DeviceParser.exe` showed the old CE
  `TerminateProcess` thunk receives `a0=0x42` as the process handle and the
  actual process exit code in `a1`; the decoder now reports and stores `a1`
  as the exit code. Debug evidence:
  `captures/inavi_autodrive_20260530_185730/child_17980_1_DeviceParser.exe.stdout.log`.
- Phase 3 compatibility alias cleanup is complete: runtime code no longer
  touches `guestMessages_` directly. Message counts, emptiness checks,
  scans, extraction, reverse erasure, first-match dequeue, and posting now go
  through named `CeGwe` queue helpers. This is still one backing deque, not yet
  CE-style per-thread `MsgQueue` ownership. Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp`.
  The 2026-05-30 Release build passed with the same Boost Beast warning.
  Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_184459`; startup window capture still
  completed, and `DeviceParser.exe` still followed the decoded CE kernel exit
  path.
- `CeGwe` now has a buildable thread-queue/window-owner registry scaffold.
  Created and imported guest windows register their current owner thread, and
  destroyed windows unregister from GWE. Message delivery still uses the
  existing flat backing queue, so this is a behavior-preserving step toward
  CE-style per-thread `MsgQueue` ownership. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:821`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:3435`.
  The 2026-05-30 Release build passed with the same Boost Beast warning.
  Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_185114`; startup window capture completed
  and the high-signal log scan found no new zero-PC fatal, hard-error, or
  unsupported coredll ordinal lines.
- `CeGwe` now exposes semantic posting entry points for posted, input, thread,
  and timer messages. These still feed the existing backing queue, but callers
  no longer have to express all queue producers as generic posts. This prepares
  the later split into CE-style posted/input/timer/sent queues without changing
  delivery order yet. Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp`.
  The 2026-05-30 Release build passed with the same Boost Beast warning.
  Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_185430`; startup capture completed and
  the high-signal log scan found no new zero-PC fatal, hard-error, unsupported
  coredll ordinal, or deadlock lines.
- `CeGwe` now assigns internal queue IDs and mirrors messages into
  owner-thread lanes for posted, sent, input, timer, and thread messages while
  preserving the current flat dispatch queue as the behavioral source of
  truth. This is a scaffold step toward CE `MsgQueue` ownership, not a message
  ordering change. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:16`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:38`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2195`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:3721`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1858`.
  The 2026-05-30 Release build passed with the same Boost Beast warning.
  Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_191908`; startup capture completed, and
  the high-signal log scan found no new hard-error, unsupported coredll
  ordinal, or false zero-PC success lines. `DeviceParser.exe` still exited via
  the decoded CE kernel path.
- `CeGwe` now exposes owner/lane query helpers and an owner-filtered
  first-match API for the next `GetMessageW`/`PeekMessageW` migration step.
  This does not change dispatch behavior yet; it gives runtime code a
  CE-shaped query surface before selection is switched away from the flat
  queue. Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:70`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:76`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:91`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:213`.
  The 2026-05-30 Release build passed with the same Boost Beast warning.
- `GetMessageW`, `GetMessageWNoWait`, and `PeekMessageW` now use
  `CeGwe::firstMatchingForOwner` with the active guest thread, or the main
  pseudo-thread when no guest thread is active. The selection still walks the
  existing flat queue to preserve current ordering inside that owner context,
  but cross-owner messages no longer match a different thread's retrieval call.
  Touched raw remove/filter constants were named locally. Current source
  reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:3790`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_192512`;
  startup capture completed, the high-signal log scan found no new hard-error
  or unsupported coredll ordinal lines, and `DeviceParser.exe` still exited via
  the decoded CE kernel path.
- Message-wait wakeup is now owner-aware. `CeKernel` accepts a GWE-supplied
  queue probe before waking `WaitingForMessage` guest threads, and active
  guest-thread `GetMessageW` parks only when that thread's owner queue is
  empty instead of checking the global queue. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.h:137`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:38`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:246`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2093`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_193402`; `00_initial.png`
  was captured at 816x519, the high-signal log scan found no new unsupported
  coredll ordinal, hard-error, invalid mapping, false zero-PC, or deadlock
  markers, and `DeviceParser.exe` still exited via the decoded CE kernel path.
- `MsgWaitForMultipleObjectsEx` is now registered at CE coredll ordinal 871
  (`0x0367`) and routed through the virtual wait/message boundary. Immediate
  handle readiness uses `CeKernel::queryWaitObjects`; immediate message
  readiness uses `CeGwe::hasMessagesForOwner`; blocking guest threads store
  both handle waits and message-wait wake state so later queue posts can make
  them runnable. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:1843`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:802`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll_modules.cpp:131`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2049`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.h:86`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:135`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_194133`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- Cross-thread `SendMessageW` queueing now applies to any different
  window-owner queue, not only active guest-thread sends to the main
  pseudo-thread. The sender is parked as `WaitingForSendMessage`, the sent
  message is queued through `CeGwe`, the scheduler prefers the receiver owner
  when it is a guest thread, and the existing message-transfer completion path
  writes the window-proc result back to the sender. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:897`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1601` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2300`.
  The 2026-05-30 Release build passed with no warnings after this narrow
  change. Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_194442`; `00_initial.png` was captured at
  816x519 and the high-signal log scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- Main-thread queued-message slices now preserve the current readable guest
  CPU context when the interactive watchdog stops a slice, and the queue loop
  restores the parked main context only when the current PC is missing or
  unreadable. This matches the CE/MFC model where `PeekMessage`/`GetMessage`
  pumping continues on the owning message queue thread instead of rewinding to
  a stale parked context. CE/MFC references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:924`,
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\thrdcore.cpp:620`,
  and
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\wincore.cpp:4715`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:2100`
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:2231`.
  The 2026-05-30 Debug build passed after stopping stale kept-alive Debug
  emulator processes, and the 2026-05-30 Release build passed with no
  warnings. Fresh interactive Debug autodrive wrote
  `captures/interactive_debug_20260530_215815/inavi_autodrive_20260530_215816`;
  `00_initial.png` was captured at 816x519, the main process stayed alive past
  the previous queued-message `PC == 0` crash, and logs reached modal
  `TGNaviDlg` create/destroy plus continued dispatch.
- Phase 4 window-region scaffolding has started. `CeGwe` now carries a
  GWE-owned window-region shadow with window, client, visible, update,
  client-visible, and client-update rectangles. Existing window publication
  feeds geometry/visibility into that shadow, `InvalidateRect` marks update
  regions there, and `ValidateRect` clears them there; `BeginPaint` still uses
  the existing paint behavior until the next migration step. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:54`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:773`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2417`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:159`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_194910`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- Paint APIs now consume the new GWE update-region shadow in the first narrow
  path: `BeginPaint` writes the GWE client-update rectangle to `PAINTSTRUCT`
  when one exists and falls back to the previous full-window rectangle
  otherwise; `EndPaint` clears the GWE update region. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:350`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:40`.
  The 2026-05-30 Release build passed with no warnings after this narrow
  change. Bounded autodrive with the companion enabled wrote
  `captures/inavi_autodrive_20260530_195145`; `00_initial.png` was captured at
  816x519 and the high-signal log scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- Input hit testing now uses GWE visible-region state for the rectangle
  decisions it can safely own today. The GWE window-region shadow is refreshed
  with absolute rectangles for all non-external guest windows whenever window
  state is published, and `windowAtPoint`/pointer capture validity now query
  `CeGwe::visibleRectForWindow` or `visibleRegionContainsPoint` instead of
  using only `GuestWindow` size/visibility flags. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:160`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:773`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1599`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_200122`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC,
  modal/input discard, or deadlock markers.
- `CeMgdi` now owns the first DC-state shadow. Existing `GuestDc` remains the
  drawing source of truth, but DC creation/destruction and state changes now
  mirror into `CeMgdi::DcState`: selected brush/pen/font/bitmap, text and
  background colors/mode/alignment, current drawing position, and a system
  clip seeded from GWE visible rectangles for window DCs. This is a scaffold
  step toward CE MGDI clipping; drawing paths do not enforce the MGDI clip
  yet. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:203`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:17`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:253`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:69`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2565`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_200752`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- The central framebuffer drawing helpers now consult the MGDI system clip for
  window DCs. Rectangle fills and bitmap blits intersect their output bounds
  with the clip; line drawing, `StretchDIBits`, and transparent-image writes
  point-check against the same clip before touching the framebuffer/backing
  path. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:203`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:41`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:555`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:464`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:474`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:539`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1444`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1752`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:2140`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_201529`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `CeMgdi` now also owns a bitmap metadata shadow. The existing `GuestBitmap`
  map still owns pixel storage and feeds drawing, but stock/default bitmap,
  `CreateBitmap`, `CreateDIBSection`, `CreateCompatibleBitmap`,
  `SetDIBColorTable` palette-size changes, and bitmap deletion now mirror
  width, raw height, bpp, stride, guest bits pointer, RGB masks, palette-entry
  count, and stock state into `CeMgdi::BitmapState`. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:37`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:170`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:281`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:379`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1327`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1408`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:532`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2652`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_201929`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `CeMgdi` now has a CE-shaped window-bitmap scaffold for each published guest
  window: viewport rectangle, system clip rectangle, and live DC count. The
  state is fed from the same absolute window rectangles used by GWE
  publication and removed when a guest window is destroyed. This is still a
  scaffold; saved backing pixels remain runtime-owned. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:51`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:211`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:239`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:792`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_202249`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `GetObjectW` for bitmap handles now reports width, absolute height, stride,
  bpp, and guest bits pointer from `CeMgdi::BitmapState` instead of reading
  the runtime `GuestBitmap` map directly. Pixel operations still use
  `GuestBitmap` storage until the later MGDI pixel-storage migration. CE
  reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1335`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_202549`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `SetBitmapBits` now uses `CeMgdi::BitmapState` for bitmap existence,
  storage byte count, and guest bits pointer. Pixel bytes still live in guest
  memory and drawing paths still use `GuestBitmap` until the next pixel
  operation migrations. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:68` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1428`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_203308`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `SetDIBColorTable` now uses `CeMgdi::BitmapState` for selected-bitmap
  existence, bpp validation, and palette bounds. The palette vector itself
  still lives in `GuestBitmap` until the later palette/pixel-storage migration.
  CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1391`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_203731`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `DeleteObject` now reads stock-bitmap protection from `CeMgdi::BitmapState`
  before host bitmap deletion, MGDI bitmap-state teardown, guest bitmap-map
  erase, and guest allocation release. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2652`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_204959`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `SelectObject` now validates bitmap selection through `CeMgdi::BitmapState`
  before updating both runtime DC state and the MGDI selected-bitmap shadow.
  CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2598`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_205207`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `SetDIBitsToDevice` and `StretchDIBits` now read their destination selected
  bitmap from `CeMgdi::DcState` before choosing the existing bitmap or
  framebuffer drawing backend. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1470` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2888`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_205431`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `TransparentImage` now reads source and destination selected-bitmap handles
  from `CeMgdi::DcState` before choosing the existing bitmap/framebuffer blit
  backend. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2927`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_205706`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `BitBlt` and `StretchBlt` now read source and destination selected-bitmap
  handles from `CeMgdi::DcState` before choosing the existing
  bitmap/framebuffer blit backend. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:3104`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_205937`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `CeMgdi::selectedBitmapForDc` now centralizes selected-bitmap lookup from
  the MGDI DC shadow. Rectangle fills, line drawing, polygon fills, host text
  drawing, `GetPixel`, and `SetDIBColorTable` use that accessor before falling
  back to the existing bitmap or framebuffer backends. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:103`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:679`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:114`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_210843`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `CeMgdi::BitmapState` now mirrors the bitmap palette vector, and
  `SetDIBColorTable` mutates that MGDI palette first before copying it back to
  `GuestBitmap` for the current pixel readers. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:50` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1399`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_211147`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- `GetPixel` now overlays the palette from `CeMgdi::BitmapState` before
  decoding indexed bitmap pixels, while still reading bitmap bytes from guest
  memory through the existing compatibility bitmap record. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:125`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_211429`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `syncBitmapPaletteFromMgdi` now refreshes the compatibility `GuestBitmap`
  palette from `CeMgdi::BitmapState` before indexed bitmap reads/writes in
  rectangle fills, line drawing, polygon fills, host text drawing, and
  `GetPixel`. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:1393` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1079`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive with the companion
  enabled wrote `captures/inavi_autodrive_20260530_211738`; `00_initial.png`
  was captured at 816x519 and the high-signal log scan found no new
  unsupported coredll ordinal, hard-error, invalid mapping, false zero-PC, or
  deadlock markers.
- DIB-to-bitmap, `TransparentImage`, `BitBlt`, and `StretchBlt` paths now call
  `syncBitmapPaletteFromMgdi` for selected source/destination bitmaps before
  indexed pixel reads/writes. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1493` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2891`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive with
  the companion enabled wrote `captures/inavi_autodrive_20260530_212021`;
  `00_initial.png` was captured at 816x519 and the high-signal log scan found
  no new unsupported coredll ordinal, hard-error, invalid mapping, false
  zero-PC, or deadlock markers.
- `CeMgdi` now exposes selected brush, pen, and font accessors from the DC
  shadow. Polygon, polyline, ellipse, rectangle, `PatBlt`, `LineTo`, and host
  text drawing use those selected-object reads, and `SelectObject` returns the
  previous object from the MGDI DC state before updating the runtime
  compatibility copy. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:111`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:565`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:963`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_212649`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- `CeMgdi` now also exposes text color, background color, background mode,
  text alignment, and current drawing position accessors from the DC shadow.
  Host text drawing reads its color/mode state from MGDI, text-state setters
  return the previous MGDI value, `MoveToEx` writes the previous MGDI current
  position, `LineTo` starts from that MGDI position, and polygon/polyline
  drawing now updates the MGDI current-position mirror. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:126`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1009`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2697`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_213043`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- `CeMgdi` now mirrors brush, pen, and font metadata, including brush color and
  pattern bitmap, pen style/width/color, font `LOGFONT` bytes, and stock-object
  flags. Object creation updates the MGDI mirror, pattern brushes publish their
  source bitmap, host text font selection reads `CeMgdi::FontState`, and
  brush/pen/font `DeleteObject` stock protection and teardown go through MGDI
  before updating the runtime compatibility maps. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:55`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:314`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2624`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_213425`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- Brush/pen drawing metadata reads now go through `CeMgdi` object state.
  `Polygon`, `Polyline`, `Ellipse`, `FillRect`, `PatBlt`, `Rectangle`, and
  `LineTo` read brush color/pattern and pen style/color from the MGDI mirror
  while keeping the existing pixel writers as the backend. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:567`.
  The 2026-05-30 Release build passed with no warnings. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_213738`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- `CeMgdi` now mirrors region metadata as GDI object state. `CreateRectRgn`
  publishes simple bounds, `CombineRgn` refreshes the destination bounds from
  the host region box, region `DeleteObject` tears down the MGDI mirror, and
  successful `SetWindowRgn` ownership transfer removes the adopted region from
  MGDI as well as the kernel handle table. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:76` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2531`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_214126`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- `CeMgdi::effectiveClipForDc` now intersects system and app clip state, the
  framebuffer drawing helper uses that effective MGDI clip, and `GetClipBox`
  now reports that clip to the guest with bitmap/window fallbacks and CE-style
  null/simple region return codes. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_mgdi.h:268`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:500`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2582`.
  The 2026-05-30 Release build passed with the pre-existing Boost Beast
  warning from `remote_server.cpp`. Bounded autodrive wrote
  `captures/inavi_autodrive_20260530_214532`; `00_initial.png` was captured at
  816x519 and the high-signal capture scan found no new unsupported coredll
  ordinal, hard-error, invalid mapping, false zero-PC, or deadlock markers.
- The touched MGDI bitmap/DC path now uses named local constants for GDI stock
  object IDs and the `BITMAP` metadata byte count instead of raw values in the
  migrated code. Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:124` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:30`.
  The 2026-05-30 Release build passed with no warnings after this cleanup.

## Threading And Message Queues

CE model:

- GWE message queues are per-thread/user-input context objects. The kernel knows
  the message queue TLS slot (`TLS_MSG_QUEUE_INDEX 0`): CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:23`.
- `MsgQueue` owns `MsgWaitForMultipleObjectsEx`, input queue posting, posted
  queue posting, `SendMessage`, `SendMessageTimeout`, `PostMessage`,
  `GetMessage`, `PeekMessage`, `TranslateMessage`, `DispatchMessage`, and
  `InSendMessage`: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.

Current emulator model:

- Guest threads are explicit saved Unicorn CPU contexts created by
  `createGuestThread`: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:101`.
- Wait/sleep completion is polled in `refreshSignaledGuestWaits`: current
  source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:254`.
- Scheduler switches are cooperative through `switchToRunnableGuestThread`:
  current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:410`.
- API-boundary yielding is limited to selected calls such as `Sleep`,
  waits, `PostMessageW`, `InvalidateRect`, `SetTimer`, `ShowWindow`,
  `CreateThread`, `ResumeThread`, and `CreateProcessW`: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:530`.
- The message representation is a compact `GuestMessage`, and posted work uses
  a runtime deque rather than a CE-style per-thread `MsgQueue`: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:662` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:912`.

Confirmed behavior difference:

- CE combines kernel waits, wake masks, send-message blocking, foreground/input
  state, and per-thread message queues. The emulator approximates those with
  cooperative runtime scheduling and a flatter message deque. This is the most
  important gap for UI fairness, cross-thread `SendMessageW`, modal ownership,
  and message wait behavior.

## Window Manager And Regions

CE model:

- `CWindow` has a creator process/thread, owning `MsgQueue`, WNDPROC pointer,
  styles, exstyles, window/client visible regions, window/client update
  regions, destroy/create flags, dialog/listbox/messagebox flags, owned-window
  grouping, and top-level position priority: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1328`.
- `CWindow` has explicit region and z-order algorithms such as
  `GetClientUpdateRgn`, `GetClientUpdateDC`, `SetVisibleRegion`,
  `SetUpdateRegion`, `GiveVisRgnToBehind`, `DistributeVisRgn`,
  `GiveUpVisRgn`, `StealVisRgnFromBehind`, `CheckForOpenDC`,
  `CalcVisRgn`, and `TopOwnedWindows`: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`.

Current emulator model:

- `GuestWindow` tracks hwnd, class/title, styles, parent, WNDPROC, owner
  thread, z-order, rectangle, visible/enabled/destroyed flags, paint bounds,
  and saved backing pixels: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:545`.
- `makeGuestWindow` creates a flat runtime window record and assigns owner
  thread/z-order directly: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:821`.
- Popup and repaint behavior is currently driven by queued messages,
  z-order/backing heuristics, and special handling for visible popups:
  current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1052`.

Confirmed behavior difference:

- CE's window manager is region-first and queue-owned. The emulator is
  z-order/backing-first. That is the primary difference behind modal/overlay
  routing risks, stale popup visuals, and under-layer hit-test leakage.

## Paint And Invalidation

CE model:

- `BeginPaint` and `EndPaint` are GWE API-set calls: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:350`.
- A window stores `m_hrgnVisible`, `m_hrgnUpdate`, `m_hrgnClientVisible`, and
  `m_hrgnClientUpdate`; client update is only valid while a DC using it exists:
  CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.

Current emulator model:

- `BeginPaint` creates a guest DC and currently writes a paint rectangle that
  covers the full guest window: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:40`.
- `ValidateRect` currently validates window existence rather than maintaining
  a real update region: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:159`.

Confirmed behavior difference:

- CE invalidation is region-based and intersects update, visible, and client
  regions. The emulator currently has paint scheduling and DC creation, but not
  first-class update-region semantics.

## GDI, DCs, And Window Bitmaps

CE model:

- MGDI `DC` tracks selected pen, brush, bitmap, font, app clip region,
  foreground/background colors, background mode, brush origin, actual device
  clip (`XCLIPOBJ`), DC type, save stack, and DC list: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:203`.
- `WBITMAP` is CE's internal GDI representation of a window. It does not map
  to an app-visible GDI object. All windows point to the same framebuffer bits,
  but each window has a viewport, system clip region, and DC list; changing the
  system clip updates all DCs for that window: CE reference
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.

Current emulator model:

- `GuestDc` tracks selected brush, pen, font, bitmap, text/background colors,
  background mode, text alignment, and an origin: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:584`.
- `GuestBitmap` tracks width, raw height, bpp, stride, bits, RGB masks,
  palette, and stock state: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:612`.
- `bitBltToFramebuffer` moves pixels directly into the host-backed framebuffer
  and consults saved backing layers manually: current source
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1743`.

Confirmed behavior difference:

- CE makes clipping a DC/window-system-clip invariant. The emulator can draw
  the needed pixels, but clipping and visible-region ownership are not yet
  first-class, so correctness depends on ad hoc checks around blits, backing
  pixels, z-order, and popup state.

## Latest Behavior Fixes: 2026-05-30

- Destroyed guest windows are no longer left as visible zombie entries in the
  cross-process guest-window registry. The synchronous `DestroyWindow` path now
  unregisters the GWE window state, tears down the MGDI window-bitmap shadow,
  clears saved backing, and republishes the removal after `WM_DESTROY` /
  `WM_NCDESTROY` completion. Child-window destruction now avoids restoring
  saved backing pixels; parent invalidation remains the repaint path. This
  follows CE's model where destroyed windows stop participating in visible and
  update regions rather than remaining as drawable/z-ordered windows. CE
  references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1352`.
  MFC reference:
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\wincore.cpp:1222`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1295`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:866`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1415`.
- MGDI line drawing now uses the selected pen width for `Polyline`, `LineTo`,
  `Polygon`, `Ellipse`, and `Rectangle` outlines. Width `0` remains a cosmetic
  one-pixel pen and wider pens are clamped through named raster constants. This
  addresses the route/polyline symptom where the app selected a wide pen but
  the emulator always rasterized one-pixel Bresenham strokes. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:203`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:481`.
  MFC references:
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\wingdi.cpp:661`,
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\wingdi.cpp:841`, and
  `C:\Program Files (x86)\Microsoft Visual Studio 8\VC\ce\atlmfc\src\mfc\wingdi.cpp:1274`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:33`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:581`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:659`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:624`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:2942`.
- `MapViewOfFile` now reuses and ref-counts exact views for the same named
  mapping object, and final `UnmapViewOfFile` releases the guest allocation
  backing the view. This fixes the leak pattern observed before the late
  `iNavi_sharedMem_traffic_static` heap exhaustion crash. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:948`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1111`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1273`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:676`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_mapping.cpp:303`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_mapping.cpp:370`.
  The 2026-05-30 Release build passed with the known Boost Beast warning.
  Bounded Release autodrive wrote `captures/inavi_autodrive_20260530_221145`;
  it captured a valid `816x519` startup frame and the high-signal log scan
  found no heap-exhaustion, `MapViewOfFile` allocation failure, unsupported
  coredll ordinal, false zero-PC success, hard-error, or UC_ERR crash lines.
- `CeDevice` now owns guest-visible serial-device state for virtual serial
  handles: guest name, mapped backend/type, host name, DCB-like mode,
  `COMMTIMEOUTS`, comm mask, queue sizes, last error, and no-data backend
  status. `CreateFileW`, close, `DeviceIoControl`, `GetCommState`,
  `SetCommState`, `SetCommTimeouts`, `SetCommMask`, `SetupComm`, `PurgeComm`,
  and `ReadFile` now consult or update that state while preserving the current
  `ReadFile TRUE + transferred=0` no-data behavior for this migration step.
  The old "disconnected serial-fallback" wording now reports a "virtual
  serial no-data backend" when the CE device remains intentionally open
  without host bytes. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/DRIVERS/SERDEV/serial.c`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/IR/IRCOMM/ircomm.c:792`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_device.h:25`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_device.cpp:8`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:734`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_comm.cpp:107`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_fs.cpp:512`.
  The 2026-05-30 Release build passed. Bounded Release autodrive wrote
  `captures/inavi_autodrive_20260530_224519`; it captured `00_initial.png`
  and the high-signal scan found no unsupported coredll ordinal, false
  zero-PC success, heap exhaustion, hard-error, UC_ERR, or old disconnected
  serial-fallback wording.
- Virtual serial `ReadFile` can now park active guest worker threads instead
  of completing an unlimited no-data success loop. `CeDevice` decides whether
  a no-data read is immediate, deadline-based, or indefinite from the stored
  CE-style `COMMTIMEOUTS`; `SyntheticDllRuntime` saves the active guest CPU
  context at the `ReadFile` return address, marks the thread
  `WaitingForSerialRead`, and lets the main/message context continue. Pending
  serial reads wake through the scheduler refresh path when remote serial
  bytes arrive or the read deadline expires, then write the transferred byte
  count and resume the guest thread with `ReadFile` success. CE references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/DRIVERS/SERDEV/serial.c`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/IR/IRCOMM/ircomm.c:792`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_device.h:44`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_fs.cpp:190`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:256`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2231`.
  The 2026-05-30 Release build passed with the known Boost Beast warning.
  Bounded Release autodrive wrote `captures/inavi_autodrive_20260530_225411`
  and captured `00_initial.png` with no high-signal crash/ordinal regressions.
  A longer Debug run wrote `captures/inavi_autodrive_20260530_225648`; it
  reached the COM7 setup path and recorded `SetCommTimeouts` with
  `ReadIntervalTimeout=4294967295`, `ReadTotalTimeoutMultiplier=0`, and
  `ReadTotalTimeoutConstant=1000`, the timeout shape the new parking path
  handles. That run did not reach an actual serial `ReadFile` before scripted
  shutdown, so the broader interactive UI responsiveness acceptance remains
  open for the owner-priority scheduler step.
- GWE owner-priority scheduling now runs before the generic runnable-thread
  round-robin. `CeGwe` exposes the oldest pending message owner from its
  mirrored owner lanes, and the scheduler restores the main pseudo-thread
  context or switches to the runnable owner thread before considering a
  preferred worker or ordinary worker slice. The handoff logs are rate-limited
  and include owner queue count, total queue count, and the previous preferred
  worker. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_gwe.h:205`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:414`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:493`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:878`.
  The 2026-05-30 Release build passed with the known Boost Beast warning.
  Bounded Release autodrive wrote `captures/inavi_autodrive_20260530_230158`
  and captured `00_initial.png` with no new high-signal crash/ordinal
  regressions. A longer Debug run wrote
  `captures/inavi_autodrive_20260530_230320`; it logged
  `guest scheduler owner-priority reason=ResumeThread target=main
  owner=0xfffffffe ownerQueued=63 totalQueued=63`, showing that pending main
  UI work was selected before the newly resumed worker batch. The full manual
  dialog-switching acceptance is still pending an interactive run.
- A Debug interactive run exposed a scheduler regression in the first
  owner-priority implementation: when pending messages belonged to the main
  pseudo-thread and the runtime was already returning from a main-thread API
  boundary, restoring the parked main context replayed stale `CreateThread`
  work until guest heap exhaustion. The scheduler now restores the parked main
  context only while yielding from an active guest worker; main-boundary
  returns fall through without replaying stale state. Current source
  reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:493`.
  The 2026-05-30 Debug build and Release build passed. Debug remote-server
  run `captures/inavi_autodrive_20260530_231358` was launched with
  `--remote-bind 192.168.0.39`, reached the map/UI path, and showed serial
  reads parking/waking on data instead of the previous loading freeze.
  Follow-up run `captures/inavi_autodrive_20260530_232138` added the
  completed scheduler guard: queued-message preemption now leaves an active
  guest worker running when that worker already owns the oldest pending
  message, and the old `pre-queued-worker` burst is skipped whenever GWE has a
  pending message owner. This removed the rapid `queued-message-preempt` bounce
  and worker-burst queue growth seen in the previous remote run. Remaining
  slow paint spans are still visible and should be investigated as rendering
  or remote-streaming cost, not the earlier scheduler storm.
- The same Debug remote-server run showed a pressed button could remain visually
  down after a tap. Log evidence at
  `captures/inavi_autodrive_20260530_232138/emulator.stdout.log:4451`
  through `:4471` showed the host/root `WM_LBUTTONUP` was queued before the
  synthetic child button-down was remembered, so the old deferred child-up
  mirror had no future release to attach to. The synthetic child-button bridge
  now detects an already queued ancestor/root `WM_LBUTTONUP` and inserts a
  matching child `WM_LBUTTONUP` before it, preserving CE/MFC-style
  down/up pairing without adding app-specific button behavior. Current source
  reference:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2448`.
- A Debug remote-server run for the full-screen return/bottom-bar redraw issue
  showed a full-screen popup could leave queued `WM_PAINT`/`WM_ERASEBKGND`/
  `WM_SHOWWINDOW` work behind after it was hidden, and the hidden popup could
  later recapture its saved backing. CE GWE models visible/update state on the
  window (`m_hrgnVisible`, `m_hrgnUpdate`) and has explicit
  clear/recalculate/invalidate hooks, so hidden windows should not keep stale
  update work as display truth. The runtime now discards queued update messages
  for a hidden window subtree on `ShowWindow(SW_HIDE)`/`SetWindowPos`
  `SWP_HIDEWINDOW`, and saved backing capture refuses hidden/destroyed
  windows. CE reference:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1123`.
  Current source references:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1081`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1241`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:3630`.
  The 2026-05-31 Release build passed with the known Boost Beast warning.
