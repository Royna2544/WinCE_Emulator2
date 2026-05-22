#include "synthetic_dll.h"

#include <cstring>

void SyntheticDllRuntime::registerCoredllRectExports(SyntheticModule& module) {
    struct CoreDllRect {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.rect",
                {
                    {0x0060, {"CopyRect", Code::CoreDllCopyRect, &SyntheticDllRuntime::handleCopyRect}},
                    {0x0061, {"EqualRect", Code::CoreDllEqualRect, &SyntheticDllRuntime::handleEqualRect}},
                    {0x0062, {"InflateRect", Code::CoreDllInflateRect, &SyntheticDllRuntime::handleInflateRect}},
                    {0x0064, {"IsRectEmpty", Code::CoreDllIsRectEmpty, &SyntheticDllRuntime::handleIsRectEmpty}},
                    {0x0066, {"PtInRect", Code::CoreDllPtInRect, &SyntheticDllRuntime::handlePtInRect}},
                    {0x0067, {"SetRect", Code::CoreDllSetRect, &SyntheticDllRuntime::handleSetRect}},
                    {0x0068, {"SetRectEmpty", Code::CoreDllSetRectEmpty, &SyntheticDllRuntime::handleSetRectEmpty}},
                },
            };
        }
    };

    const CoreDllRect rect;
    registerHandlers(module, rect.group());
}

bool SyntheticDllRuntime::handleCopyRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = copyGuest(args.a0, args.a1, 16) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleEqualRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    int32_t left[4]{};
    int32_t right[4]{};
    ret = args.a0 && args.a1 &&
          uc_mem_read(uc_, args.a0, left, sizeof(left)) == UC_ERR_OK &&
          uc_mem_read(uc_, args.a1, right, sizeof(right)) == UC_ERR_OK &&
          std::memcmp(left, right, sizeof(left)) == 0 ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleInflateRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    int32_t rect[4]{};
    if (args.a0 && uc_mem_read(uc_, args.a0, rect, sizeof(rect)) == UC_ERR_OK) {
        rect[0] -= int32_t(args.a1);
        rect[1] -= int32_t(args.a2);
        rect[2] += int32_t(args.a1);
        rect[3] += int32_t(args.a2);
        uc_mem_write(uc_, args.a0, rect, sizeof(rect));
        ret = 1;
    } else {
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleIsRectEmpty(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    int32_t rect[4]{};
    ret = args.a0 && uc_mem_read(uc_, args.a0, rect, sizeof(rect)) == UC_ERR_OK &&
          (rect[2] <= rect[0] || rect[3] <= rect[1]) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handlePtInRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    int32_t rect[4]{};
    ret = args.a0 && uc_mem_read(uc_, args.a0, rect, sizeof(rect)) == UC_ERR_OK &&
          int32_t(args.a1) >= rect[0] && int32_t(args.a1) < rect[2] &&
          int32_t(args.a2) >= rect[1] && int32_t(args.a2) < rect[3] ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleSetRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (args.a0) {
        writeGuestRect(args.a0, int32_t(args.a1), int32_t(args.a2), int32_t(args.a3), int32_t(stackArg(4)));
        ret = 1;
    } else {
        lastError_ = 87;
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleSetRectEmpty(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const int32_t rect[4]{};
    ret = args.a0 && uc_mem_write(uc_, args.a0, rect, sizeof(rect)) == UC_ERR_OK ? 1 : 0;
    return true;
}
