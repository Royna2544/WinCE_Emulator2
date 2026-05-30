# Known Bugs

Last refreshed: 2026-05-30.

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
  The bug remains open until the queue model, wake categories, and
  send-message edge cases are the behavioral truth.

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
  regions as behavioral truth.

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
  the same model.

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
  drawing now uses the system/app clip intersection. The bug remains open until
  more pixel storage/DC object ownership moves behind MGDI instead of runtime
  GDI-object maps and saved backing heuristics remaining the clipping truth.
