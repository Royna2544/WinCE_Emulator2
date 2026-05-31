# Known Bugs

Last refreshed: 2026-05-31.

This file was reset to record behavior gaps explained by the CE 6.x
coredll/GWE source comparison. Older route, GPS, remote-server, and performance
entries were intentionally removed.

## Resolved: Old MIPS TerminateProcess Encoding Reached PC Zero

Symptom:

- `DeviceParser.exe` could stop with `PC == 0` after a high-address MIPS/CE
  kernel-call thunk targeting `0xfffff3fa`.

Evidence:

- Release smoke log:
  `captures/inavi_autodrive_20260530_182807/child_24848_1_DeviceParser.exe.stdout.log`.
- CE MIPS compatibility encoding:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/INC/nkmips.h:95`.
- CE process API set method 2 is `TerminateProcess`:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/KERNEL/process.c:356`.

Status:

- Fixed by decoding the CE old MIPS `TerminateProcess` API-call target through
  `CeKernel` while preserving fatal handling for undecoded `PC == 0` paths.

## Message Queue And Scheduler Semantics Are Too Flat

Symptom:

- UI work can be delayed or delivered in a different order than CE would use.
- Cross-thread/message-wait edge cases remain risky.
- Modal UI, worker-thread handoff, and queued repaint bursts can behave
  differently from real CE.

CE reference:

- Kernel-special message queue TLS slot:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:23`.
- `MsgQueue` owns `MsgWaitForMultipleObjectsEx`, input posting, posted queue
  posting, `SendMessage`, `PostMessage`, `GetMessage`, `PeekMessage`,
  `TranslateMessage`, `DispatchMessage`, and `InSendMessage`:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.

Current emulator reference:

- Guest thread creation:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:101`.
- Wait polling:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:254`.
- Cooperative switching:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:410`.
- API-boundary yield list:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:530`.
- Flat message record:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:662`.

Status:

- Known behavior gap. Build a per-thread/user-input-context message queue
  model before adding more app-specific diagnostics around stuck UI work.
- Current scaffold status: `CeGwe` now mirrors posted, sent, input, timer, and
  thread messages into owner-thread lanes. `GetMessageW`/`PeekMessageW` now
  use owner-filtered selection while preserving flat ordering inside that owner
  context, and message-wait wakeup now checks the waiting thread's owner queue
  before making it runnable. `MsgWaitForMultipleObjectsEx` now uses
  owner-aware message readiness, but wake masks are still category-coarse.
  Cross-thread `SendMessageW` now uses a sender-blocked queue/result path for
  any different window-owner queue; finer `InSendMessage`/timeout accounting
  remains future work if the app reaches those APIs. A Debug interactive run
  on 2026-05-30 also fixed the observed stale main-message-pump context
  replay: queued-message watchdog slices now save the current readable main
  context, and owner-priority main pseudo-thread restore only happens while
  yielding from an active guest worker. This avoids replaying stale
  main-thread API-boundary state and exhausting the guest heap during loading.
  A follow-up fix also avoids preempting an active guest worker when that
  worker is already the oldest pending message owner, and disables the legacy
  `pre-queued-worker` burst while GWE has a pending owner. This removed the
  rapid `queued-message-preempt` bounce and worker-burst queue growth seen
  after the first fix. Live remote input then showed a stuck pressed-button
  state when a synthetic child button-down was remembered after its ancestor
  `WM_LBUTTONUP` was already queued; the bridge now inserts a matching child
  `WM_LBUTTONUP` before that queued ancestor release. Evidence:
  `captures/inavi_autodrive_20260530_232138/emulator.stdout.log:4451`.
  Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2448`.
  A later remote-endpoint run showed another pointer-stream pairing bug: when
  a new down was rejected because an older touch was still queued, the matching
  host release was still queued as a guest `WM_LBUTTONUP`. The host input
  backpressure path now drops that matching release too, and queued-message
  preemption now requires a schedulable GWE owner. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1691` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:415`.
  A 2026-05-31 run then showed the release was queued but delayed about 1.85s
  because the window owner was parked in a non-message multi-object wait;
  `CeKernel` now wakes such owner waits when their GWE queue has pending input,
  modeling CE `MsgQueue::m_hNewEvents`.
  A later 2026-05-31 remote run showed the release could be retrieved quickly
  but the scheduler still bounced through repeated `queued-message-preempt`
  yields while pending UI work remained. The active-thread yield path now
  leaves the active worker visible to the owner-priority switcher so it can
  save that worker and select the real pending GWE owner, matching CE's
  owner-thread queue shape instead of restoring a parked main context first.
  Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:569`.
  The 2026-05-31 remote run
  `captures/inavi_autodrive_20260531_105702/emulator.stdout.log` showed a
  remaining CE mismatch after route/dialog interaction: ordinary posted or
  broadcast messages to a covered window can become undeliverable because
  `GetMessageW`/`PeekMessageW` rejects non-synchronous candidates when
  `coveringFullScreenOwnedPopup(candidate.hwnd)` is true. CE `MsgQueue`
  describes posted messages as queue-owned by the target window's thread and
  signals `m_hNewEvents`; MFC CE only drops mouse/syskey messages for disabled
  windows while a modal dialog is not yet shown. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:4146`.
  Evidence in the run: `PostMessageW(HWND_BROADCAST, 0x6ee, ...)` and
  owner-priority logs showed main owner queue growth, then repeated
  `queued-message-preempt` worker rotation. Later input was rejected with
  `discarded host mouse down while previous touch sequence is still queued`,
  because the previous pointer sequence remained behind the stuck queue.
  Fix in progress: `GetMessageW`/`PeekMessageW` now keeps ordinary
  posted/broadcast/custom messages deliverable through the owner queue, and
  only discards modal-covered mouse/syskey input on removing reads. This
  follows the CE/MFC distinction without allowing click-through to disabled or
  covered windows. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:4125`.
  Release build passed with zero warnings and bounded smoke
  `captures/inavi_autodrive_20260531_111213` found no fatal/unsupported/PC-zero
  signatures. The remote route/dialog button-stuck path still needs live
  validation.
  The bug remains open until the queue model, wake categories, and
  send-message edge cases are the behavioral truth.

### Bottom Bar Can Be Hidden After Full-Screen Popup Teardown

Symptom:

- The bottom bar window can be visible in the GWE/window state but missing or
  overwritten on the framebuffer after a full-screen owned popup is destroyed.

Evidence:

- Debug run:
  `captures/inavi_autodrive_20260531_162456/emulator.stdout.log`.
- The bottom bar `hwnd=0x00010143` is shown, receives text
  `양천구 목동`, and receives `WM_PAINT`, but later `DestroyWindow` for
  full-screen popup `0x00013e24` restores saved backing and queues exposed
  repaints from unordered map iteration.
- CE reference: paint requests are a separate GWE queue component and windows
  own visible/update regions rather than relying on container iteration order:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:471`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1352`.

Status:

- Mitigated in source on 2026-05-31 by queuing exposed repaint requests in
  visible-stack order: owner/root windows first, then visible children and
  overlays by stack depth and z-order. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1229`.
  Follow-up source now also clips parent/root framebuffer writes against
  visible higher z-order child/overlay windows, so delayed child `WM_PAINT`
  should not be required to undo owner/root pixels. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1727` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:474`. Release and
  Debug builds passed. Needs live validation in the next interactive run
  before this entry can be marked resolved.

### Current Search Freeze Signature

Symptom:

- During route/search, the app can appear frozen with the main owner parked in
  `WaitForSingleObject(0x10021, INFINITE)`.
- A sent message remains queued to the main owner while worker `0x124d6`
  continues long guest CPU slices.

Evidence:

- Debug run:
  `captures/inavi_autodrive_20260531_143011/emulator.stdout.log`.
- The earlier hard crash at the same point is fixed: worker `0x124d9` now
  returns through the CE thread-exit path instead of falling through to
  `PC == 0`.
- CE GWE reference: `MsgQueue` has separate posted, received-send, sent-stack,
  paint, and quit components, and `m_hNewEvents` signals new queue events:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:533`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:590`.

Status:

- Open, narrowed. The emulator now gives the worker larger CE-neutral CPU
  slices while main is legally parked, and the first CE-shaped received-send
  wait handling is implemented. A main-owned message transfer is no longer
  deferred solely because a main wait is parked, and main-thread
  `WaitForSingleObject` parking plus still-blocked wait continuations now
  dispatch queued received sent messages for that owner before yielding to
  workers. Debug run
  `captures/inavi_autodrive_20260531_160231/emulator.stdout.log` showed
  `ownerSent=1` followed by
  `WaitForSingleObject dispatching received sent message while main wait
  parked`; the same run then exposed a second still-blocked continuation case
  where worker `0x124df` repeatedly sent `msg=0x52e8` with `ownerSent=0`, which
  is now covered by the continuation-side received-send check. The remaining
  risk is posted-message backlog and/or genuine route CPU dominance, not the
  original dead received-send lane observed in
  `captures/inavi_autodrive_20260531_154920`.

## UI Can Stall During Host Waits Or Shared Mapping Storms

Symptom:

- Startup audio/event waits could block the emulator instead of only parking
  the waiting guest thread.
- During remote interactive testing, button release/paint could be delayed
  while `iNavi_sharedMem_traffic_static` was repeatedly mapped, force-written,
  and unmapped.

CE/MFC reference:

- MFC CE `CWnd::OnLButtonDown` delegates to default window handling after CE
  gesture recognition:
  `/mnt/c/Program Files (x86)/Microsoft Visual Studio 8/VC/ce/atlmfc/src/mfc/wincore.cpp:5246`.
- CE GWE owns capture/message routing:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:1297`.
- CE mapfile keeps map/unmap and explicit flush behavior separate:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1273`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/NK/MAPFILE/mapfile.c:1333`.

Current emulator reference:

- Named-dispatch `WaitForSingleObject` fallback:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_named_dispatch.cpp:1879`.
- Parked wait timeout bookkeeping:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_kernel.cpp:112`.
- Named shared mapping sync:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_mapping.cpp:180`.

Status:

- Partially fixed on 2026-05-30. The named-dispatch host wait sleep loop was
  removed, finite parked waits now carry explicit timeout return values, and
  named shared mapping unmap no longer force-writes unchanged whole views.
  Release build passed and bounded startup smokes captured frames. The bug
  remains open until a Debug interactive remote-server run verifies button
  release/paint responsiveness through the previous mapping-storm path. A
  later remote run confirmed forced unmap writes are gone, but map UI updates
  can still look slow while long guest `message-transfer`/paint spans coincide
  with high-frequency `iNavi_sharedMem_traffic_static` map/unmap churn. The
  successful named-shared mapping hot-path logs are now debug-level so normal
  info logging no longer amplifies that churn; the remaining question is how
  much of the visible delay is guest work versus emulator scheduling/rendering.
  The scheduler now treats `message-transfer` as CE/MFC-synchronous owner work:
  CPU-only transfers get larger slices to reduce hook/pump overhead, but
  input/presentation pressure still forces short responsive slices. Scheduler
  named-mapping sync is heartbeat-gated instead of running after every slice,
  and watchdog diagnostics report owner-lane counts and long transfer elapsed
  time. Release build and bounded startup/route-preset smokes passed without
  new main-emulator fatal/unsupported/false-PC-zero signatures; the live
  remote route-calculation path still needs validation because the script did
  not reproduce the long calculation span.
  Debug run `captures/inavi_autodrive_20260531_112632` also shows the host
  presenter lagging behind the remote framebuffer during long synchronous
  `UpdateWindow`/message-transfer spans; `hwnd=0x0001004c` paints take about
  1.1-1.3 seconds. Treat this as a host-present/paint-batching issue until CE
  visible/update-region comparison says otherwise. A separate parked-main wait
  regression is fixed: the scheduler no longer readiness-probes and then
  completes the same auto-reset host event in two calls, because the first
  zero-time host wait can consume the event. Debug run
  `captures/inavi_autodrive_20260531_123407` confirmed the long startup audio
  wait now completes after its virtual duration. Another log slice from the
  same run showed the host queued `WM_LBUTTONUP` quickly, but the guest owner
  thread stayed inside the preceding `WM_LBUTTONDOWN` message transfer for
  about 2.4 seconds. The interactive watchdog now timeslices message transfers
  instead of deferring all stops while `pendingMessageTransfers_` is non-empty;
  this should keep host/remote presentation responsive, but CE-style
  same-thread delivery still means `WM_LBUTTONUP` cannot be dispatched until
  the guest down handler yields or returns. Debug interactive run
  `captures/inavi_autodrive_20260531_130929` then exposed a separate
  source-mismatch regression: the synthetic blocking-wait continuation pumped
  queued paint/timer work while the guest was inside plain `Sleep`/wait, and
  the process crashed with `UC_ERR_MAP` in `mfcce400.dll+0x0001f53c` after
  route UI work. CE/MFC message pumping belongs to `GetMessage`/
  `DispatchMessage` and modal loops, not plain waits, so that synthetic
  reentrant paint pump is now disabled. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1345`. The bug
  remains open until fresh remote route validation confirms the host window and
  remote endpoint survive the same path. The same fresh run then showed route
  guide audio beating the corresponding fullscreen route/search UI transition
  by several seconds. Evidence:
  `captures/inavi_autodrive_20260531_131703/emulator.stdout.log:13959` starts
  a 2865 ms `waveOutWrite`; owner-priority logs show pending main-owner work
  at `:14026` and `:14170`; the fullscreen `SendMessageW`/`ShowWindow`
  transition starts only at `:14194`. Root cause: the scheduler's main-owner
  priority path only restored the parked main context while saving an active
  worker first, so an already-idle scheduler could fall through to generic
  worker round-robin. Fixed in source at
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:619` by
  restoring the main owner when no worker is active too, while not waking it
  through a plain blocking wait. The same validation then showed a host-only
  freeze where the remote API/framebuffer path kept running while the local
  Windows presenter stayed stale. Root cause was local host-presentation
  batching: during long `pendingMessageTransfers_` spans, the loop could
  continue before the normal batch-release path. Source now heartbeat-flushes
  host batches during sent-message transfers at
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1048` and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:2370`.
  A follow-up live report showed a startup safety/fullscreen popup overlap
  returned: stale fullscreen pixels could remain while the bottom strip
  repainted. The owner-stack hide policy was too broad because it discarded
  backing for all owned popups. Full-screen owned popup teardown now restores
  valid captured backing first, while smaller owner-stack popups still defer
  to owner repaint. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1572`.

## Partially Resolved: Remote Audio WebSocket And Host Audio Timing

Symptom:

- Debug remote-server runs could show guest `waveOutOpen`/`waveOutWrite` while
  `/api/v1/audio/ws` produced no useful audio when remote audio was disabled.
- Keeping a bounded startup PCM queue without a connected websocket client
  would be wrong because a late client could hear stale delayed audio.
- Short live click sounds could arrive over the audio websocket grouped in
  small bursts because the audio websocket loop slept between polls and tiny
  frames could be coalesced by TCP.
- When a websocket client was connected during a long guest `waveOutWrite`,
  the remote audio queue could hold seconds of old PCM, so later click sounds
  waited behind that backlog.
- A websocket client connecting while a long startup buffer was already
  playing could not join the current audio position because the old path only
  published PCM at `waveOutWrite` submit time.
- Host/local WinMM was still too close to the guest-visible timing model:
  callbacks/events from host playback could act like CE completion instead of
  a virtual wave stream returning queued buffers.
- The remote websocket tap could add a light buzz because it resampled every
  20 ms live slice with a fresh converter instead of preserving stream
  continuity.

Evidence:

- Run `captures/inavi_autodrive_20260531_081857/emulator.stdout.log`
  logged `remote server: 192.168.0.39:8765 ... audio=0`, then later logged a
  startup `waveOutWrite` with an 11.2s buffer.

Status:

- Partially fixed on 2026-05-31. `tools/autodrive_inavi.ps1` now enables
  remote audio by default when `-RemoteServer` is used, with `-NoRemoteAudio`
  available for explicit opt-out. Remote PCM is only queued while at least one
  audio websocket client is connected, and stale queued audio is dropped on
  first connect and last disconnect. The audio websocket wakes when new PCM is
  available and sets `TCP_NODELAY`.
- `CeAudio` now owns the virtual CE wave-output timeline. `waveOutWrite`
  queues copied guest PCM, virtual completion marks `WHDR_DONE`, clears
  `WHDR_INQUEUE`, and signals the guest completion event. The websocket now
  materializes live slices from the active timeline, so a mid-playback client
  can join near the current offset without replaying stale audio. CE
  references:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:944`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/wavemain.cpp:396`,
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.cpp:130`,
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.h:93`.
  Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_audio.h:10`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_host_audio.cpp:135`,
  and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:1022`.
- Local WinMM playback is now a backend and no longer the guest-visible
  clock; it is fed through a backend chunk queue instead of a single
  whole-buffer host submission. A follow-up report found the chunked backend
  could buzz/distort because copied local buffers were released after a
  duration estimate instead of waiting for WinMM to mark the `WAVEHDR` done.
  The backend now waits for `WHDR_DONE`/successful unprepare before freeing
  the copied chunk, and the websocket path now refuses to send raw guest PCM as
  the configured remote format if miniaudio conversion fails. A follow-up pop
  report showed the local backend should not stitch many tiny WinMM buffers
  together; it now submits one copied host buffer per guest `waveOutWrite`
  while keeping guest-visible completion on the virtual `CeAudio` timeline.
- The websocket tap now keeps a continuous miniaudio converter across adjacent
  live `CeAudio` slices and resets it only on format/cursor discontinuity or
  client reset. Websocket send pacing now uses the actual output PCM duration
  instead of the requested chunk duration. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:56`,
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:440`, and
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:1232`.
  Bounded Release smoke `captures/inavi_autodrive_20260531_112536` found no
  fatal/unsupported/PC-zero signatures, and Debug interactive run
  `captures/inavi_autodrive_20260531_112632` is live for subjective buzz
  validation.
- A later route-guide run found that cooperative paint around a blocking
  `WaitForSingleObject(..., INFINITE)` could still return `WAIT_TIMEOUT` from
  the named wait shim. That caused audio buffers to be unprepared while still
  queued and also correlated with modal/draw desync. This wait-continuation
  bug is fixed in source and Release-build verified; remaining open piece:
  validate a real interactive run with mid-startup websocket connect, route
  guide audio, and short click sounds.
- Debug run `captures/inavi_autodrive_20260531_112632` showed another
  source-shaped ordering bug in the same area: a menu click reached the guest
  and posted `msg=0x5734`, but the following short `waveOutWrite` had
  `durationMs=91` and its event wait resumed about 4.35 s later because the
  blocking-wait continuation pumped paint/timer work before refreshing virtual
  audio and re-probing the wait object. Fixed in source by letting wait
  readiness win first and using cooperative paint only while the wait remains
  blocked. Current source:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:1529`. Release build
  passed and bounded smoke `captures/inavi_autodrive_20260531_114217` still
  captured startup; live remote click validation remains open.

## Resolved: Missing Host Serial Port Added Startup Delay

Symptom:

- Map/startup loading could pause while `CreateFileW("COM7:")` mapped to an
  unavailable host COM port and the emulator retried the host `CreateFileW`
  several times before falling back to the virtual serial backend.

Evidence:

- Current run `captures/inavi_autodrive_20260531_081857/emulator.stdout.log`
  logged the serial-open wrapper at `08:19:20.933` and the unavailable-host
  fallback at `08:19:22.687`, about 1.75s later.

Status:

- Fixed on 2026-05-31. Mapped `win32_com` serial backends now probe the host
  port once in the guest `CreateFileW` path, then use the same virtual CE
  serial no-data backend if the host port is unavailable. This is generic to
  mapped serial devices and does not special-case `COM7:`.

## Virtual Serial No-Data Reads Can Still Hot-Poll

Symptom:

- A configured CE serial device with no host bytes can still complete
  `ReadFile` immediately with `TRUE` and zero bytes, which lets polling worker
  threads consume too many slices and can delay the UI message pump.

CE reference:

- Serial driver behavior should be modeled as device/read timeout state rather
  than a permanently disconnected success path:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/DRIVERS/SERDEV/serial.c`.
- IRCOMM read paths keep timeout/no-data behavior as device state:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/IR/IRCOMM/ircomm.c:792`.

Current emulator reference:

- Virtual serial state:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_device.h:25`.
- Guest serial `ReadFile` completion:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_fs.cpp:512`.
- Cooperative scheduler selection:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:410`.

Status:

- `CeDevice` now owns serial config, DCB-like mode, `COMMTIMEOUTS`, comm mask,
  queue sizes, and virtual no-data backend state. No-data virtual serial reads
  now park active guest threads and wake them on remote bytes or deadline
  expiry. GWE owner-priority scheduling now prefers the oldest pending message
  owner without replaying stale parked main-thread state at main API
  boundaries. Debug remote-server run
  `captures/inavi_autodrive_20260530_231358` reached the UI/map path and
  showed serial reads parking and waking on data; follow-up run
  `captures/inavi_autodrive_20260530_232138` removed the rapid
  `queued-message-preempt` bounce and reached serial no-data wait/wake again.
  The bug remains open until this is verified in a live UI interaction path
  through the previous sensor-polling freeze and the remaining slow paint
  spans are understood.

## Window Visibility And Modal Ownership Are Region-Incomplete

Symptom:

- Under-layer windows can still be plausible hit-test targets while popups or
  overlays are active.
- Popup/modal ordering depends on z-order and saved backing heuristics instead
  of CE-style visible-region ownership.
- Stale overlay or repaint artifacts are possible when windows show, hide,
  move, or destroy.

CE reference:

- `CWindow` state includes message queue, creator process/thread, WNDPROC,
  visible/update/client regions, destroy/create flags, owned-window grouping,
  and top-level priority:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1328`.
- Region and z-order algorithms:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`.

Current emulator reference:

- `GuestWindow` state:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:545`.
- Window creation:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:821`.
- Popup/paint queue heuristics:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:1052`.

Status:

- Known behavior gap. The fix direction is a window-region model, not
  app-specific modal or route-window handling.
- Current scaffold status: `CeGwe` now owns a window-region shadow populated
  from current window geometry/visibility and invalidate/validate calls. The
  input hit-test path now consumes GWE visible-region rectangles for point
  checks and pointer-capture validity. The bug remains open until z-order
  visibility, popup/backing behavior, and paint/DC clipping consume those
  regions as behavioral truth. A 2026-05-31 owner-stack pass added GWE-side
  owner/root/z-order/enabled shadow state, top/modal popup hit-test blocking,
  stack-wide pointer purge on hide/destroy/disable, and owner/root repaint
  after stack changes. It also stops owner-stack popup hides from restoring
  stale saved backing over newer owner/root paint. The next observed CE
  mismatch remains open: z-order alone is not enough, because CE `CalcVisRgn`
  removes lower owner/root visible regions below covering popups. A quick
  full-rectangle clipping experiment was backed out after a transition/
  not-responding regression; implement proper region subtraction before MGDI
  receives clipped system clips. Hidden or destroyed UI can still briefly
  leave black fill before the owner/root repaint lands. A 2026-05-31
  follow-up fixed two generic cache/queue issues in this area: pointer input no
  longer waits behind same-window `WM_ERASEBKGND`/`WM_PAINT`, and retiring an
  older fullscreen popup no longer restores its saved backing under a newer
  fullscreen popup. The bug remains open for real visible-region subtraction
  and any remaining coarse full-window paint latency.

## Remote Audio Click Sounds Can Still Burst

Symptom:

- Short click sounds over the websocket can still stack and arrive in a burst
  instead of being delivered immediately.

Current emulator reference:

- Remote audio queueing and websocket send path:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:759`.
- Guest wave output publish path:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_host_audio.cpp`.

Status:

- Still open as of the 2026-05-31 interactive remote run. Earlier fixes made
  remote audio opt-out instead of opt-in, dropped stale startup audio when no
  client is connected, woke the websocket sender on new PCM, disabled Nagle,
  and capped queued PCM. That was not enough for short click latency; the next
  pass should inspect PCM chunking/coalescing and websocket send backpressure
  at the publish/send boundary.

## Paint And ValidateRect Do Not Preserve CE Update Regions

Symptom:

- Painting can be too broad because `BeginPaint` reports a full-window paint
  rectangle.
- Validation does not clear a real update region.
- Erase/paint ordering may look correct in common cases but lacks CE's
  `update intersect visible/client` semantics.

CE reference:

- GWE paint API set:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:350`.
- Window visible/update/client region members:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.

Current emulator reference:

- `BeginPaint` returns a DC and writes a whole-window paint rectangle:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:40`.
- `ValidateRect` only checks window existence:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:159`.

Status:

- Known behavior gap. Paint APIs should be backed by window update/client
  regions.
- Current scaffold status: `InvalidateRect` and `ValidateRect` now update the
  new `CeGwe` window-region shadow, and `BeginPaint` now reports the
  GWE client-update rectangle when present. The bug remains open until update
  regions are intersected with real visible/client regions and DC clipping uses
  the same model. A narrower full-screen hide/return redraw bug is mitigated:
  hidden window subtrees now drop stale queued update messages and cannot
  recapture saved backing while hidden.

## GDI Clipping Is Not Yet A First-Class DC/Window Invariant

Symptom:

- Drawing correctness depends on blit-time framebuffer/backing/z-order checks.
- Saved backing layers can mask or expose problems that CE would solve through
  window system clipping and DC clipping.
- Further pixel-path optimization risks preserving the wrong model if clipping
  is not fixed first.

CE reference:

- MGDI `DC` state and actual device clip (`XCLIPOBJ`):
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`
  and
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:203`.
- `WBITMAP` as internal window representation with framebuffer bits,
  viewport, system clip, and DC list:
  `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.

Current emulator reference:

- `GuestDc` state:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:584`.
- `GuestBitmap` state:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:612`.
- Direct framebuffer blit path with backing-layer handling:
  `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1743`.

Status:

- Known behavior gap. Add a CE-shaped clipping/window-bitmap layer before
  treating current blit behavior as faithful.
- Current scaffold status: `CeMgdi` now mirrors DC lifetime, selected object
  state, text state, current drawing position, and app/system clip storage,
  with window DC system clips seeded from GWE visible rectangles. Central
  framebuffer drawing helpers now enforce that system clip for fills, lines,
  bitmap blits, `StretchDIBits`, and transparent image writes. MGDI also now
  mirrors bitmap metadata for created/deleted bitmaps and a window-bitmap
  scaffold for published guest windows, and `GetObjectW` bitmap metadata now
  reads through MGDI. `SetBitmapBits` now also uses MGDI metadata for storage
  size and guest bits lookup, `SetDIBColorTable` validates palette bounds
  through MGDI metadata, and `DeleteObject`/`SelectObject` use MGDI metadata
  for stock-bitmap protection and bitmap selection validation. DIB destination
  bitmap selection and `TransparentImage` source/destination bitmap selection
  also read from the MGDI DC shadow, along with `BitBlt` and `StretchBlt`
  source/destination bitmap selection. Rectangle/line/polygon/text,
  `GetPixel`, and palette-write paths now use a shared MGDI selected-bitmap
  accessor, and `SetDIBColorTable` now mutates the MGDI palette before
  mirroring to the runtime compatibility copy. `GetPixel` now overlays that
  MGDI palette for indexed pixel decode, and bitmap rectangle/line/polygon and
  host-text writes refresh the compatibility palette from MGDI before indexed
  writes. DIB-to-bitmap, `TransparentImage`, `BitBlt`, and `StretchBlt` paths
  now also refresh compatibility palettes from MGDI before indexed
  reads/writes. Selected brush/pen/font lookups and `SelectObject` return
  values now also read through the MGDI DC shadow. Text color, background
  color/mode, text alignment, and current drawing position reads now also go
  through the MGDI DC shadow for text drawing, setter return values,
  `MoveToEx`, and `LineTo`. Brush, pen, and font metadata now mirrors into
  MGDI, with host text font selection and brush/pen/font `DeleteObject` stock
  checks reading that MGDI metadata. Brush/pen drawing metadata reads for
  polygon, polyline, ellipse, fill, pat-blit, rectangle, and line drawing now
  also read through MGDI object state. Region bounds/ownership now mirror into
  MGDI for `CreateRectRgn`, `CombineRgn`, region deletion, and `SetWindowRgn`
  transfer. `GetClipBox` now reports an MGDI effective clip, and framebuffer
  drawing now uses the system/app clip intersection. After a route-guide
  visual artifact report, CE region/rounded drawing exports `RoundRect`,
  `FillRgn`, `SetRectRgn`, `SelectClipRgn`, `PtInRegion`, and `RectInRegion`
  were added to the coredll boundary and dispatcher, and framebuffer polygon
  fills now honor the effective MGDI clip. The bug remains open until route
  guide visual validation confirms whether the noisy rectangle and rectangular
  black backing are gone, and until more pixel storage/DC object ownership
  moves behind MGDI instead of runtime GDI-object maps and saved backing
  heuristics remaining the clipping truth.
