#include "synthetic_dll.h"

#include <array>
#include <cstdlib>
#include <vector>

namespace {

uint32_t pixelToColorRef(uint32_t pixel) {
    return ((pixel >> 16) & 0x000000ffu) |
           (pixel & 0x0000ff00u) |
           ((pixel & 0x000000ffu) << 16);
}

}

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
                    {0x03A8, {"GetPixel", Code::CoreDllGetPixel, &SyntheticDllRuntime::handleGetPixel}},
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
            const CeGwe::WindowRegionState* region = ceGwe_.windowRegionState(args.a0);
            if (region && region->hasClientUpdateRegion) {
                writeGuestRect(args.a1 + 8,
                               region->clientUpdateRect.left,
                               region->clientUpdateRect.top,
                               region->clientUpdateRect.right,
                               region->clientUpdateRect.bottom);
            } else {
                writeGuestRect(args.a1 + 8, 0, 0, it->second.width, it->second.height);
            }
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
        ceMgdi_.destroyDc(hdc);
        dcs_.erase(hdc);
        ceKernel_.handles().erase(hdc);
    }
    ret = windows_.count(args.a0) ? 1 : 0;
    lastError_ = ret ? 0 : 1400;
    if (ret) {
        ceGwe_.validateWindow(args.a0);
        presentHostWindows(true);
    }
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

bool SyntheticDllRuntime::handleGetPixel(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto dcIt = dcs_.find(args.a0);
    if (dcIt == dcs_.end()) {
        lastError_ = 6;
        ret = 0xffffffffu;
        return true;
    }

    const GuestDc& dc = dcIt->second;
    uint32_t pixel = 0xff000000u;
    const uint32_t selectedBitmap = ceMgdi_.selectedBitmapForDc(dc.hdc, dc.selectedBitmap);
    if (selectedBitmap) {
        const auto bitmapIt = bitmaps_.find(selectedBitmap);
        if (bitmapIt == bitmaps_.end() || !bitmapIt->second.bits ||
            bitmapIt->second.width <= 0 || bitmapIt->second.heightRaw == 0 ||
            bitmapIt->second.stride == 0) {
            lastError_ = 6;
            ret = 0xffffffffu;
            return true;
        }

        const GuestBitmap& bitmap = bitmapIt->second;
        const int32_t height = std::abs(bitmap.heightRaw);
        const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
        if (!byteCount || byteCount > 0x2000000ull) {
            lastError_ = 87;
            ret = 0xffffffffu;
            return true;
        }

        std::vector<uint8_t> raw(static_cast<size_t>(byteCount));
        if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK ||
            !readBitmapPixel(bitmap, raw, height, int32_t(args.a1), int32_t(args.a2), pixel)) {
            lastError_ = 87;
            ret = 0xffffffffu;
            return true;
        }
    } else {
        int32_t x = int32_t(args.a1);
        int32_t y = int32_t(args.a2);
        if (dc.hwnd) {
            const auto [originX, originY] = guestWindowOrigin(dc.hwnd);
            x += originX;
            y += originY;
        }
        pixel = readFramebufferTargetPixel(dc.hwnd, x, y);
    }

    ret = pixelToColorRef(pixel);
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleReleaseDC(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto handle = ceKernel_.handles().find(args.a1);
    if (handle == ceKernel_.handles().end() || handle->second.kind != GuestHandle::Kind::GuestDc) {
        lastError_ = 6;
        ret = 0;
    } else {
        ceMgdi_.destroyDc(args.a1);
        dcs_.erase(args.a1);
        ceKernel_.handles().erase(handle);
        lastError_ = 0;
        ret = 1;
        presentHostWindows(false);
    }
    return true;
}

bool SyntheticDllRuntime::handleValidateRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = windows_.count(args.a0) ? 1 : 0;
    if (ret) ceGwe_.validateWindow(args.a0);
    lastError_ = ret ? 0 : 1400;
    return true;
}
