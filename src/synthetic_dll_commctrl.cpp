#include "synthetic_dll.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace {

int32_t commandBarHeight() {
    return 26;
}

}

void SyntheticDllRuntime::registerCommctrlExports(SyntheticModule& module) {
    struct CommctrlDll {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "commctrl",
                {
                    {0x0001, {"InitCommonControls", Code::CommctrlInitCommonControls, &SyntheticDllRuntime::handleCommctrlInitCommonControls}},
                    {0x0002, {"InitCommonControlsEx", Code::CommctrlInitCommonControlsEx, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0003, {"CommandBar_Create", Code::CommctrlCommandBarCreate, &SyntheticDllRuntime::handleCommctrlCommandBarCreate}},
                    {0x0004, {"CommandBar_Show", Code::CommctrlCommandBarShow, &SyntheticDllRuntime::handleCommctrlCommandBarShow}},
                    {0x0005, {"CommandBar_AddBitmap", Code::CommctrlCommandBarAddBitmap, &SyntheticDllRuntime::handleCommctrlCommandBarAddBitmap}},
                    {0x0006, {"CommandBar_InsertComboBox", Code::CommctrlCommandBarInsertComboBox, &SyntheticDllRuntime::handleCommctrlCommandBarInsertComboBox}},
                    {0x0007, {"CommandBar_InsertControl", Code::CommctrlCommandBarInsertControl, &SyntheticDllRuntime::handleCommctrlCommandBarInsertControl}},
                    {0x0008, {"CommandBar_InsertMenubar", Code::CommctrlCommandBarInsertMenubar, &SyntheticDllRuntime::handleCommctrlCommandBarInsertMenubar}},
                    {0x0009, {"CommandBar_GetMenu", Code::CommctrlCommandBarGetMenu, &SyntheticDllRuntime::handleCommctrlCommandBarGetMenu}},
                    {0x000A, {"CommandBar_AddAdornments", Code::CommctrlCommandBarAddAdornments, &SyntheticDllRuntime::handleCommctrlCommandBarAdornments}},
                    {0x000B, {"CommandBar_GetItemWindow", Code::CommctrlCommandBarGetItemWindow, &SyntheticDllRuntime::handleCommctrlCommandBarAdornments}},
                    {0x000C, {"CommandBar_Height", Code::CommctrlCommandBarHeight, &SyntheticDllRuntime::handleCommctrlCommandBarHeight}},
                    {0x000D, {"IsCommandBarMessage", Code::CommctrlIsCommandBarMessage, &SyntheticDllRuntime::handleCommctrlIsCommandBarMessage}},
                    {0x000E, {"CreateUpDownControl", Code::CommctrlCreateUpDownControl, &SyntheticDllRuntime::handleCommctrlCreateToolbar}},
                    {0x000F, {"?CreateToolbar@@YAPAUHWND__@@PAU1@KIHPAUHINSTANCE__@@IPBU_TBBUTTON@@H@Z", Code::CommctrlCreateToolbar, &SyntheticDllRuntime::handleCommctrlCreateToolbar}},
                    {0x0010, {"CreateToolbarEx", Code::CommctrlCreateToolbarEx, &SyntheticDllRuntime::handleCommctrlCreateToolbar}},
                    {0x0011, {"CreateStatusWindowW", Code::CommctrlCreateStatusWindowW, &SyntheticDllRuntime::handleCommctrlCreateStatusWindowW}},
                    {0x0012, {"PropertySheetW", Code::CommctrlPropertySheetW, &SyntheticDllRuntime::handleCommctrlPropertySheetW}},
                    {0x0013, {"CreatePropertySheetPageW", Code::CommctrlCreatePropertySheetPageW, &SyntheticDllRuntime::handleCommctrlCreatePropertySheetPageW}},
                    {0x0014, {"DestroyPropertySheetPage", Code::CommctrlDestroyPropertySheetPage, &SyntheticDllRuntime::handleCommctrlDestroyPropertySheetPage}},
                    {0x0015, {"DrawStatusTextW", Code::CommctrlDrawStatusTextW, &SyntheticDllRuntime::handleCommctrlDrawStatusTextW}},
                    {0x0016, {"InvertRect", Code::CommctrlInvertRect, &SyntheticDllRuntime::handleCommctrlInvertRect}},
                    {0x002A, {"CommandBar_InsertMenubarEx", Code::CommctrlCommandBarInsertMenubarEx, &SyntheticDllRuntime::handleCommctrlCommandBarInsertMenubar}},
                    {0x002B, {"CommandBar_DrawMenuBar", Code::CommctrlCommandBarDrawMenuBar, &SyntheticDllRuntime::handleCommctrlCommandBarAdornments}},
                    {0x002C, {"CommandBar_AlignAdornments", Code::CommctrlCommandBarAlignAdornments, &SyntheticDllRuntime::handleCommctrlCommandBarAdornments}},
                    {0x0030, {"InitCapEdit", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0033, {"InitDateClasses", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0034, {"InitProgressClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0035, {"InitReBarClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0036, {"InitSBEdit", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0037, {"InitStatusClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0038, {"InitTTButton", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0039, {"InitTTStatic", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x003A, {"InitToolTipsClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x003B, {"InitToolbarClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x003C, {"InitTrackBar", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x003D, {"InitUpDownClass", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                    {0x0041, {"ListView_SetItemSpacing", Code::CommctrlListViewSetItemSpacing, &SyntheticDllRuntime::handleCommctrlListViewSetItemSpacing}},
                    {0x0045, {"Tab_Init", Code::CommctrlInitClass, &SyntheticDllRuntime::handleCommctrlInitClass}},
                },
            };
        }
    };

    const CommctrlDll commctrl;
    registerHandlers(module, commctrl.group());
}

bool SyntheticDllRuntime::handleCommctrlInitCommonControls(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlInitClass(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarCreate(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto parent = windows_.find(args.a1);
    const int32_t width = parent == windows_.end()
        ? (framebufferWidth_ > 0 ? framebufferWidth_ : 800)
        : std::max<int32_t>(1, parent->second.width);
    ret = makeGuestWindow("CommandBar", {}, 0x50000000u, 0, args.a1, uint32_t(args.a2), args.a0, 0,
                          0, 0, width, commandBarHeight(), true);
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarShow(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
    } else {
        it->second.visible = args.a1 != 0;
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarHeight(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = windows_.count(args.a0) ? uint32_t(commandBarHeight()) : 0;
    lastError_ = ret ? 0 : 1400;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarInsertComboBox(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
        return true;
    }
    ret = makeGuestWindow("ComboBox", {}, args.a3 | 0x50000000u, 0, args.a0, stackArg(4), args.a1, 0,
                          0, 0, std::max<int32_t>(1, int32_t(args.a2)), 22, it->second.visible);
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarInsertControl(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
        return true;
    }
    ret = makeGuestWindow("CommandBarControl", {}, 0x50000000u, 0, args.a0, args.a2, args.a1, 0,
                          0, 0, 80, 22, it->second.visible);
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarAddBitmap(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = findResource(2, args.a2) ? 0 : 0xffffffffu;
    lastError_ = ret == 0xffffffffu ? 1814 : 0;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarInsertMenubar(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
        return true;
    }
    const uint32_t menu = loadMenuResourceHandle(args.a2);
    if (!menu) {
        ret = 0;
        return true;
    }
    it->second.menu = menu;
    handleCommctrlCommandBarAdornments(SyntheticExportCode::CommctrlCommandBarDrawMenuBar, args, ret);
    ret = 1;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarGetMenu(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
    } else {
        lastError_ = it->second.menu ? 0 : 1401;
        ret = it->second.menu;
    }
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCommandBarAdornments(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
        return true;
    }
    if (code == SyntheticExportCode::CommctrlCommandBarDrawMenuBar) {
        uint32_t current = it->second.hwnd;
        GuestWindow* top = nullptr;
        for (;;) {
            auto window = windows_.find(current);
            if (window == windows_.end()) break;
            if (!window->second.parent) {
                top = &window->second;
                break;
            }
            current = window->second.parent;
        }
        auto* menuHandle = lookupGuestHandle(it->second.menu);
        if (top && top->hostHwnd && menuHandle && menuHandle->kind == GuestHandle::Kind::HostMenu && menuHandle->hostValue) {
            HWND hostHwnd = reinterpret_cast<HWND>(top->hostHwnd);
            if (IsWindow(hostHwnd)) {
                SetMenu(hostHwnd, reinterpret_cast<HMENU>(menuHandle->hostValue));
                DrawMenuBar(hostHwnd);
            }
        }
    }
    lastError_ = 0;
    ret = code == SyntheticExportCode::CommctrlCommandBarAlignAdornments ? 0 : 1;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlIsCommandBarMessage(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 0;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCreateStatusWindowW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto parent = windows_.find(args.a2);
    const int32_t parentWidth = parent == windows_.end()
        ? (framebufferWidth_ > 0 ? framebufferWidth_ : 800)
        : std::max<int32_t>(1, parent->second.width);
    const int32_t parentHeight = parent == windows_.end()
        ? (framebufferHeight_ > 0 ? framebufferHeight_ : 480)
        : std::max<int32_t>(1, parent->second.height);
    constexpr int32_t height = 22;
    ret = makeGuestWindow("msctls_statusbar32", readUtf16(args.a1), args.a0 | 0x50000000u, 0, args.a2, args.a3, 0, 0,
                          0, std::max<int32_t>(0, parentHeight - height), parentWidth, height, true);
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCreateToolbar(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    const bool upDown = code == SyntheticExportCode::CommctrlCreateUpDownControl;
    const uint32_t parentHwnd = upDown ? stackArg(9) : args.a0;
    auto parent = windows_.find(parentHwnd);
    const int32_t parentWidth = parent == windows_.end()
        ? (framebufferWidth_ > 0 ? framebufferWidth_ : 800)
        : std::max<int32_t>(1, parent->second.width);
    ret = makeGuestWindow(upDown ? "msctls_updown32" : "ToolbarWindow32",
                          {}, 0x50000000u, 0, parentHwnd, args.a2, args.a3, 0,
                          0, 0, parentWidth, commandBarHeight(), true);
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlDrawStatusTextW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    GuestDc* dc = lookupGuestDc(args.a0);
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    if (!dc || !readGuestRect(args.a1, left, top, right, bottom)) {
        lastError_ = 87;
        ret = 0;
    } else {
        fillFramebufferRect(*dc, left, top, right, bottom, colorRefToPixel(0x00c0c0c0));
        lastError_ = 0;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleCommctrlInvertRect(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    GuestDc* dc = lookupGuestDc(args.a0);
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    if (!dc || !readGuestRect(args.a1, left, top, right, bottom)) {
        lastError_ = 87;
        ret = 0;
    } else {
        fillFramebufferRect(*dc, left, top, right, bottom, colorRefToPixel(0x00000000));
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleCommctrlCreatePropertySheetPageW(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = makeGuestHandle({GuestHandle::Kind::GuestPropertySheetPage, 0, 0});
    lastError_ = ret ? 0 : 8;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlDestroyPropertySheetPage(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = guestHandles_.find(args.a0);
    if (it == guestHandles_.end() || it->second.kind != GuestHandle::Kind::GuestPropertySheetPage) {
        lastError_ = 6;
        ret = 0;
    } else {
        guestHandles_.erase(it);
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleCommctrlPropertySheetW(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    lastError_ = 120;
    ret = 0xffffffffu;
    return true;
}

bool SyntheticDllRuntime::handleCommctrlListViewSetItemSpacing(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    lastError_ = windows_.count(args.a0) ? 0 : 1400;
    ret = 0;
    return true;
}
