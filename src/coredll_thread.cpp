#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

void SyntheticDllRuntime::registerCoredllThreadExports(SyntheticModule& module) {
    struct CoreDllThread {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.thread",
                {
                    {0x01EC, {"CreateThread", Code::CoreDllCreateThread, &SyntheticDllRuntime::handleCreateThread}},
                    {0x01EF, {"CreateEventW", Code::CoreDllCreateEventW, &SyntheticDllRuntime::handleCreateEventW}},
                    {0x01EE, {"EventModify", Code::CoreDllEventModify, &SyntheticDllRuntime::handleEventModify}},
                    {0x01F2, {"WaitForMultipleObjects", Code::CoreDllWaitForMultipleObjects, &SyntheticDllRuntime::handleWaitForMultipleObjects}},
                    {0x01F4, {"ResumeThread", Code::CoreDllResumeThread, &SyntheticDllRuntime::handleResumeThread}},
                    {0x0202, {"SetThreadPriority", Code::CoreDllSetThreadPriority, &SyntheticDllRuntime::handleSetThreadPriority}},
                    {0x0203, {"GetThreadPriority", Code::CoreDllGetThreadPriority, &SyntheticDllRuntime::handleGetThreadPriority}},
                    {0x022B, {"CreateMutexW", Code::CoreDllCreateMutexW, &SyntheticDllRuntime::handleCreateMutexW}},
                    {0x022C, {"ReleaseMutex", Code::CoreDllReleaseMutex, &SyntheticDllRuntime::handleReleaseMutex}},
                    {0x026D, {"CeSetThreadPriority", Code::CoreDllSetThreadPriority, &SyntheticDllRuntime::handleSetThreadPriority}},
                },
            };
        }
    };

    const CoreDllThread thread;
    registerHandlers(module, thread.group());
}

bool SyntheticDllRuntime::handleCreateThread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const uint32_t startAddress = args.a2;
    const uint32_t parameter = args.a3;
    const uint32_t flags = stackArg(4);
    const uint32_t threadIdPtr = stackArg(5);
    ret = createGuestThread(startAddress, parameter, flags);
    if (threadIdPtr) {
        auto thread = guestThreads_.find(ret);
        writeU32(threadIdPtr, thread == guestThreads_.end() ? ret : thread->second.threadId);
    }
    spdlog::info("CreateThread guestHandle=0x{:08x} start=0x{:08x} param=0x{:08x} flags=0x{:08x} idPtr=0x{:08x}",
                 ret, startAddress, parameter, flags, threadIdPtr);
    return true;
}

bool SyntheticDllRuntime::handleCreateEventW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    HANDLE host = CreateEventW(nullptr, args.a1 != 0, args.a2 != 0, nullptr);
    if (host) {
        ret = makeGuestHandle({GuestHandle::Kind::HostEvent, reinterpret_cast<uintptr_t>(host), 0});
        lastError_ = 0;
    } else {
        ret = 0;
        lastError_ = GetLastError();
    }
    return true;
}

bool SyntheticDllRuntime::handleEventModify(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostEvent) {
        lastError_ = 6;
        ret = 0;
        return true;
    }

    HANDLE host = reinterpret_cast<HANDLE>(handle->hostValue);
    BOOL ok = FALSE;
    if (host) {
        if (args.a1 == 1) ok = PulseEvent(host);
        else if (args.a1 == 2) ok = ResetEvent(host);
        else if (args.a1 == 3) ok = SetEvent(host);
    }
    ret = ok ? 1 : 0;
    if (!ret) lastError_ = GetLastError();
    else lastError_ = 0;

    if (ret && (args.a1 == 1 || args.a1 == 3)) {
        for (auto& [threadHandle, thread] : guestThreads_) {
            (void)threadHandle;
            if (thread.state == GuestThreadRunState::Waiting && thread.waitHandle == args.a0) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.context.registers[UC_MIPS_REG_V0] = 0;
            }
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWaitForMultipleObjects(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = waitForMultipleGuestObjects(args.a0, args.a1, args.a2 != 0);
    return true;
}

bool SyntheticDllRuntime::handleResumeThread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = resumeGuestThread(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleSetThreadPriority(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::GuestThread ? 1 : 0;
    lastError_ = ret ? 0 : 6;
    return true;
}

bool SyntheticDllRuntime::handleGetThreadPriority(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::GuestThread ? 251 : 0xffffffffu;
    lastError_ = ret == 0xffffffffu ? 6 : 0;
    return true;
}

bool SyntheticDllRuntime::handleCreateMutexW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    HANDLE host = CreateMutexW(nullptr, args.a1 != 0, nullptr);
    if (host) {
        ret = makeGuestHandle({GuestHandle::Kind::HostMutex, reinterpret_cast<uintptr_t>(host), 0});
        lastError_ = 0;
    } else {
        ret = 0;
        lastError_ = GetLastError();
    }
    return true;
}

bool SyntheticDllRuntime::handleReleaseMutex(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (handle && handle->kind == GuestHandle::Kind::HostMutex && handle->hostValue) {
        ret = ReleaseMutex(reinterpret_cast<HANDLE>(handle->hostValue)) ? 1 : 0;
        lastError_ = ret ? 0 : GetLastError();
    } else {
        ret = handle ? 1 : 0;
        if (!ret) lastError_ = 6;
        else lastError_ = 0;
    }
    return true;
}
