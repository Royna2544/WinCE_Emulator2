#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
    for (auto& [threadHandle, thread] : ceKernel_.threads()) {
        if (thread.state != GuestThreadRunState::WaitingForMessage) continue;
        thread.state = GuestThreadRunState::Runnable;
        spdlog::info("guest thread message wait satisfied handle=0x{:08x}", threadHandle);
    }
}

void SyntheticDllRuntime::refreshSignaledGuestWaits() {
    refreshCompletedHostWaveBuffers();
#if defined(_WIN32)
    constexpr DWORD kWaitObject0 = WAIT_OBJECT_0;
    constexpr DWORD kWaitTimeout = WAIT_TIMEOUT;
    const uint64_t now = hostTickMilliseconds();
    for (auto& [threadHandle, thread] : ceKernel_.threads()) {
        if (thread.state != GuestThreadRunState::Waiting) continue;
        if (thread.sleepUntilMs) {
            if (now < thread.sleepUntilMs) continue;
            thread.sleepUntilMs = 0;
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[UC_MIPS_REG_V0] = 0;
            lastError_ = 0;
            spdlog::debug("guest thread sleep satisfied handle=0x{:08x}", threadHandle);
            continue;
        }
        std::vector<uint32_t> handles = thread.waitHandles;
        if (handles.empty() && thread.waitHandle) handles.push_back(thread.waitHandle);
        if (handles.empty()) continue;

        bool allReady = true;
        for (size_t i = 0; i < handles.size(); ++i) {
            auto* handle = lookupGuestHandle(handles[i]);
            if (!handle) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = 0xffffffffu;
                lastError_ = 6;
                spdlog::warn("guest thread wait invalid handle=0x{:08x} waitHandle=0x{:08x}",
                             threadHandle, handles[i]);
                break;
            }

            bool ready = false;
            DWORD wait = kWaitObject0;
            if (handle->hostValue &&
                (handle->kind == GuestHandle::Kind::HostEvent ||
                 handle->kind == GuestHandle::Kind::HostMutex ||
                 handle->kind == GuestHandle::Kind::GuestProcess ||
                 handle->kind == GuestHandle::Kind::GuestThread)) {
                wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0);
                ready = wait == kWaitObject0;
            } else if (handle->kind == GuestHandle::Kind::GuestThread) {
                auto waitedThread = ceKernel_.threads().find(handles[i]);
                ready = waitedThread == ceKernel_.threads().end() ||
                        waitedThread->second.state == GuestThreadRunState::Terminated;
            } else {
                ready = true;
            }

            if (!ready && wait != kWaitTimeout) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = 0xffffffffu;
                lastError_ = GetLastError();
                spdlog::warn("guest thread wait failed handle=0x{:08x} waitHandle=0x{:08x} error={}",
                             threadHandle, handles[i], lastError_);
                break;
            }

            allReady = allReady && ready;
            if (!thread.waitAll && ready) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = uint32_t(i);
                lastError_ = 0;
                spdlog::info("guest thread wait satisfied handle=0x{:08x} waitHandle=0x{:08x} index={}",
                             threadHandle, handles[i], i);
                break;
            }
        }
        if (thread.state == GuestThreadRunState::Waiting && thread.waitAll && allReady) {
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[UC_MIPS_REG_V0] = 0;
            lastError_ = 0;
            spdlog::info("guest thread wait-all satisfied handle=0x{:08x} count={}",
                         threadHandle, handles.size());
        }
    }
#endif
}

bool SyntheticDllRuntime::hasRunnableGuestThread() {
    refreshSignaledGuestWaits();
    for (const auto& [handle, thread] : ceKernel_.threads()) {
        (void)handle;
        if (thread.state == GuestThreadRunState::Runnable) return true;
    }
    return false;
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

bool SyntheticDllRuntime::switchToRunnableGuestThread(const char* reason,
                                                      uint32_t returnAddress,
                                                      uint32_t preferredHandle) {
    refreshSignaledGuestWaits();
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
    ceKernel_.activeGuestThread() = 0;
    if (restoreMainThreadContextIfRunnable(reason)) return true;
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
    if (restoreMainThreadContextIfRunnable("thread exit")) return true;
    return switchToRunnableGuestThread("thread exit");
}

bool SyntheticDllRuntime::cooperateGuestThreadsAfterCall(const std::string& name, uint32_t returnAddress) {
    if (!returnAddress) returnAddress = reg(UC_MIPS_REG_RA);
    const bool yieldingCall = name == "Sleep" || name == "WaitForSingleObject" ||
                              name == "WaitForMultipleObjects";
    const bool queuedUiWork = name == "PostMessageW" || name == "InvalidateRect" ||
                              name == "SetTimer" || name == "ShowWindow";
    if (ceKernel_.activeGuestThread() && (yieldingCall || (queuedUiWork && !guestMessages_.empty()))) {
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
    constexpr uint32_t kWaitObject0 = 0x00000000u;
    constexpr uint32_t kWaitTimeout = 0x00000102u;
    constexpr uint32_t kWaitFailed = 0xffffffffu;

    std::vector<uint32_t> handles;
    if (!readGuestWaitHandles(count, handlesPtr, handles)) return kWaitFailed;

    bool allReady = true;
    for (uint32_t i = 0; i < count; ++i) {
        auto* handle = lookupGuestHandle(handles[i]);
        if (!handle) {
            lastError_ = 6;
            return kWaitFailed;
        }
        bool ready = false;
#if defined(_WIN32)
        if (handle->hostValue &&
            (handle->kind == GuestHandle::Kind::HostEvent ||
             handle->kind == GuestHandle::Kind::HostMutex ||
             handle->kind == GuestHandle::Kind::GuestProcess ||
             handle->kind == GuestHandle::Kind::GuestThread)) {
            ready = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0) == kWaitObject0;
        } else
#endif
        if (handle->kind == GuestHandle::Kind::GuestThread) {
            auto thread = ceKernel_.threads().find(handles[i]);
            ready = thread == ceKernel_.threads().end() ||
                    thread->second.state == GuestThreadRunState::Terminated;
        }
        else {
            ready = true;
        }
        allReady = allReady && ready;
        if (!waitAll && ready) {
            lastError_ = 0;
            return kWaitObject0 + i;
        }
    }

    lastError_ = 0;
    return waitAll && allReady ? kWaitObject0 : kWaitTimeout;
}
