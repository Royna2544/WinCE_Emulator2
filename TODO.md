# TODO

Last refreshed: 2026-05-31.

This file was reset to track the CE coredll/GWE behavior-modeling work. Do not
add app-specific shortcuts here. Use CE source references and current emulator
line references when changing behavior.

Active refactor checklist: `PLAN.md`.

## Immediate

0. Validate and finish the CE-shaped audio timeline.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/core_common.def:944`,
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/wavemain.cpp:396`,
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.cpp:130`,
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/BLUETOOTH/AV/A2DP/strmctxt.h:93`.
   - Current source:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_audio.h:10`,
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_host_audio.cpp:135`,
     and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/remote_server.cpp:1022`.
   - Current status: `CeAudio` owns queued guest wave-output buffers,
     completion timing, header done/in-queue transitions, and live websocket
     slices. Websocket clients now join active playback near the current
     offset instead of receiving stale startup PCM or silence until the next
     `waveOutWrite`. Guest completion is virtual; host WinMM is a backend
     opened with `CALLBACK_NULL`, and local WinMM playback submits one copied
     backend buffer per guest `waveOutWrite` while waiting for `WHDR_DONE`
     before freeing it. The websocket tap now drops a chunk if conversion to
     the remote advertised format fails instead of sending mismatched raw
     guest PCM.
     Next step is Debug interactive validation after closing the old Debug
     emulator process: confirm the latest no-stitching local backend removes
     the pops, test a mid-startup websocket connect, and test short
     button-click sounds. Also verify the old route-guide signature no longer
     appears: `WaitForSingleObject(..., INFINITE)` must not resume with
     `WAIT_TIMEOUT`, and
     `waveOutUnprepareHeader` must not run while `WHDR_INQUEUE` is still set
     just because cooperative paint drained.

1. Finish virtual serial wait semantics and scheduler responsiveness.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/DRIVERS/SERDEV/serial.c`,
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COMM/IR/IRCOMM/ircomm.c:792`,
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
   - Current source to reshape:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/ce_device.h:25`,
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_fs.cpp:512`,
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:410`,
     and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:734`.
   - Goal: keep virtual serial devices open when configured, but park/yield
     no-data reads according to CE timeout semantics instead of producing an
     unlimited immediate `ReadFile TRUE + 0` polling loop. Then prefer GWE
     message owners over hot polling workers when UI/input work is pending.
   - Current status: serial configuration, DCB-like mode, `COMMTIMEOUTS`,
     comm mask, queue sizes, and virtual no-data backend state now live behind
     `CeDevice`. Timeout-aware no-data reads now park active guest threads and
     wake them on remote bytes or deadline expiry. GWE owner-priority
     scheduling now picks the oldest pending owner queue before generic worker
     slices, and the main pseudo-thread path now avoids replaying parked stale
     state when already returning from a main-thread API boundary. Next step
     is live interactive validation through the previous UI
     freeze/sensor-polling path and investigation of the remaining slow paint
     spans observed with the remote server enabled. The immediate
     `queued-message-preempt` bounce is fixed by not preempting an active
     worker that already owns the oldest pending message, and the legacy
     `pre-queued-worker` burst now stands down while GWE has a pending message
     owner.
     Startup audio/event waits no longer use the named-dispatch host sleep
     loop, and finite parked waits now resume with explicit `WAIT_TIMEOUT`
     results. Shared named mappings also no longer force-write the whole view
     on every `UnmapViewOfFile`; explicit `FlushViewOfFile` remains the
     force-sync path. Next validation should be a Debug interactive run with
     remote server and companion enabled through the button/update stall path.
     A remote-input backpressure fix now drops the matching release for a
     rejected pointer-down sequence, and queued-message preemption now requires
     a schedulable GWE owner. Next run should verify buttons no longer remain
     visually pressed when remote endpoint clicks arrive faster than the guest
     message pump drains them. Remote audio is now enabled by default in
     remote-server autodrive runs, but remote PCM is only buffered while an
     audio websocket client is connected so late clients do not receive stale
     startup audio. Audio websocket delivery now wakes on new chunks and uses
     TCP_NODELAY to avoid short-click PCM frames grouping into delayed bursts;
     the live audio queue is also capped to a small latency window so older PCM
     is dropped instead of delaying later clicks. Remote input now also wakes
     a window-owner thread parked in a non-`waitAll` multi-object wait when
     GWE has pending messages for that owner, matching CE's queue new-events
     signal more closely. Next validation should confirm remote button-up
     dispatch is no longer delayed until unrelated worker/event pulses.
     A follow-up source-aligned scheduler fix keeps active worker context
     visible during `queued-message-preempt` yields, so owner-priority
     selection can switch to the actual pending GWE message owner instead of
     restoring any parked main context first. Next validation should confirm
     the previous preempt bounce is gone in the remote button-stuck path.
     Host-backed mapped serial open now does only one immediate host COM probe before
     falling back to the virtual serial device, so missing host COM devices no
     longer add multi-retry startup delay. The 2026-05-31 route/dialog run
     exposed a remaining CE mismatch: `GetMessageW`/`PeekMessageW` was using
     fullscreen popup coverage as a blanket filter for posted messages. That
     is now narrowed to modal-covered mouse/syskey input only, while normal
     posted/broadcast/custom messages stay deliverable through the owner
     queue. Next validation should verify the old symptoms are gone:
     `ownerQueued` must drain instead of sticking at `7`, the repeated
     `queued-message-preempt` worker rotation should stop, and new remote
     pointer downs should no longer be rejected because an older touch
     sequence is trapped in the queue.

2. Introduce a CE-shaped internal `MsgQueue` model.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:23`
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:798`.
   - Current source to replace/reshape:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:662`,
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:912`,
     and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_thread_runtime.cpp:530`.
   - Goal: route posted, input, timer, and sent messages through per-thread
     queues instead of treating all guest messages as one mostly global deque.
   - Current status: `CeGwe` now has owner-thread lane mirrors for posted,
     sent, input, timer, and thread messages. `GetMessageW`/`PeekMessageW`
     now use owner-filtered selection while preserving flat ordering inside
     that owner context, and guest message-wait wakeup now checks the waiting
     thread's owner queue before making it runnable. `MsgWaitForMultipleObjectsEx`
     is registered and routed through the same owner-aware queue state for
     basic message readiness. Cross-thread `SendMessageW` now queues to any
     different receiver owner and parks the sender until the transfer result
     returns. Main-thread queued-message watchdog slices now preserve the
     current readable pump context instead of restoring stale parked state.
     The synthetic child-button bridge now also mirrors a child `WM_LBUTTONUP`
     before an already queued ancestor/root release when the child down is
     observed late from synchronous MFC message dispatch.
     `GetMessageW`/`PeekMessageW` now also follows the CE/MFC split for modal
     coverage: it discards stale modal-covered mouse/syskey input on removing
     reads, but does not block ordinary posted/broadcast/custom messages to
     covered windows.
     Next step is to continue Phase 4 window visible/update/client region
     migration.

3. Model cross-thread `SendMessageW` as a queue transaction.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:897`.
   - Current source to audit:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2199`.
   - Goal: sender blocks, owner queue runs, result returns to sender, and
     `InSendMessage`/timeout behavior remains explainable without faking null
     callbacks or app-specific success.
   - Current status: the sender-blocked queue/result path now covers any
     different window-owner queue. Remaining follow-up is finer
     `InSendMessage`/timeout accounting if a target app imports those APIs.

4. Implement real window visible/update/client regions.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
   - Current source to reshape:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:545` and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:821`.
   - Goal: stop relying on popup/backing/z-order heuristics as the primary
     visibility model.
   - Current status: `CeGwe` now owns a window-region shadow populated from
     current window state and invalidate/validate calls. `BeginPaint` now
     consumes the GWE client-update rectangle when present, and `EndPaint`
     clears the GWE update region. Next step is to broaden region-owned
     visibility and DC clipping. Input hit testing now uses GWE visible-region
     rectangles for its point checks. Full-screen hide/return redraw now also
     discards stale queued update messages for hidden subtrees and blocks
     backing recapture for hidden/destroyed windows, matching CE's visible/
     update region ownership more closely. The newest owner-stack pass adds
     inferred popup ownership, GWE hit-test blocking for active top/modal
     popups, pointer/capture purge on hide/destroy/disable, and owner/root
     repaint after stack changes. It also stops owner-stack popup hides from
     restoring stale saved backing over newer owner/root paint. CE source
     comparison confirms the remaining difference is `CalcVisRgn`-style
     visible-region subtraction below higher popups; a quick full-rect clip was
     backed out after a transition regression. Remaining follow-up: implement
     proper region subtraction before using it as MGDI/DC paint truth, and
     stop hide/destroy black flash by letting exposure repaint flow through the
     owner update region instead of direct erase-first behavior. Pointer input
     is no longer queued behind same-window `WM_ERASEBKGND`/`WM_PAINT`, and
     older fullscreen popup retirement no longer restores stale backing before
     hiding the old popup. Next validation should check whether nearby map UI
     still appears late after these CE-aligned queue/cache changes; if it does,
     the next suspect is coarse synchronous `UpdateWindow`/full-window paint
     cost rather than input delivery.

5. Make paint APIs consume update regions.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/INC/gweapiset1.hpp:350`
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
   - Current source to fix:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:40` and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_paint.cpp:159`.
   - Goal: `BeginPaint`, `EndPaint`, `InvalidateRect`, and `ValidateRect`
     should manipulate window update/client regions instead of whole-window
     paint rectangles only.
   - Current status: first migration is complete for `BeginPaint`/`EndPaint`
     PAINTSTRUCT rectangles. Remaining work is to intersect updates with real
     visible/client regions and feed MGDI/DC clipping from the same model.

6. Add a CE-shaped GDI clipping layer.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/dc.hpp:13`
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/MGDI/INC/gdiobj.h:358`.
   - Current source to reshape:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:584`,
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:612`,
     and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_bitmap.cpp:1743`.
   - Goal: put system clip/app clip/DC clip in one clear model before adding
     more drawing optimizations.
   - Current status: `CeMgdi` now has a DC-state shadow for DC lifetime,
     selected objects, text state, current drawing position, app/system clip
     storage, and window-DC system clips seeded from GWE visible rectangles.
     Central framebuffer drawing helpers now consult MGDI system clipping for
     rectangle fills, line drawing, bitmap blits, `StretchDIBits`, and
     transparent image writes. Bitmap metadata now also mirrors into
     `CeMgdi::BitmapState`, and published windows now mirror into a
     `CeMgdi::WindowBitmapState` scaffold with viewport, system clip, and live
     DC count. `GetObjectW` bitmap metadata, `SetBitmapBits` storage lookup,
     `SetDIBColorTable` palette bounds, `DeleteObject` stock-bitmap protection,
     `SelectObject` bitmap validation, and DIB destination-bitmap selection now
     read through MGDI metadata/state. `TransparentImage` also reads selected
     source/destination bitmap handles through MGDI DC state, as do `BitBlt`
     and `StretchBlt`. Rectangle/line/polygon/text, `GetPixel`, and
     palette-write paths now use a shared MGDI selected-bitmap accessor, and
     `SetDIBColorTable` now mutates the MGDI bitmap palette before mirroring to
     the runtime compatibility copy. `GetPixel` now overlays that MGDI palette
     before decoding indexed bitmap pixels, and bitmap rectangle/line/polygon
     and host-text writes now refresh the compatibility palette from MGDI
     before indexed writes. Route-guide source comparison added the next
     missing CE region/drawing
     surface: `RoundRect`, `FillRgn`, `SetRectRgn`, `SelectClipRgn`,
     `PtInRegion`, and `RectInRegion` are exported/handled, and framebuffer
     polygon fills now honor the effective MGDI clip. Next validation should
     check whether the route-guide rounded overlay no longer paints a
     rectangular black backing and whether the noisy route-guide area still
     leaks stale pixels. DIB-to-bitmap, `TransparentImage`, `BitBlt`, and
     `StretchBlt` paths now do the same before indexed reads/writes. Selected
     brush/pen/font lookups and `SelectObject` return values now also read
     through the MGDI DC shadow. Text color, background color/mode, text
     alignment, and current drawing position reads now also go through the
     MGDI DC shadow for text drawing, setter return values, `MoveToEx`, and
     `LineTo`. Brush, pen, and font metadata now mirrors into MGDI, with host
     text font selection and brush/pen/font `DeleteObject` stock checks reading
     that MGDI metadata. Brush/pen drawing metadata reads for polygon, polyline,
     ellipse, fill, pat-blit, rectangle, and line drawing now also read through
     MGDI object state. Region bounds/ownership now mirror into MGDI for
     `CreateRectRgn`, `CombineRgn`, region deletion, and `SetWindowRgn`
     transfer. `GetClipBox` now reports an MGDI effective clip, and framebuffer
     drawing now uses the system/app clip intersection. Next step is to move
     more pixel operations/DC object ownership behind MGDI instead of keeping
     `GuestBitmap`, runtime GDI-object maps, and saved backing layers as
     runtime-owned truth.

## Next

7. Preserve the coredll/GWE API-set separation internally.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/coredll.cpp:41`.
   - Current source to audit:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll_modules.cpp:35`.
   - Goal: keep synthetic exports as the ABI boundary while moving behavior
     into clearer internal subsystems: scheduler, message queue, window
     manager, paint/update regions, and MGDI/DC.

8. Re-test modal/overlay/input behavior after the queue and region work.
   - Do not add route-screen, file-name, window-title, or coordinate-specific
     emulator behavior.
   - Use bounded runs and keep logs targeted.

## Later

9. Re-profile only after correctness changes settle.
   - The current GDI hot paths may change substantially once clipping and
     update regions are first-class.
   - Do not optimize around today's backing/z-order heuristics if those
     heuristics are being replaced.
