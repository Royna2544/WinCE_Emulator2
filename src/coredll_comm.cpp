#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

void SyntheticDllRuntime::registerCoredllCommExports(SyntheticModule& module) {
    struct CoreDllComm {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.comm",
                {
                    {0x006C, {"ClearCommError", Code::CoreDllClearCommError, &SyntheticDllRuntime::handleClearCommError}},
                    {0x0071, {"GetCommState", Code::CoreDllGetCommState, &SyntheticDllRuntime::handleGetCommState}},
                    {0x0073, {"PurgeComm", Code::CoreDllPurgeComm, &SyntheticDllRuntime::handlePurgeComm}},
                    {0x0075, {"SetCommMask", Code::CoreDllSetCommMask, &SyntheticDllRuntime::handleSetCommMask}},
                    {0x0076, {"SetCommState", Code::CoreDllSetCommState, &SyntheticDllRuntime::handleSetCommState}},
                    {0x0077, {"SetCommTimeouts", Code::CoreDllSetCommTimeouts, &SyntheticDllRuntime::handleSetCommTimeouts}},
                    {0x0078, {"SetupComm", Code::CoreDllSetupComm, &SyntheticDllRuntime::handleSetupComm}},
                },
            };
        }
    };

    const CoreDllComm comm;
    registerHandlers(module, comm.group());
}

bool SyntheticDllRuntime::handleGetCommState(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        auto debugName = fileHandleDebugNames_.find(args.a0);
        const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
        lastError_ = 120;
        ret = 0;
        spdlog::info("GetCommState guest device handle=0x{:08x} name=\"{}\" has no host serial bridge yet -> 0",
                     args.a0, debugPath);
        return true;
    }
    HANDLE host = reinterpret_cast<HANDLE>(handle->hostValue);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    const BOOL ok = GetCommState(host, &dcb);
    if (ok && args.a1) {
        uint32_t guestLength = readU32(args.a1);
        if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
        uc_mem_write(uc_, args.a1, &dcb, guestLength);
        writeU32(args.a1, guestLength);
    }
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleSetCommState(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = 120;
        ret = 0;
        return true;
    }
    HANDLE host = reinterpret_cast<HANDLE>(handle->hostValue);
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(host, &dcb);
    BOOL ok = FALSE;
    if (args.a1) {
        uint32_t guestLength = readU32(args.a1);
        if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
        uc_mem_read(uc_, args.a1, &dcb, guestLength);
        dcb.DCBlength = sizeof(dcb);
        ok = SetCommState(host, &dcb);
    }
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleSetCommTimeouts(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = handle ? 120 : 6;
        ret = 0;
        return true;
    }
    COMMTIMEOUTS timeouts{};
    const BOOL ok = args.a1 && uc_mem_read(uc_, args.a1, &timeouts, sizeof(timeouts)) == UC_ERR_OK &&
                    SetCommTimeouts(reinterpret_cast<HANDLE>(handle->hostValue), &timeouts);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleSetCommMask(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = handle ? 120 : 6;
        ret = 0;
        return true;
    }
    const BOOL ok = SetCommMask(reinterpret_cast<HANDLE>(handle->hostValue), args.a1);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleSetupComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = handle ? 120 : 6;
        ret = 0;
        return true;
    }
    const BOOL ok = SetupComm(reinterpret_cast<HANDLE>(handle->hostValue), args.a1, args.a2);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handlePurgeComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = handle ? 120 : 6;
        ret = 0;
        return true;
    }
    const BOOL ok = PurgeComm(reinterpret_cast<HANDLE>(handle->hostValue), args.a1);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleClearCommError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        lastError_ = handle ? 120 : 6;
        ret = 0;
        return true;
    }
    DWORD errors = 0;
    COMSTAT stat{};
    const BOOL ok = ClearCommError(reinterpret_cast<HANDLE>(handle->hostValue), &errors, &stat);
    if (ok && args.a1) writeU32(args.a1, errors);
    if (ok && args.a2) uc_mem_write(uc_, args.a2, &stat, sizeof(stat));
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    return true;
}
