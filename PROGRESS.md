# Progress

Last refreshed: 2026-05-30.

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
- Build-only `CeKernel`, `CeGwe`, and `CeMgdi` scaffolds now exist and are
  wired into `iNavi_Unicorn_Emulator.vcxproj`. No runtime behavior has moved
  into those scaffolds yet. The 2026-05-30 Release build passed with one
  pre-existing Boost Beast warning from `remote_server.cpp`.
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
