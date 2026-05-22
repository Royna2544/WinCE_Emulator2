#include "synthetic_dll.h"

#include <array>

void SyntheticDllRuntime::registerCoredllPaintExports(SyntheticModule& module) {
    struct CoreDllPaint {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.paint",
                {
                    {0x0104, {"BeginPaint", Code::CoreDllBeginPaint, &SyntheticDllRuntime::handleBeginPaint}},
                    {0x0105, {"EndPaint", Code::CoreDllEndPaint, &SyntheticDllRuntime::handleEndPaint}},
                    {0x0106, {"GetDC", Code::CoreDllGetDC, &SyntheticDllRuntime::handleGetDC}},
                    {0x0107, {"ReleaseDC", Code::CoreDllReleaseDC, &SyntheticDllRuntime::handleReleaseDC}},
                    {0x0116, {"ValidateRect", Code::CoreDllValidateRect, &SyntheticDllRuntime::handleValidateRect}},
                    {0x04A1, {"GetDCEx", Code::CoreDllGetDCEx, &SyntheticDllRuntime::handleGetDCEx}},
                },
            };
        }
    };

    const CoreDllPaint paint;
    registerHandlers(module, paint.group());
}

bool SyntheticDllRuntime::handleBeginPaint(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = windows_.find(args.a0);
    if (it == windows_.end()) {
        lastError_ = 1400;
        ret = 0;
    } else {
        ret = makeGuestDc(args.a0);
        if (args.a1) {
            std::array<uint8_t, 64> paint{};
            uc_mem_write(uc_, args.a1, paint.data(), paint.size());
            writeU32(args.a1, ret);
            writeU32(args.a1 + 4, 1);
            writeGuestRect(args.a1 + 8, 0, 0, it->second.width, it->second.height);
        }
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleEndPaint(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t hdc = 0;
    if (args.a1) uc_mem_read(uc_, args.a1, &hdc, sizeof(hdc));
    if (hdc) {
        dcs_.erase(hdc);
        guestHandles_.erase(hdc);
    }
    ret = windows_.count(args.a0) ? 1 : 0;
    lastError_ = ret ? 0 : 1400;
    return true;
}

bool SyntheticDllRuntime::handleGetDC(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (args.a0 && !windows_.count(args.a0)) {
        lastError_ = 1400;
        ret = 0;
    } else {
        ret = makeGuestDc(args.a0);
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetDCEx(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleGetDC(code, args, ret);
}

bool SyntheticDllRuntime::handleReleaseDC(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto handle = guestHandles_.find(args.a1);
    if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestDc) {
        lastError_ = 6;
        ret = 0;
    } else {
        dcs_.erase(args.a1);
        guestHandles_.erase(handle);
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleValidateRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = windows_.count(args.a0) ? 1 : 0;
    lastError_ = ret ? 0 : 1400;
    return true;
}
