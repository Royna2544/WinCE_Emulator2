#include "synthetic_dll.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>

void SyntheticDllRuntime::registerCoredllSystemExports(SyntheticModule& module) {
    struct CoreDllSystem {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.system",
                {
                    {0x00BA, {"GetACP", Code::CoreDllGetACP, &SyntheticDllRuntime::handleGetACP}},
                    {0x00D1, {"IsValidLocale", Code::CoreDllIsValidLocale, &SyntheticDllRuntime::handleIsValidLocale}},
                    {0x0204, {"GetLastError", Code::CoreDllGetLastError, &SyntheticDllRuntime::handleGetLastError}},
                    {0x0205, {"SetLastError", Code::CoreDllSetLastError, &SyntheticDllRuntime::handleSetLastError}},
                    {0x020A, {"IsBadReadPtr", Code::CoreDllIsBadReadPtr, &SyntheticDllRuntime::handleIsBadReadPtr}},
                    {0x020B, {"IsBadWritePtr", Code::CoreDllIsBadWritePtr, &SyntheticDllRuntime::handleIsBadWritePtr}},
                    {0x0280, {"GetProcessIndexFromID", Code::CoreDllGetProcessIndexFromID, &SyntheticDllRuntime::handleGetProcessIndexFromID}},
                    {0x04BD, {"IsProcessDying", Code::CoreDllIsProcessDying, &SyntheticDllRuntime::handleIsProcessDying}},
                },
            };
        }
    };

    const CoreDllSystem system;
    registerHandlers(module, system.group());
}

bool SyntheticDllRuntime::handleGetProcessIndexFromID(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    // Windows CE process IDs are process handles; runtime evidence and CE docs
    // show the low six bits identify the process slot.
    ret = args.a0 & 0x3fu;
    return true;
}

bool SyntheticDllRuntime::handleGetACP(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    // The target image and real-device dump are Korean WinCE, and CP_ACP is
    // already translated as CP949 in the string conversion bridge.
    ret = 949;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleIsValidLocale(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const uint32_t lcid = args.a0;
    const std::array<uint32_t, 5> ceLocales{0x0412, 0x0812, 0x0409, 0x0411, 0x0000};
    ret = std::find(ceLocales.begin(), ceLocales.end(), lcid) != ceLocales.end() ? 1 : 0;
#if defined(_WIN32)
    if (!ret) ret = ::IsValidLocale(LCID(lcid), DWORD(args.a1)) ? 1 : 0;
#endif
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetLastError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = lastError_;
    return true;
}

bool SyntheticDllRuntime::handleSetLastError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    lastError_ = args.a0;
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleIsBadReadPtr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = isGuestRangeReadable(args.a0, args.a1) ? 0 : 1;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleIsBadWritePtr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = isGuestRangeReadable(args.a0, args.a1) ? 0 : 1;
    lastError_ = 0;
    return true;
}

bool SyntheticDllRuntime::handleIsProcessDying(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = 0;
    return true;
}
