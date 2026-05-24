#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

void SyntheticDllRuntime::registerCoredllWindowExports(SyntheticModule& module) {
    struct CoreDllWindow {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.window",
                {
                    {0x0101, {"GetWindowTextW", Code::CoreDllGetWindowTextW, &SyntheticDllRuntime::handleGetWindowTextW}},
                    {0x0114, {"GetWindowTextLengthW", Code::CoreDllGetWindowTextLengthW, &SyntheticDllRuntime::handleGetWindowTextLengthW}},
                    {0x011F, {"EnableWindow", Code::CoreDllEnableWindow, &SyntheticDllRuntime::handleEnableWindow}},
                    {0x0120, {"IsWindowEnabled", Code::CoreDllIsWindowEnabled, &SyntheticDllRuntime::handleIsWindowEnabled}},
                    {0x02C0, {"SetFocus", Code::CoreDllSetFocus, &SyntheticDllRuntime::handleSetFocus}},
                    {0x02C1, {"GetFocus", Code::CoreDllGetFocus, &SyntheticDllRuntime::handleGetFocus}},
                    {0x02C3, {"GetCapture", Code::CoreDllGetCapture, &SyntheticDllRuntime::handleGetCapture}},
                    {0x02C4, {"SetCapture", Code::CoreDllSetCapture, &SyntheticDllRuntime::handleSetCapture}},
                    {0x02C5, {"ReleaseCapture", Code::CoreDllReleaseCapture, &SyntheticDllRuntime::handleReleaseCapture}},
                },
            };
        }
    };

    const CoreDllWindow window;
    registerHandlers(module, window.group());
}

bool SyntheticDllRuntime::handleGetWindowTextLengthW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = windows_.find(args.a0);
    ret = it == windows_.end() ? 0 : uint32_t(it->second.title.size());
    lastError_ = it == windows_.end() ? 1400 : 0;
    return true;
}

bool SyntheticDllRuntime::handleGetWindowTextW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
    } else if (!args.a1 || !args.a2) {
        lastError_ = 0;
        ret = 0;
    } else {
        ret = writeUtf16(args.a1, it->second.title, args.a2);
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleSetFocus(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto target = windows_.find(args.a0);
    if (!args.a0 || (target != windows_.end() && !target->second.destroyed)) {
        ret = focusedWindow_;
        focusedWindow_ = args.a0;
        if (args.a0) guestMessages_.push_back({args.a0, 0x0007, 0, 0, uint32_t(++tick_ * 16), 0, 0});
        lastError_ = 0;
    } else {
        lastError_ = 1400;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetFocus(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = focusedWindow_;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleSetCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto target = windows_.find(args.a0);
    if (!args.a0 || (target != windows_.end() && !target->second.destroyed)) {
        ret = capturedWindow_;
        capturedWindow_ = args.a0;
        lastError_ = 0;
    } else {
        lastError_ = 1400;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = capturedWindow_;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleReleaseCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = capturedWindow_ ? 1 : 0;
    capturedWindow_ = 0;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleEnableWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = windows_.find(args.a0);
    if (it != windows_.end()) {
        const bool wasEnabled = it->second.enabled;
        it->second.enabled = args.a1 != 0;
        lastError_ = 0;
        ret = wasEnabled ? 1 : 0;
        spdlog::info("EnableWindow guest=0x{:08x} enable={} oldEnabled={}",
                     args.a0, args.a1 != 0, wasEnabled);
    } else {
        lastError_ = 1400;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleIsWindowEnabled(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = windows_.find(args.a0);
    if (it != windows_.end()) {
        ret = it->second.enabled ? 1 : 0;
        lastError_ = 0;
    } else {
        ret = 0;
        lastError_ = 1400;
    }
    return true;
}
