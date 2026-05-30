#include "ce_kernel.h"

static_assert(!CeKernel::name().empty());
static_assert(!CeKernel::role().empty());

uint32_t CeKernel::makeHandle(GuestHandle handle) {
    const uint32_t guest = nextHandle_++;
    guestHandles_[guest] = handle;
    return guest;
}

CeKernel::GuestHandle* CeKernel::lookupHandle(uint32_t guestHandle) {
    auto it = guestHandles_.find(guestHandle);
    return it == guestHandles_.end() ? nullptr : &it->second;
}

const CeKernel::GuestHandle* CeKernel::lookupHandle(uint32_t guestHandle) const {
    auto it = guestHandles_.find(guestHandle);
    return it == guestHandles_.end() ? nullptr : &it->second;
}

bool CeKernel::eraseHandle(uint32_t guestHandle) {
    return guestHandles_.erase(guestHandle) != 0;
}

bool CeKernel::containsHandle(uint32_t guestHandle) const {
    return guestHandles_.find(guestHandle) != guestHandles_.end();
}

bool CeKernel::hasRunnableThread() const {
    for (const auto& [handle, thread] : guestThreads_) {
        (void)handle;
        if (thread.state == GuestThreadRunState::Runnable) return true;
    }
    return false;
}

std::vector<uint32_t> CeKernel::wakeThreadsWaitingForMessage(
    const MessageWaitProbe& hasMessagesForThread) {
    std::vector<uint32_t> awakened;
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::WaitingForMessage) continue;
        if (hasMessagesForThread && !hasMessagesForThread(threadHandle)) continue;
        thread.state = GuestThreadRunState::Runnable;
        awakened.push_back(threadHandle);
    }
    return awakened;
}

CeKernel::WaitQueryResult CeKernel::queryWaitObject(uint32_t guestHandle,
                                                    const HostWaitProbe& hostWaitProbe,
                                                    bool failOnHostError) const {
    const auto* handle = lookupHandle(guestHandle);
    if (!handle) return {kWaitFailed, 6, 0};

    uint32_t preferredThread = 0;
    if (handle->kind == GuestHandle::Kind::GuestThread) preferredThread = guestHandle;

    if (handle->hostValue &&
        (handle->kind == GuestHandle::Kind::HostEvent ||
         handle->kind == GuestHandle::Kind::HostMutex ||
         handle->kind == GuestHandle::Kind::GuestProcess ||
         handle->kind == GuestHandle::Kind::GuestThread)) {
        const HostWaitResult wait = hostWaitProbe ? hostWaitProbe(*handle) : HostWaitResult{};
        if (wait.ready) return {kWaitObject0, 0, preferredThread};
        if (wait.failed && failOnHostError) return {kWaitFailed, wait.error, preferredThread};
        return {kWaitTimeout, 0, preferredThread};
    }

    if (handle->kind == GuestHandle::Kind::GuestThread) {
        const auto thread = guestThreads_.find(guestHandle);
        const bool terminated = thread == guestThreads_.end() ||
                                thread->second.state == GuestThreadRunState::Terminated;
        return {terminated ? kWaitObject0 : kWaitTimeout, 0, preferredThread};
    }

    if (handle->kind == GuestHandle::Kind::GuestProcess && !handle->hostValue) {
        bool processStillRunning = false;
        for (const auto& [threadHandle, thread] : guestThreads_) {
            (void)threadHandle;
            if (thread.processHandle == guestHandle &&
                thread.state != GuestThreadRunState::Terminated) {
                processStillRunning = true;
                break;
            }
        }
        return {processStillRunning ? kWaitTimeout : kWaitObject0, 0, 0};
    }

    return {kWaitObject0, 0, 0};
}

CeKernel::WaitQueryResult CeKernel::queryWaitObjects(const std::vector<uint32_t>& guestHandles,
                                                     bool waitAll,
                                                     const HostWaitProbe& hostWaitProbe,
                                                     bool failOnHostError) const {
    bool allReady = true;
    uint32_t preferredThread = 0;
    for (size_t i = 0; i < guestHandles.size(); ++i) {
        WaitQueryResult single = queryWaitObject(guestHandles[i], hostWaitProbe, failOnHostError);
        if (!preferredThread && single.preferredThread) preferredThread = single.preferredThread;
        if (single.result == kWaitFailed) return single;

        const bool ready = single.result == kWaitObject0;
        allReady = allReady && ready;
        if (!waitAll && ready) return {kWaitObject0 + uint32_t(i), 0, preferredThread};
    }

    return {waitAll && allReady ? kWaitObject0 : kWaitTimeout, 0, preferredThread};
}

std::vector<CeKernel::WaitRefreshEvent> CeKernel::refreshSignaledWaits(
    uint64_t nowMs,
    int resultRegister,
    const HostWaitProbe& hostWaitProbe,
    const MessageWaitProbe& hasMessagesForThread) {
    std::vector<WaitRefreshEvent> events;
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::Waiting) continue;
        if (thread.sleepUntilMs) {
            if (nowMs < thread.sleepUntilMs) continue;
            thread.sleepUntilMs = 0;
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.waitForMessages = false;
            thread.waitWakeMask = 0;
            thread.context.registers[resultRegister] = 0;
            events.push_back({WaitRefreshKind::SleepSatisfied, threadHandle});
            continue;
        }

        std::vector<uint32_t> handles = thread.waitHandles;
        if (handles.empty() && thread.waitHandle) handles.push_back(thread.waitHandle);
        if (thread.waitForMessages && hasMessagesForThread && hasMessagesForThread(threadHandle)) {
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.waitForMessages = false;
            thread.waitWakeMask = 0;
            thread.context.registers[resultRegister] = kWaitObject0 + uint32_t(handles.size());
            events.push_back({WaitRefreshKind::MessageWaitSatisfied, threadHandle, 0, 0, 0, handles.size()});
            continue;
        }
        if (handles.empty()) continue;

        bool allReady = true;
        for (size_t i = 0; i < handles.size(); ++i) {
            const uint32_t waitHandle = handles[i];
            const auto* handle = lookupHandle(waitHandle);
            if (!handle) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.waitForMessages = false;
                thread.waitWakeMask = 0;
                thread.context.registers[resultRegister] = 0xffffffffu;
                events.push_back({WaitRefreshKind::InvalidHandle, threadHandle, waitHandle, 6});
                break;
            }

            bool ready = false;
            bool failed = false;
            uint32_t error = 0;
            if (handle->hostValue &&
                (handle->kind == GuestHandle::Kind::HostEvent ||
                 handle->kind == GuestHandle::Kind::HostMutex ||
                 handle->kind == GuestHandle::Kind::GuestProcess ||
                 handle->kind == GuestHandle::Kind::GuestThread)) {
                const HostWaitResult wait = hostWaitProbe ? hostWaitProbe(*handle) : HostWaitResult{};
                ready = wait.ready;
                failed = wait.failed;
                error = wait.error;
            } else if (handle->kind == GuestHandle::Kind::GuestThread) {
                const auto waitedThread = guestThreads_.find(waitHandle);
                ready = waitedThread == guestThreads_.end() ||
                        waitedThread->second.state == GuestThreadRunState::Terminated;
            } else {
                ready = true;
            }

            if (failed) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.waitForMessages = false;
                thread.waitWakeMask = 0;
                thread.context.registers[resultRegister] = 0xffffffffu;
                events.push_back({WaitRefreshKind::WaitFailed, threadHandle, waitHandle, error});
                break;
            }

            allReady = allReady && ready;
            if (!thread.waitAll && ready) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.waitForMessages = false;
                thread.waitWakeMask = 0;
                thread.context.registers[resultRegister] = uint32_t(i);
                events.push_back({WaitRefreshKind::WaitSatisfied, threadHandle, waitHandle, 0, i});
                break;
            }
        }
        if (thread.state == GuestThreadRunState::Waiting && thread.waitAll && allReady) {
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.waitForMessages = false;
            thread.waitWakeMask = 0;
            thread.context.registers[resultRegister] = 0;
            events.push_back({WaitRefreshKind::WaitAllSatisfied, threadHandle, 0, 0, 0, handles.size()});
        }
    }
    return events;
}

std::optional<CeKernel::EncodedKernelCall> CeKernel::decodeMipsKernelCall(uint32_t target) {
    // CE nkmips.h defines this old MIPS encoding for directly-linked
    // TerminateProcess calls from CRT startup/security code.
    constexpr uint32_t kOldFirstMethod = 0xfffffc02u;
    constexpr uint32_t kApiCallScale = 4;
    constexpr uint32_t kApiSetShift = 8;
    constexpr uint32_t kCurrentProcessApiSet = 2;
    constexpr uint32_t kProcTerminateMethod = 2;

    if (target > kOldFirstMethod) return std::nullopt;
    const uint32_t delta = kOldFirstMethod - target;
    if (delta % kApiCallScale != 0) return std::nullopt;

    const uint32_t encoded = delta / kApiCallScale;
    const uint32_t apiSet = encoded >> kApiSetShift;
    const uint32_t method = encoded & ((1u << kApiSetShift) - 1u);
    EncodedKernelCall call{};
    call.target = target;
    call.apiSet = apiSet;
    call.method = method;
    call.oldEncoding = true;
    if (apiSet == kCurrentProcessApiSet && method == kProcTerminateMethod) {
        call.kind = EncodedKernelCallKind::TerminateProcess;
    }
    return call.kind == EncodedKernelCallKind::Unknown ? std::nullopt
                                                       : std::optional<EncodedKernelCall>{call};
}

void CeKernel::terminateCurrentProcess(uint32_t exitCode) {
    processTerminated_ = true;
    processExitCode_ = exitCode;
    for (auto& [threadHandle, thread] : guestThreads_) {
        (void)threadHandle;
        if (thread.processHandle != mainProcessPseudoHandle_) continue;
        thread.exitCode = exitCode;
        thread.state = GuestThreadRunState::Terminated;
    }
    activeGuestThread_ = 0;
    lastScheduledGuestThread_ = 0;
}
