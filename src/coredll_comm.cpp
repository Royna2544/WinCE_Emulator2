#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <string>

namespace {

void applyGuestSerialConfigToDcb(DCB& dcb, uint32_t baud, const std::string& configuredMode) {
    dcb = {};
    dcb.DCBlength = sizeof(dcb);
    dcb.BaudRate = baud ? baud : 9600;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;

    std::string mode = configuredMode.empty() ? "8N1" : configuredMode;
    for (char& ch : mode) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (mode.size() != 3) return;
    if (mode[0] >= '5' && mode[0] <= '8') dcb.ByteSize = BYTE(mode[0] - '0');
    switch (mode[1]) {
    case 'O':
        dcb.Parity = ODDPARITY;
        dcb.fParity = TRUE;
        break;
    case 'E':
        dcb.Parity = EVENPARITY;
        dcb.fParity = TRUE;
        break;
    default:
        dcb.Parity = NOPARITY;
        dcb.fParity = FALSE;
        break;
    }
    dcb.StopBits = mode[2] == '2' ? TWOSTOPBITS : ONESTOPBIT;
}

void applyCeSerialModeToDcb(DCB& dcb, const CeDevice::SerialMode& mode) {
    dcb = {};
    dcb.DCBlength = sizeof(dcb);
    dcb.BaudRate = mode.baud ? mode.baud : 9600;
    dcb.ByteSize = mode.byteSize ? mode.byteSize : 8;
    dcb.Parity = mode.parity;
    dcb.StopBits = mode.stopBits;
    dcb.fBinary = TRUE;
    dcb.fParity = mode.parity != NOPARITY;
}

CeDevice::SerialMode ceSerialModeFromDcb(const DCB& dcb) {
    CeDevice::SerialMode mode{};
    mode.baud = dcb.BaudRate;
    mode.byteSize = dcb.ByteSize;
    mode.parity = dcb.Parity;
    mode.stopBits = dcb.StopBits;
    return mode;
}

CeDevice::CommTimeouts ceCommTimeoutsFromWin32(const COMMTIMEOUTS& timeouts) {
    return CeDevice::CommTimeouts{
        timeouts.ReadIntervalTimeout,
        timeouts.ReadTotalTimeoutMultiplier,
        timeouts.ReadTotalTimeoutConstant,
        timeouts.WriteTotalTimeoutMultiplier,
        timeouts.WriteTotalTimeoutConstant,
    };
}

} // namespace

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
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        DCB dcb{};
        const CeDevice::SerialState* serial = ceDevice_.serialState(args.a0);
        if (serial) {
            applyCeSerialModeToDcb(dcb, serial->mode);
        } else {
            applyGuestSerialConfigToDcb(dcb, defaultSerialBaud_, defaultSerialMode_);
        }
        if (args.a1) {
            uint32_t guestLength = readU32(args.a1);
            if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
            uc_mem_write(uc_, args.a1, &dcb, guestLength);
            writeU32(args.a1, guestLength);
        }
        lastError_ = 0;
        ret = 1;
        spdlog::info("GetCommState guest device handle=0x{:08x} name=\"{}\" -> 1 baud={} byteSize={} parity={} stopBits={}",
                     args.a0, debugPath, dcb.BaudRate, static_cast<unsigned>(dcb.ByteSize),
                     static_cast<unsigned>(dcb.Parity), static_cast<unsigned>(dcb.StopBits));
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
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("GetCommState host serial handle=0x{:08x} path=\"{}\" -> {} lastError={} baud={} byteSize={} parity={} stopBits={}",
                 args.a0, debugPath, ret, lastError_, ok ? dcb.BaudRate : 0,
                 ok ? static_cast<unsigned>(dcb.ByteSize) : 0,
                 ok ? static_cast<unsigned>(dcb.Parity) : 0,
                 ok ? static_cast<unsigned>(dcb.StopBits) : 0);
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
        DCB dcb{};
        BOOL ok = FALSE;
        if (args.a1) {
            uint32_t guestLength = readU32(args.a1);
            if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
            ok = uc_mem_read(uc_, args.a1, &dcb, guestLength) == UC_ERR_OK;
            if (ok) {
                dcb.DCBlength = sizeof(dcb);
                ceDevice_.setSerialMode(args.a0, ceSerialModeFromDcb(dcb));
            }
        }
        lastError_ = ok ? 0 : 87;
        ret = ok ? 1 : 0;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("SetCommState guest device handle=0x{:08x} name=\"{}\" -> {} lastError={} baud={} byteSize={} parity={} stopBits={}",
                     args.a0, debugPath, ret, lastError_,
                     ok ? dcb.BaudRate : 0, ok ? static_cast<unsigned>(dcb.ByteSize) : 0,
                     ok ? static_cast<unsigned>(dcb.Parity) : 0, ok ? static_cast<unsigned>(dcb.StopBits) : 0);
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
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("SetCommState host serial handle=0x{:08x} path=\"{}\" -> {} lastError={} baud={} byteSize={} parity={} stopBits={}",
                 args.a0, debugPath, ret, lastError_, ok ? dcb.BaudRate : 0,
                 ok ? static_cast<unsigned>(dcb.ByteSize) : 0,
                 ok ? static_cast<unsigned>(dcb.Parity) : 0,
                 ok ? static_cast<unsigned>(dcb.StopBits) : 0);
    return true;
}

bool SyntheticDllRuntime::handleSetCommTimeouts(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        COMMTIMEOUTS timeouts{};
        const BOOL ok = args.a1 && uc_mem_read(uc_, args.a1, &timeouts, sizeof(timeouts)) == UC_ERR_OK;
        if (ok) ceDevice_.setSerialTimeouts(args.a0, ceCommTimeoutsFromWin32(timeouts));
        lastError_ = ok ? 0 : 87;
        ret = ok ? 1 : 0;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("SetCommTimeouts guest device handle=0x{:08x} name=\"{}\" -> {} lastError={} interval={} readMultiplier={} readConstant={} writeMultiplier={} writeConstant={}",
                     args.a0, debugPath, ret, lastError_, timeouts.ReadIntervalTimeout,
                     timeouts.ReadTotalTimeoutMultiplier, timeouts.ReadTotalTimeoutConstant,
                     timeouts.WriteTotalTimeoutMultiplier, timeouts.WriteTotalTimeoutConstant);
        return true;
    }
    COMMTIMEOUTS timeouts{};
    const BOOL ok = args.a1 && uc_mem_read(uc_, args.a1, &timeouts, sizeof(timeouts)) == UC_ERR_OK &&
                    SetCommTimeouts(reinterpret_cast<HANDLE>(handle->hostValue), &timeouts);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("SetCommTimeouts host serial handle=0x{:08x} path=\"{}\" -> {} lastError={} interval={} readMultiplier={} readConstant={} writeMultiplier={} writeConstant={}",
                 args.a0, debugPath, ret, lastError_, timeouts.ReadIntervalTimeout,
                 timeouts.ReadTotalTimeoutMultiplier, timeouts.ReadTotalTimeoutConstant,
                 timeouts.WriteTotalTimeoutMultiplier, timeouts.WriteTotalTimeoutConstant);
    return true;
}

bool SyntheticDllRuntime::handleSetCommMask(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        ceDevice_.setSerialMask(args.a0, args.a1);
        lastError_ = 0;
        ret = 1;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("SetCommMask guest device handle=0x{:08x} name=\"{}\" mask=0x{:08x} -> 1",
                     args.a0, debugPath, args.a1);
        return true;
    }
    const BOOL ok = SetCommMask(reinterpret_cast<HANDLE>(handle->hostValue), args.a1);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("SetCommMask host serial handle=0x{:08x} path=\"{}\" mask=0x{:08x} -> {} lastError={}",
                 args.a0, debugPath, args.a1, ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleSetupComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        ceDevice_.setSerialQueueSizes(args.a0, args.a1, args.a2);
        lastError_ = 0;
        ret = 1;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("SetupComm guest device handle=0x{:08x} name=\"{}\" inQueue={} outQueue={} -> 1",
                     args.a0, debugPath, args.a1, args.a2);
        return true;
    }
    const BOOL ok = SetupComm(reinterpret_cast<HANDLE>(handle->hostValue), args.a1, args.a2);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("SetupComm host serial handle=0x{:08x} path=\"{}\" inQueue={} outQueue={} -> {} lastError={}",
                 args.a0, debugPath, args.a1, args.a2, ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handlePurgeComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        ceDevice_.markSerialPurged(args.a0, args.a1);
        lastError_ = 0;
        ret = 1;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("PurgeComm guest device handle=0x{:08x} name=\"{}\" flags=0x{:08x} -> 1",
                     args.a0, debugPath, args.a1);
        return true;
    }
    const BOOL ok = PurgeComm(reinterpret_cast<HANDLE>(handle->hostValue), args.a1);
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("PurgeComm host serial handle=0x{:08x} path=\"{}\" flags=0x{:08x} -> {} lastError={}",
                 args.a0, debugPath, args.a1, ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleClearCommError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice)) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        const DWORD errors = 0;
        COMSTAT stat{};
        stat.cbInQue = static_cast<DWORD>(std::min<size_t>(remoteSerialByteCount(), 0xffffffffu));
        if (args.a1) writeU32(args.a1, errors);
        if (args.a2) uc_mem_write(uc_, args.a2, &stat, sizeof(stat));
        lastError_ = 0;
        ret = 1;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        spdlog::info("ClearCommError guest device handle=0x{:08x} name=\"{}\" -> 1 errors=0x{:08x} cbInQue={} cbOutQue={}",
                     args.a0, debugPath, errors, stat.cbInQue, stat.cbOutQue);
        return true;
    }
    DWORD errors = 0;
    COMSTAT stat{};
    const BOOL ok = ClearCommError(reinterpret_cast<HANDLE>(handle->hostValue), &errors, &stat);
    if (ok && args.a1) writeU32(args.a1, errors);
    if (ok && args.a2) uc_mem_write(uc_, args.a2, &stat, sizeof(stat));
    ret = ok ? 1 : 0;
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("ClearCommError host serial handle=0x{:08x} path=\"{}\" -> {} lastError={} errors=0x{:08x} cbInQue={} cbOutQue={}",
                 args.a0, debugPath, ret, lastError_, errors, stat.cbInQue, stat.cbOutQue);
    return true;
}
