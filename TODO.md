# TODO

Last refreshed: 2026-05-30.

This file was reset to track the CE coredll/GWE behavior-modeling work. Do not
add app-specific shortcuts here. Use CE source references and current emulator
line references when changing behavior.

Active refactor checklist: `PLAN.md`.

## Immediate

1. Introduce a CE-shaped internal `MsgQueue` model.
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
     sent, input, timer, and thread messages, but `GetMessageW`/`PeekMessageW`
     still consume the flat queue as dispatch truth. Next step is to make
     selection owner-thread-aware without changing CE-visible order.

2. Model cross-thread `SendMessageW` as a queue transaction.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/cmsgque.h:897`.
   - Current source to audit:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.cpp:2199`.
   - Goal: sender blocks, owner queue runs, result returns to sender, and
     `InSendMessage`/timeout behavior remains explainable without faking null
     callbacks or app-specific success.

3. Implement real window visible/update/client regions.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1038`
     and
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/GWE/INC/window.hpp:1351`.
   - Current source to reshape:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll.h:545` and
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/coredll_window_runtime.cpp:821`.
   - Goal: stop relying on popup/backing/z-order heuristics as the primary
     visibility model.

4. Make paint APIs consume update regions.
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

5. Add a CE-shaped GDI clipping layer.
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

## Next

6. Preserve the coredll/GWE API-set separation internally.
   - CE reference:
     `/home/royna/WinCE-src_20201004/PRIVATE/WINCEOS/COREOS/CORE/DLL/coredll.cpp:41`.
   - Current source to audit:
     `/mnt/d/GitHub/WinCE_Emulator_v2/src/synthetic_dll_modules.cpp:35`.
   - Goal: keep synthetic exports as the ABI boundary while moving behavior
     into clearer internal subsystems: scheduler, message queue, window
     manager, paint/update regions, and MGDI/DC.

7. Re-test modal/overlay/input behavior after the queue and region work.
   - Do not add route-screen, file-name, window-title, or coordinate-specific
     emulator behavior.
   - Use bounded runs and keep logs targeted.

## Later

8. Re-profile only after correctness changes settle.
   - The current GDI hot paths may change substantially once clipping and
     update regions are first-class.
   - Do not optimize around today's backing/z-order heuristics if those
     heuristics are being replaced.
