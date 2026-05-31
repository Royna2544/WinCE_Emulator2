#include "synthetic_dll.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

void SyntheticDllRuntime::registerCoredllGuiExports(SyntheticModule& module) {
    struct CoreDllGui {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.gui",
                {
                    {0x02AA, {"SetCursor", Code::CoreDllSetCursor, &SyntheticDllRuntime::handleSetCursor}},
                    {0x02B4, {"GetDlgItem", Code::CoreDllGetDlgItem, &SyntheticDllRuntime::handleGetDlgItem}},
                    {0x02B5, {"GetDlgCtrlID", Code::CoreDllGetDlgCtrlID, &SyntheticDllRuntime::handleGetDlgCtrlID}},
                    {0x02BD, {"GetForegroundWindow", Code::CoreDllGetForegroundWindow, &SyntheticDllRuntime::handleGetForegroundWindow}},
                    {0x02BE, {"SetForegroundWindow", Code::CoreDllSetForegroundWindow, &SyntheticDllRuntime::handleSetForegroundWindow}},
                    {0x02BF, {"SetActiveWindow", Code::CoreDllSetActiveWindow, &SyntheticDllRuntime::handleSetActiveWindow}},
                    {0x02C2, {"GetActiveWindow", Code::CoreDllGetActiveWindow, &SyntheticDllRuntime::handleGetActiveWindow}},
                    {0x02DE, {"GetCursorPos", Code::CoreDllGetCursorPos, &SyntheticDllRuntime::handleGetCursorPos}},
                    {0x0346, {"TranslateAcceleratorW", Code::CoreDllTranslateAcceleratorW, &SyntheticDllRuntime::handleTranslateAcceleratorW}},
                    {0x035A, {"MessageBoxW", Code::CoreDllMessageBoxW, &SyntheticDllRuntime::handleMessageBoxW}},
                    {0x0375, {"GetSystemMetrics", Code::CoreDllGetSystemMetrics, &SyntheticDllRuntime::handleGetSystemMetrics}},
                    {0x0376, {"IsWindowVisible", Code::CoreDllIsWindowVisible, &SyntheticDllRuntime::handleIsWindowVisible}},
                    {0x0377, {"AdjustWindowRectEx", Code::CoreDllAdjustWindowRectEx, &SyntheticDllRuntime::handleAdjustWindowRectEx}},
                    {0x0379, {"GetSysColor", Code::CoreDllGetSysColor, &SyntheticDllRuntime::handleGetSysColor}},
                    {0x037B, {"RegisterWindowMessageW", Code::CoreDllRegisterWindowMessageW, &SyntheticDllRuntime::handleRegisterWindowMessageW}},
                    {0x03A9, {"GetSysColorBrush", Code::CoreDllGetSysColorBrush, &SyntheticDllRuntime::handleGetSysColorBrushOrdinal}},
                },
            };
        }
    };

    const CoreDllGui gui;
    registerHandlers(module, gui.group());
}

bool SyntheticDllRuntime::handleSetCursor(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = currentCursor_;
    currentCursor_ = args.a0;
    auto handle = ceKernel_.handles().find(args.a0);
    HCURSOR hostCursor = nullptr;
    if (handle != ceKernel_.handles().end() && handle->second.hostValue) {
        hostCursor = reinterpret_cast<HCURSOR>(handle->second.hostValue);
    }
    ::SetCursor(hostCursor);
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetCursorPos(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    if (!args.a0) {
        lastError_ = 87;
        ret = 0;
        return true;
    }

    POINT point{};
    ret = GetCursorPos(&point) ? 1 : 0;
    writeU32(args.a0, uint32_t(point.x));
    writeU32(args.a0 + 4, uint32_t(point.y));
    if (!ret) lastError_ = GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleGetSystemMetrics(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    if (args.a0 == 0 && framebufferWidth_ > 0) ret = uint32_t(framebufferWidth_);
    else if (args.a0 == 1 && framebufferHeight_ > 0) ret = uint32_t(framebufferHeight_);
    else ret = uint32_t(GetSystemMetrics(int(args.a0)));
    return true;
}

bool SyntheticDllRuntime::handleGetSysColor(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = uint32_t(GetSysColor(int(args.a0 & 0xffu)));
    return true;
}

bool SyntheticDllRuntime::handleGetSysColorBrushOrdinal(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = handleGetSysColorBrush(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleRegisterWindowMessageW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string value = readUtf16(args.a0);
    const std::wstring wide(value.begin(), value.end());
    ret = RegisterWindowMessageW(wide.c_str());
    if (!ret) lastError_ = GetLastError();
    spdlog::info("RegisterWindowMessageW name=\"{}\" -> 0x{:08x} lastError={}", value, ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleAdjustWindowRectEx(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    int32_t rect[4]{};
    if (!args.a0 || uc_mem_read(uc_, args.a0, rect, sizeof(rect)) != UC_ERR_OK) {
        lastError_ = 87;
        ret = 0;
        return true;
    }

    RECT hostRect{rect[0], rect[1], rect[2], rect[3]};
    ret = ::AdjustWindowRectEx(&hostRect, args.a1, args.a2 != 0, args.a3) ? 1 : 0;
    if (ret) {
        writeGuestRect(args.a0, hostRect.left, hostRect.top, hostRect.right, hostRect.bottom);
        lastError_ = 0;
    } else {
        lastError_ = ::GetLastError();
    }
    return true;
}

bool SyntheticDllRuntime::handleGetDlgItem(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto parent = ceGwe_.windows().find(args.a0);
    if (parent == ceGwe_.windows().end()) {
        lastError_ = 1400;
        ret = 0;
        return true;
    }

    ret = 0;
    for (const auto& [hwnd, window] : ceGwe_.windows()) {
        if (window.parent == args.a0 && window.menu == args.a1) {
            ret = hwnd;
            break;
        }
    }
    lastError_ = ret ? 0 : 1421;
    return true;
}

bool SyntheticDllRuntime::handleGetDlgCtrlID(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = ceGwe_.windows().find(args.a0);
    if (it == ceGwe_.windows().end()) {
        lastError_ = 1400;
        ret = 0xffffffffu;
    } else {
        lastError_ = 0;
        ret = it->second.menu;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetForegroundWindow(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    auto firstWindow = [&]() -> uint32_t {
        for (const auto& [hwnd, window] : ceGwe_.windows()) {
            if (!window.destroyed && !window.parent) return hwnd;
        }
        for (const auto& [hwnd, window] : ceGwe_.windows()) {
            if (!window.destroyed) return hwnd;
        }
        return 0;
    };
    ret = focusedWindow_ ? focusedWindow_ : firstWindow();
    lastError_ = ret ? 0 : 1400;
    return true;
}

bool SyntheticDllRuntime::handleSetForegroundWindow(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    if (ceGwe_.windows().count(args.a0)) {
        ceGwe_.postPostedMessage({args.a0, 0x0006, 1, 0, uint32_t(++tick_ * 16), 0, 0});
        ceGwe_.postPostedMessage({args.a0, 0x0007, 0, 0, uint32_t(++tick_ * 16), 0, 0});
        focusedWindow_ = args.a0;
        lastError_ = 0;
        ret = 1;
    } else {
        lastError_ = 1400;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleSetActiveWindow(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const uint32_t previous = focusedWindow_;
    auto target = ceGwe_.windows().find(args.a0);
    if (!args.a0 || (target != ceGwe_.windows().end() && !target->second.destroyed)) {
        focusedWindow_ = args.a0;
        ret = previous;
        lastError_ = 0;
    } else {
        ret = 0;
        lastError_ = 1400;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetActiveWindow(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 0;
    for (const auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.destroyed && !window.parent) {
            ret = hwnd;
            break;
        }
    }
    if (!ret) {
        for (const auto& [hwnd, window] : ceGwe_.windows()) {
            if (!window.destroyed) {
                ret = hwnd;
                break;
            }
        }
    }
    lastError_ = ret ? 0 : 1400;
    return true;
}

bool SyntheticDllRuntime::handleMessageBoxW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    spdlog::info("MessageBoxW caption=\"{}\" text=\"{}\" flags=0x{:08x}",
                 readUtf16(args.a2), readUtf16(args.a1), args.a3);
    ret = 1;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleTranslateAcceleratorW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    (void)args;
    ret = 0;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleIsWindowVisible(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto it = ceGwe_.windows().find(args.a0);
    if (it == ceGwe_.windows().end()) {
        lastError_ = 1400;
        ret = 0;
        spdlog::debug("IsWindowVisible hwnd=0x{:08x} missing -> 0", args.a0);
    } else {
        lastError_ = 0;
        ret = it->second.visible ? 1 : 0;
        spdlog::debug("IsWindowVisible hwnd=0x{:08x} visible={} destroyed={} parent=0x{:08x} style=0x{:08x} rect={},{} {}x{} host=0x{:x} -> {}",
                      args.a0, it->second.visible, it->second.destroyed, it->second.parent,
                      it->second.style, it->second.x, it->second.y, it->second.width,
                      it->second.height, it->second.hostHwnd, ret);
    }
    return true;
}

uint32_t SyntheticDllRuntime::handleGetSysColorBrush(uint32_t colorIndex) {
    return makeGuestBrush(uint32_t(GetSysColor(int(colorIndex & 0xffu))), true);
}
