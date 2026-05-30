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
  remains future work if the app reaches those APIs. The bug remains open
  until the queue model, wake categories, and send-message edge cases are the
  behavioral truth.

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
  size and guest bits lookup, while `SetDIBColorTable` validates palette
  bounds through MGDI metadata. The bug remains open until more pixel
  storage/DC object ownership moves behind MGDI instead of saved backing
  heuristics remaining the clipping truth.
