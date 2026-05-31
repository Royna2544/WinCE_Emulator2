#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
uint64_t hostTickMilliseconds() {
#if defined(_WIN32)
    return GetTickCount64();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string utf8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
#else
    return path.string();
#endif
}
}

SyntheticDllRuntime::GuestCpuContext SyntheticDllRuntime::captureGuestCpuContext() const {
    static constexpr int kRegisters[] = {
        UC_MIPS_REG_PC, UC_MIPS_REG_RA, UC_MIPS_REG_SP, UC_MIPS_REG_GP, UC_MIPS_REG_FP,
        UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3,
        UC_MIPS_REG_V0, UC_MIPS_REG_V1, UC_MIPS_REG_AT,
        UC_MIPS_REG_T0, UC_MIPS_REG_T1, UC_MIPS_REG_T2, UC_MIPS_REG_T3, UC_MIPS_REG_T4,
        UC_MIPS_REG_T5, UC_MIPS_REG_T6, UC_MIPS_REG_T7, UC_MIPS_REG_T8, UC_MIPS_REG_T9,
        UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3, UC_MIPS_REG_S4,
        UC_MIPS_REG_S5, UC_MIPS_REG_S6, UC_MIPS_REG_S7,
        UC_MIPS_REG_K0, UC_MIPS_REG_K1, UC_MIPS_REG_HI, UC_MIPS_REG_LO,
    };
    GuestCpuContext context;
    context.valid = true;
    for (int regId : kRegisters) {
        uint32_t value = 0;
        uc_reg_read(uc_, regId, &value);
        context.registers[regId] = value;
    }
    return context;
}

SyntheticDllRuntime::GuestCpuContext SyntheticDllRuntime::initialGuestThreadContext(
    uint32_t startAddress,
    uint32_t parameter,
    uint32_t stackTop) const {
    GuestCpuContext context = captureGuestCpuContext();
    for (auto& [regId, value] : context.registers) {
        if (regId != UC_MIPS_REG_GP) value = 0;
    }
    context.registers[UC_MIPS_REG_PC] = startAddress;
    context.registers[UC_MIPS_REG_RA] = threadExitStub_;
    context.registers[UC_MIPS_REG_SP] = stackTop;
    context.registers[UC_MIPS_REG_FP] = 0;
    context.registers[UC_MIPS_REG_A0] = parameter;
    context.registers[UC_MIPS_REG_T9] = startAddress;
    return context;
}

void SyntheticDllRuntime::restoreGuestCpuContext(const GuestCpuContext& context) const {
    if (!context.valid) return;
    for (const auto& [regId, value] : context.registers) {
        uc_reg_write(uc_, regId, &value);
    }
}

const SyntheticDllRuntime::GuestThreadState* SyntheticDllRuntime::activeGuestThreadState() const {
    if (!ceKernel_.activeGuestThread()) return nullptr;
    auto it = ceKernel_.threads().find(ceKernel_.activeGuestThread());
    return it == ceKernel_.threads().end() ? nullptr : &it->second;
}

std::string SyntheticDllRuntime::currentProcessModulePath() const {
    if (const auto* thread = activeGuestThreadState()) {
        if (!thread->modulePath.empty()) return thread->modulePath;
    }
    return mainModulePath_;
}

uint32_t SyntheticDllRuntime::currentProcessModuleBase() const {
    if (const auto* thread = activeGuestThreadState()) {
        if (thread->moduleBase) return thread->moduleBase;
    }
    return mainModuleBase_;
}

uint32_t SyntheticDllRuntime::createGuestThread(uint32_t startAddress, uint32_t parameter, uint32_t flags) {
    if (!startAddress || !threadExitStub_) {
        lastError_ = 87;
        return 0;
    }
    constexpr uint32_t kGuestThreadStackSize = 0x10000;
    const uint32_t stackBase = allocate(kGuestThreadStackSize, true);
    if (!stackBase) {
        lastError_ = 8;
        return 0;
    }
    const uint32_t guestHandle = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
    const uint32_t stackTop = (stackBase + kGuestThreadStackSize - 0x100u) & ~0x0fu;
    GuestThreadState thread;
    thread.handle = guestHandle;
    thread.threadId = ceKernel_.nextGuestThreadId()++;
    thread.startAddress = startAddress;
    thread.parameter = parameter;
    thread.stackBase = stackBase;
    thread.stackSize = kGuestThreadStackSize;
    thread.tlsBase = allocate(64 * sizeof(uint32_t), true);
    if (!thread.tlsBase) {
        releaseAllocation(stackBase);
        lastError_ = 8;
        return 0;
    }
    thread.suspendCount = (flags & 0x00000004u) ? 1 : 0;
    thread.state = thread.suspendCount ? GuestThreadRunState::Suspended : GuestThreadRunState::Runnable;
    if (const auto* active = activeGuestThreadState()) {
        thread.processHandle = active->processHandle;
        thread.processId = active->processId;
        thread.moduleBase = active->moduleBase;
        thread.modulePath = active->modulePath;
    } else {
        thread.processHandle = ceKernel_.mainProcessPseudoHandle();
        thread.processId = ceKernel_.mainProcessId();
        thread.moduleBase = mainModuleBase_;
        thread.modulePath = mainModulePath_;
    }
    thread.context = initialGuestThreadContext(startAddress, parameter, stackTop);
    ceKernel_.threads()[guestHandle] = std::move(thread);
    lastError_ = 0;
    return guestHandle;
}

bool SyntheticDllRuntime::startGuestProcessImage(const std::string& guestApplication,
                                                 const std::filesystem::path& hostApplication,
                                                 uint32_t moduleBase,
                                                 uint32_t entryPoint,
                                                 const std::string& commandLine,
                                                 uint32_t& processHandle,
                                                 uint32_t& threadHandle,
                                                 uint32_t& processId,
                                                 uint32_t& threadId) {
    processHandle = 0;
    threadHandle = 0;
    processId = 0;
    threadId = 0;
    if (!moduleBase || !entryPoint || !threadExitStub_) {
        lastError_ = 87;
        return false;
    }

    constexpr uint32_t kGuestThreadStackSize = 0x10000;
    const uint32_t stackBase = allocate(kGuestThreadStackSize, true);
    if (!stackBase) {
        lastError_ = 8;
        return false;
    }
    const uint32_t commandLineChars = uint32_t(commandLine.size() + 1);
    const uint32_t commandLinePtr = allocate(std::max<uint32_t>(2, commandLineChars * 2), true);
    if (!commandLinePtr) {
        releaseAllocation(stackBase);
        lastError_ = 8;
        return false;
    }
    writeUtf16(commandLinePtr, commandLine, commandLineChars);

    processId = ceKernel_.nextGuestProcessId()++;
    processHandle = makeGuestHandle({GuestHandle::Kind::GuestProcess, 0, processId});
    threadHandle = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
    const uint32_t stackTop = (stackBase + kGuestThreadStackSize - 0x100u) & ~0x0fu;

    GuestThreadState thread;
    thread.handle = threadHandle;
    thread.threadId = ceKernel_.nextGuestThreadId()++;
    thread.startAddress = entryPoint;
    thread.parameter = commandLinePtr;
    thread.stackBase = stackBase;
    thread.stackSize = kGuestThreadStackSize;
    thread.tlsBase = allocate(64 * sizeof(uint32_t), true);
    if (!thread.tlsBase) {
        releaseAllocation(commandLinePtr);
        releaseAllocation(stackBase);
        lastError_ = 8;
        return false;
    }
    thread.processHandle = processHandle;
    thread.processId = processId;
    thread.moduleBase = moduleBase;
    thread.modulePath = guestApplication.empty()
        ? ("\\" + pathToUtf8(hostApplication.filename()))
        : guestApplication;
    thread.state = GuestThreadRunState::Runnable;
    thread.context = initialGuestThreadContext(entryPoint, 0, stackTop);
    thread.context.registers[UC_MIPS_REG_GP] = moduleBase + 0x8000;
    thread.context.registers[UC_MIPS_REG_A0] = moduleBase;
    thread.context.registers[UC_MIPS_REG_A1] = 0;
    thread.context.registers[UC_MIPS_REG_A2] = commandLinePtr;
    thread.context.registers[UC_MIPS_REG_A3] = 1;
    thread.context.registers[UC_MIPS_REG_T9] = entryPoint;

    threadId = thread.threadId;
    ceKernel_.threads()[threadHandle] = std::move(thread);
    lastError_ = 0;
    spdlog::info("CreateProcessW guest image scheduled app=\"{}\" host=\"{}\" base=0x{:08x} entry=0x{:08x} process=0x{:08x}/{} thread=0x{:08x}/{} cmd=\"{}\"",
                 guestApplication, pathToUtf8(hostApplication), moduleBase, entryPoint,
                 processHandle, processId, threadHandle, threadId, commandLine);
    return true;
}

uint32_t SyntheticDllRuntime::resumeGuestThread(uint32_t guestHandle) {
    auto* handle = lookupGuestHandle(guestHandle);
    if (!handle || handle->kind != GuestHandle::Kind::GuestThread) {
        lastError_ = 6;
        return 0xffffffffu;
    }
    auto thread = ceKernel_.threads().find(guestHandle);
    if (thread == ceKernel_.threads().end()) {
        lastError_ = 0;
        return 1;
    }
    if (thread->second.state == GuestThreadRunState::Terminated) {
        lastError_ = 0;
        return 0;
    }
    const uint32_t previousSuspendCount = thread->second.suspendCount;
    if (thread->second.suspendCount) --thread->second.suspendCount;
    if (!thread->second.suspendCount && thread->second.state == GuestThreadRunState::Suspended) {
        thread->second.state = GuestThreadRunState::Runnable;
    }
    lastError_ = 0;
    return previousSuspendCount;
}

void SyntheticDllRuntime::wakeGuestThreadsWaitingForMessage() {
    refreshSignaledGuestWaits();
    auto hasMessagesForThread = [this](uint32_t threadHandle) {
        return ceGwe_.hasMessagesForOwner(threadHandle);
    };
    for (const uint32_t threadHandle : ceKernel_.wakeThreadsWaitingForMessage(hasMessagesForThread)) {
        spdlog::info("guest thread message wait satisfied handle=0x{:08x}", threadHandle);
    }
}

void SyntheticDllRuntime::refreshPendingSerialReads() {
    const uint64_t nowMs = hostTickMilliseconds();
    for (const CeDevice::PendingSerialRead& read : ceDevice_.pendingSerialReads()) {
        auto thread = ceKernel_.threads().find(read.threadHandle);
        if (thread == ceKernel_.threads().end() ||
            thread->second.state != GuestThreadRunState::WaitingForSerialRead) {
            ceDevice_.completePendingSerialRead(read.threadHandle);
            continue;
        }

        const bool hasData = remoteSerialByteCount() != 0;
        const bool timedOut = read.deadlineMs && nowMs >= read.deadlineMs;
        if (!hasData && !timedOut) continue;

        uint32_t transferred = 0;
        if (hasData && read.requested && read.buffer) {
            constexpr uint32_t kMaxVirtualSerialReadChunk = 64 * 1024;
            const uint32_t requested = std::min(read.requested, kMaxVirtualSerialReadChunk);
            std::vector<uint8_t> bytes(requested);
            transferred = static_cast<uint32_t>(readRemoteSerialBytes(bytes.data(), bytes.size()));
            if (transferred) {
                uc_mem_write(uc_, read.buffer, bytes.data(), transferred);
            }
        }
        if (read.transferredPtr) writeU32(read.transferredPtr, transferred);

        thread->second.context.registers[UC_MIPS_REG_V0] = 1;
        thread->second.state = GuestThreadRunState::Runnable;
        thread->second.waitHandle = 0;
        thread->second.waitHandles.clear();
        thread->second.waitForMessages = false;
        thread->second.waitWakeMask = 0;
        thread->second.waitTimeoutResult = 0;
        thread->second.sleepUntilMs = 0;
        ceDevice_.completePendingSerialRead(read.threadHandle);
        lastError_ = 0;
        spdlog::info("ReadFile virtual serial no-data wake handle=0x{:08x} thread=0x{:08x} transferred={} reason={}",
                     read.serialHandle,
                     read.threadHandle,
                     transferred,
                     transferred ? "data" : "timeout");
    }
}

void SyntheticDllRuntime::refreshSignaledGuestWaits() {
    refreshCompletedHostWaveBuffers();
    refreshPendingSerialReads();
#if defined(_WIN32)
    auto hostWaitProbe = [](const GuestHandle& handle) {
        const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
        if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
        if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
        return CeKernel::HostWaitResult{false, true, GetLastError()};
    };
    auto hasMessagesForThread = [this](uint32_t threadHandle) {
        return ceGwe_.hasMessagesForOwner(threadHandle);
    };
    for (const auto& event : ceKernel_.refreshSignaledWaits(hostTickMilliseconds(),
                                                           UC_MIPS_REG_V0,
                                                           hostWaitProbe,
                                                           hasMessagesForThread)) {
        switch (event.kind) {
        case CeKernel::WaitRefreshKind::SleepSatisfied:
            lastError_ = 0;
            spdlog::debug("guest thread sleep satisfied handle=0x{:08x}", event.threadHandle);
            break;
        case CeKernel::WaitRefreshKind::InvalidHandle:
            lastError_ = event.error;
            spdlog::warn("guest thread wait invalid handle=0x{:08x} waitHandle=0x{:08x}",
                         event.threadHandle, event.waitHandle);
            break;
        case CeKernel::WaitRefreshKind::WaitFailed:
            lastError_ = event.error;
            spdlog::warn("guest thread wait failed handle=0x{:08x} waitHandle=0x{:08x} error={}",
                         event.threadHandle, event.waitHandle, lastError_);
            break;
        case CeKernel::WaitRefreshKind::WaitSatisfied:
            lastError_ = 0;
            spdlog::info("guest thread wait satisfied handle=0x{:08x} waitHandle=0x{:08x} index={}",
                         event.threadHandle, event.waitHandle, event.index);
            break;
        case CeKernel::WaitRefreshKind::WaitAllSatisfied:
            lastError_ = 0;
            spdlog::info("guest thread wait-all satisfied handle=0x{:08x} count={}",
                         event.threadHandle, event.count);
            break;
        case CeKernel::WaitRefreshKind::QueueEventSatisfied:
            lastError_ = 0;
            spdlog::info("guest thread queue-event satisfied handle=0x{:08x}",
                         event.threadHandle);
            break;
        case CeKernel::WaitRefreshKind::MessageWaitSatisfied:
            lastError_ = 0;
            spdlog::info("guest thread msg-wait satisfied handle=0x{:08x} waitCount={}",
                         event.threadHandle, event.count);
            break;
        }
    }
#endif
}

bool SyntheticDllRuntime::hasRunnableGuestThread() {
    refreshSignaledGuestWaits();
    return ceKernel_.hasRunnableThread();
}

uint32_t SyntheticDllRuntime::guestContextReg(const GuestCpuContext& context, int regId) const {
    auto it = context.registers.find(regId);
    return it == context.registers.end() ? 0 : it->second;
}

bool SyntheticDllRuntime::isGuestContextPcReadable(const GuestCpuContext& context) const {
    if (!context.valid) return false;
    const uint32_t pc = guestContextReg(context, UC_MIPS_REG_PC);
    return pc && isGuestRangeReadable(pc, 4);
}

uint32_t SyntheticDllRuntime::normalizeGuestCodeAddress(uint32_t address, const char* why) const {
    if (!address) return 0;
    if (isGuestRangeReadable(address, 4)) return address;
    const uint32_t slotAddress = address & 0x01ffffffu;
    if (slotAddress && slotAddress != address && isGuestRangeReadable(slotAddress, 4)) {
        if (why) {
            spdlog::warn("{} normalized unreadable code address 0x{:08x} -> 0x{:08x}",
                         why, address, slotAddress);
        }
        return slotAddress;
    }
    return address;
}

uint32_t SyntheticDllRuntime::guestGpForCodeAddress(uint32_t address) const {
    const uint32_t normalized = normalizeGuestCodeAddress(address);
    for (const auto& [base, module] : loadedModulesByBase_) {
        const uint64_t begin = base;
        const uint64_t end = begin + (module.imageSize ? module.imageSize : 0x1000u);
        if (normalized >= begin && uint64_t(normalized) < end) {
            return base + 0x8000u;
        }
    }
    uint32_t gp = 0;
    uc_reg_read(uc_, UC_MIPS_REG_GP, &gp);
    return gp;
}

bool SyntheticDllRuntime::restoreMainThreadContextIfRunnable(const char* reason) {
    if (!ceKernel_.mainThreadContext().valid) return false;
    const uint32_t pc = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_PC);
    const uint32_t ra = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_RA);
    const uint32_t sp = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_SP);
    if (pc && isGuestRangeReadable(pc, 4)) {
        updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
        restoreGuestCpuContext(ceKernel_.mainThreadContext());
        spdlog::debug("restored parked main thread context reason={} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                      reason ? reason : "cooperate", pc, ra, sp);
        return true;
    }
    spdlog::warn("discarded unreadable parked main thread context reason={} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                 reason ? reason : "cooperate", pc, ra, sp);
    ceKernel_.mainThreadContext().valid = false;
    return false;
}

bool SyntheticDllRuntime::hasReadyPendingBlockingMainContinuation() {
    if (pendingBlockingApis_.empty() || !ceKernel_.mainThreadContext().valid) return false;

    const PendingBlockingApi& pending = pendingBlockingApis_.back();
    constexpr uint16_t kSleepOrdinal = 0x01f0;
    constexpr uint16_t kWaitForSingleObjectOrdinal = 0x01f1;
    constexpr uint32_t kInfiniteTimeout = 0xffffffffu;

    if (pending.ordinal == kSleepOrdinal) {
        return pending.deadlineMs && hostTickMilliseconds() >= pending.deadlineMs;
    }

    if (pending.ordinal != kWaitForSingleObjectOrdinal) return false;

#if defined(_WIN32)
    auto hostWaitProbe = [](const GuestHandle& handle) {
        const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
        if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
        if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
        return CeKernel::HostWaitResult{false, true, GetLastError()};
    };
#else
    CeKernel::HostWaitProbe hostWaitProbe;
#endif
    refreshCompletedHostWaveBuffers();
    const CeKernel::WaitQueryResult wait =
        ceKernel_.queryWaitObject(pending.args.a0, hostWaitProbe, true);
    if (wait.result != CeKernel::kWaitTimeout) return true;
    return pending.args.a1 != kInfiniteTimeout &&
           pending.deadlineMs &&
           hostTickMilliseconds() >= pending.deadlineMs;
}

bool SyntheticDllRuntime::completeReadyPendingBlockingMainContinuation(const char* reason) {
    if (pendingBlockingApis_.empty() || !ceKernel_.mainThreadContext().valid) return false;

    PendingBlockingApi pending = pendingBlockingApis_.back();
    constexpr uint16_t kSleepOrdinal = 0x01f0;
    constexpr uint16_t kWaitForSingleObjectOrdinal = 0x01f1;
    constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
    uint32_t ret = 0;
    uint32_t error = 0;

    if (pending.ordinal == kSleepOrdinal) {
        if (!pending.deadlineMs || hostTickMilliseconds() < pending.deadlineMs) return false;
    } else if (pending.ordinal == kWaitForSingleObjectOrdinal) {
#if defined(_WIN32)
        auto hostWaitProbe = [](const GuestHandle& handle) {
            const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
            if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
            if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
            return CeKernel::HostWaitResult{false, true, GetLastError()};
        };
#else
        CeKernel::HostWaitProbe hostWaitProbe;
#endif
        refreshCompletedHostWaveBuffers();
        const CeKernel::WaitQueryResult wait =
            ceKernel_.queryWaitObject(pending.args.a0, hostWaitProbe, true);
        ret = wait.result;
        error = wait.error;
        if (ret == CeKernel::kWaitObject0) {
            ceKernel_.consumeAutoResetEvent(pending.args.a0);
        }
        const bool finiteExpired =
            pending.deadlineMs && hostTickMilliseconds() >= pending.deadlineMs;
        if (ret == CeKernel::kWaitTimeout &&
            pending.args.a1 != 0 &&
            pending.args.a1 != kInfiniteTimeout &&
            !finiteExpired) {
            return false;
        }
        if (ret == CeKernel::kWaitTimeout && pending.args.a1 == kInfiniteTimeout) {
            return false;
        }
    } else {
        return false;
    }

    pendingBlockingApis_.pop_back();
    updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
    restoreGuestCpuContext(ceKernel_.mainThreadContext());
    ceKernel_.mainThreadContext().valid = false;
    lastError_ = error;
    setReg(UC_MIPS_REG_V0, ret);
    setReg(UC_MIPS_REG_RA, pending.args.ra);
    setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(pending.args.ra));
    setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(pending.args.ra, reason));
    spdlog::info("{} completed parked main wait reason={} wait=0x{:08x} timeout=0x{:08x} return=0x{:08x} ra=0x{:08x} queued={}",
                 pending.name,
                 reason ? reason : "scheduler",
                 pending.args.a0,
                 pending.args.a1,
                 ret,
                 pending.args.ra,
                 ceGwe_.messageCount());
    return true;
}

bool SyntheticDllRuntime::hasSchedulableGweMessageOwner() const {
    const std::optional<uint32_t> owner = ceGwe_.oldestPendingOwner();
    if (!owner) return false;
    const uint32_t activeThread = ceKernel_.activeGuestThread();
    if (*owner == activeThread) return false;
    if (*owner == ceKernel_.mainThreadPseudoHandle()) {
        if (!pendingBlockingApis_.empty()) return false;
        if (!activeThread) return true;
        if (!ceKernel_.mainThreadContext().valid) return false;
        const uint32_t pc = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_PC);
        return pc && isGuestRangeReadable(pc, 4);
    }
    const auto queuedOwner = ceKernel_.threads().find(*owner);
    return queuedOwner != ceKernel_.threads().end() &&
           queuedOwner->second.state == GuestThreadRunState::Runnable &&
           queuedOwner->second.context.valid;
}

bool SyntheticDllRuntime::switchToRunnableGuestThread(const char* reason,
                                                      uint32_t returnAddress,
                                                      uint32_t preferredHandle) {
    refreshSignaledGuestWaits();
    auto logOwnerPriority = [&](uint32_t ownerThread, const char* target) {
        constexpr uint64_t kOwnerPriorityLogIntervalMs = 1000;
        const uint64_t nowMs = hostTickMilliseconds();
        if (!diagnostics_.shouldLog("gwe-owner-priority", nowMs, kOwnerPriorityLogIntervalMs)) return;
        spdlog::info("guest scheduler owner-priority reason={} target={} owner=0x{:08x} ownerQueued={} totalQueued={} preferred=0x{:08x}",
                     reason ? reason : "cooperate",
                     target,
                     ownerThread,
                     ceGwe_.messageCountForOwner(ownerThread),
                     ceGwe_.messageCount(),
                     preferredHandle);
    };
    auto switchTo = [&](uint32_t handle, GuestThreadState& thread) {
        if (ceKernel_.activeGuestThread()) {
            auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
            if (active != ceKernel_.threads().end()) {
                active->second.context = captureGuestCpuContext();
                if (returnAddress) active->second.context.registers[UC_MIPS_REG_PC] = returnAddress;
                if (active->second.state == GuestThreadRunState::Running) {
                    active->second.state = GuestThreadRunState::Runnable;
                }
            }
        } else {
            auto capturedMain = captureGuestCpuContext();
            if (returnAddress) capturedMain.registers[UC_MIPS_REG_PC] = returnAddress;
            const uint32_t capturedPc = capturedMain.registers.count(UC_MIPS_REG_PC)
                ? capturedMain.registers[UC_MIPS_REG_PC]
                : 0;
            const uint32_t capturedRa = capturedMain.registers.count(UC_MIPS_REG_RA)
                ? capturedMain.registers[UC_MIPS_REG_RA]
                : 0;
            const uint32_t capturedSp = capturedMain.registers.count(UC_MIPS_REG_SP)
                ? capturedMain.registers[UC_MIPS_REG_SP]
                : 0;
            const bool capturedPcReadable = capturedPc && isGuestRangeReadable(capturedPc, 4);
            if (capturedPcReadable || !ceKernel_.mainThreadContext().valid) {
                ceKernel_.mainThreadContext() = std::move(capturedMain);
                spdlog::debug("main thread context saved reason={} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                              reason ? reason : "cooperate", capturedPc, capturedRa, capturedSp);
            } else {
                const uint32_t preservedPc = ceKernel_.mainThreadContext().registers.count(UC_MIPS_REG_PC)
                    ? ceKernel_.mainThreadContext().registers[UC_MIPS_REG_PC]
                    : 0;
                spdlog::warn("preserved parked main thread context reason={} currentPc=0x{:08x} currentRa=0x{:08x} "
                             "currentSp=0x{:08x} parkedPc=0x{:08x}",
                             reason ? reason : "cooperate", capturedPc, capturedRa, capturedSp, preservedPc);
            }
        }
        const uint32_t threadPc = thread.context.registers.count(UC_MIPS_REG_PC)
            ? thread.context.registers.at(UC_MIPS_REG_PC)
            : 0;
        const uint32_t threadRa = thread.context.registers.count(UC_MIPS_REG_RA)
            ? thread.context.registers.at(UC_MIPS_REG_RA)
            : 0;
        const uint32_t threadSp = thread.context.registers.count(UC_MIPS_REG_SP)
            ? thread.context.registers.at(UC_MIPS_REG_SP)
            : 0;
        const uint32_t threadV0 = thread.context.registers.count(UC_MIPS_REG_V0)
            ? thread.context.registers.at(UC_MIPS_REG_V0)
            : 0;
        thread.state = GuestThreadRunState::Running;
        ceKernel_.activeGuestThread() = handle;
        ceKernel_.lastScheduledGuestThread() = handle;
        updateCurrentThreadKData(thread.threadId, thread.tlsBase);
        restoreGuestCpuContext(thread.context);
        spdlog::debug("guest thread switch reason={} handle=0x{:08x} start=0x{:08x} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x} v0=0x{:08x}",
                      reason ? reason : "cooperate", handle, thread.startAddress,
                      threadPc, threadRa, threadSp, threadV0);
        return true;
    };

    if (const std::optional<uint32_t> owner = ceGwe_.oldestPendingOwner()) {
        if (*owner == ceKernel_.mainThreadPseudoHandle()) {
            if (pendingBlockingApis_.empty()) {
                const uint32_t activeThread = ceKernel_.activeGuestThread();
                if (activeThread) {
                    const uint32_t mainPc = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_PC);
                    if (mainPc && isGuestRangeReadable(mainPc, 4)) {
                        auto active = ceKernel_.threads().find(activeThread);
                        if (active != ceKernel_.threads().end()) {
                            active->second.context = captureGuestCpuContext();
                            if (returnAddress) active->second.context.registers[UC_MIPS_REG_PC] = returnAddress;
                            if (active->second.state == GuestThreadRunState::Running) {
                                active->second.state = GuestThreadRunState::Runnable;
                            }
                        }
                        ceKernel_.activeGuestThread() = 0;
                        restoreMainThreadContextIfRunnable(reason);
                        logOwnerPriority(*owner, "main");
                        return true;
                    }
                } else {
                    uint32_t currentPc = 0;
                    uc_reg_read(uc_, UC_MIPS_REG_PC, &currentPc);
                    if ((!currentPc || !isGuestRangeReadable(currentPc, 4)) &&
                        restoreMainThreadContextIfRunnable(reason)) {
                        logOwnerPriority(*owner, "main");
                        return true;
                    }
                }
            }
        } else {
            auto queuedOwner = ceKernel_.threads().find(*owner);
            if (queuedOwner != ceKernel_.threads().end() &&
                queuedOwner->second.state == GuestThreadRunState::Runnable &&
                queuedOwner->second.context.valid) {
                logOwnerPriority(*owner, "guest");
                return switchTo(queuedOwner->first, queuedOwner->second);
            }
        }
    }

    if (preferredHandle) {
        auto preferred = ceKernel_.threads().find(preferredHandle);
        if (preferred != ceKernel_.threads().end() &&
            preferred->second.state == GuestThreadRunState::Runnable &&
            preferred->second.context.valid) {
            return switchTo(preferred->first, preferred->second);
        }
    }

    auto first = ceKernel_.lastScheduledGuestThread() ? ceKernel_.threads().upper_bound(ceKernel_.lastScheduledGuestThread())
                                           : ceKernel_.threads().begin();
    for (auto it = first; it != ceKernel_.threads().end(); ++it) {
        if (it->second.state == GuestThreadRunState::Runnable && it->second.context.valid) {
            return switchTo(it->first, it->second);
        }
    }
    for (auto it = ceKernel_.threads().begin(); it != first; ++it) {
        if (it->second.state == GuestThreadRunState::Runnable && it->second.context.valid) {
            return switchTo(it->first, it->second);
        }
    }
    return false;
}

bool SyntheticDllRuntime::yieldActiveGuestThread(const char* reason, uint32_t returnAddress) {
    if (!ceKernel_.activeGuestThread()) return switchToRunnableGuestThread(reason, returnAddress);
    auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
    if (active != ceKernel_.threads().end() && active->second.state == GuestThreadRunState::Running) {
        active->second.context = captureGuestCpuContext();
        if (returnAddress) active->second.context.registers[UC_MIPS_REG_PC] = returnAddress;
        active->second.state = GuestThreadRunState::Runnable;
        spdlog::info("guest thread yield reason={} handle=0x{:08x} pc=0x{:08x}",
                     reason ? reason : "cooperate", ceKernel_.activeGuestThread(),
                     active->second.context.registers.count(UC_MIPS_REG_PC)
                         ? active->second.context.registers.at(UC_MIPS_REG_PC)
                         : 0);
    }
    return switchToRunnableGuestThread(reason);
}

bool SyntheticDllRuntime::finishActiveGuestThread(uint32_t exitCode) {
    if (!ceKernel_.activeGuestThread()) return false;
    auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
    if (active != ceKernel_.threads().end()) {
        active->second.exitCode = exitCode;
        active->second.state = GuestThreadRunState::Terminated;
        active->second.context = captureGuestCpuContext();
        spdlog::info("guest thread exit handle=0x{:08x} exitCode=0x{:08x}", ceKernel_.activeGuestThread(), exitCode);
    }
    if (ceKernel_.lastScheduledGuestThread() == ceKernel_.activeGuestThread()) ceKernel_.lastScheduledGuestThread() = 0;
    ceKernel_.activeGuestThread() = 0;
    if (pendingBlockingApis_.empty() &&
        restoreMainThreadContextIfRunnable("thread exit")) {
        return true;
    }
    return switchToRunnableGuestThread("thread exit");
}

bool SyntheticDllRuntime::cooperateGuestThreadsAfterCall(const std::string& name, uint32_t returnAddress) {
    if (!returnAddress) returnAddress = reg(UC_MIPS_REG_RA);
    const bool yieldingCall = name == "Sleep" || name == "WaitForSingleObject" ||
                              name == "WaitForMultipleObjects";
    const bool queuedUiWork = name == "PostMessageW" || name == "InvalidateRect" ||
                              name == "SetTimer" || name == "ShowWindow";
    if (ceKernel_.activeGuestThread() &&
        (yieldingCall || (queuedUiWork && ceGwe_.hasMessages() && hasSchedulableGweMessageOwner()))) {
        return yieldActiveGuestThread(name.c_str(), returnAddress);
    }
    const bool processStarterCall = name == "CreateProcessW";
    if (ceKernel_.activeGuestThread() && processStarterCall && hasRunnableGuestThread()) {
        return yieldActiveGuestThread(name.c_str(), returnAddress);
    }
    if (!ceKernel_.activeGuestThread() && name == "Sleep" && hasRunnableGuestThread()) {
        setReg(UC_MIPS_REG_V0, 0);
        return switchToRunnableGuestThread(name.c_str(), returnAddress);
    }
    const bool threadStarterCall = name == "CreateThread" || name == "ResumeThread" ||
                                   processStarterCall;
    if (!ceKernel_.activeGuestThread() && threadStarterCall && hasRunnableGuestThread()) {
        return switchToRunnableGuestThread(name.c_str(), returnAddress);
    }
    return false;
}

bool SyntheticDllRuntime::readGuestWaitHandles(uint32_t count,
                                               uint32_t handlesPtr,
                                               std::vector<uint32_t>& handles) {
    handles.clear();
    if (!count || !handlesPtr || count > 64) {
        lastError_ = 87;
        return false;
    }
    handles.resize(count);
    if (uc_mem_read(uc_, handlesPtr, handles.data(), handles.size() * sizeof(uint32_t)) != UC_ERR_OK) {
        handles.clear();
        lastError_ = 87;
        return false;
    }
    return true;
}

uint32_t SyntheticDllRuntime::waitForMultipleGuestObjects(uint32_t count,
                                                          uint32_t handlesPtr,
                                                          bool waitAll) {
    std::vector<uint32_t> handles;
    if (!readGuestWaitHandles(count, handlesPtr, handles)) return CeKernel::kWaitFailed;

#if defined(_WIN32)
    auto hostWaitProbe = [](const GuestHandle& handle) {
        const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
        if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
        if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
        return CeKernel::HostWaitResult{false, true, GetLastError()};
    };
#else
    CeKernel::HostWaitProbe hostWaitProbe;
#endif

    const CeKernel::WaitQueryResult wait =
        ceKernel_.queryWaitObjects(handles, waitAll, hostWaitProbe, false);
    lastError_ = wait.error;
    return wait.result;
}
