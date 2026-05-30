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

std::vector<uint32_t> CeKernel::wakeThreadsWaitingForMessage() {
    std::vector<uint32_t> awakened;
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::WaitingForMessage) continue;
        thread.state = GuestThreadRunState::Runnable;
        awakened.push_back(threadHandle);
    }
    return awakened;
}

std::vector<CeKernel::WaitRefreshEvent> CeKernel::refreshSignaledWaits(
    uint64_t nowMs,
    int resultRegister,
    const HostWaitProbe& hostWaitProbe) {
    std::vector<WaitRefreshEvent> events;
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::Waiting) continue;
        if (thread.sleepUntilMs) {
            if (nowMs < thread.sleepUntilMs) continue;
            thread.sleepUntilMs = 0;
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[resultRegister] = 0;
            events.push_back({WaitRefreshKind::SleepSatisfied, threadHandle});
            continue;
        }

        std::vector<uint32_t> handles = thread.waitHandles;
        if (handles.empty() && thread.waitHandle) handles.push_back(thread.waitHandle);
        if (handles.empty()) continue;

        bool allReady = true;
        for (size_t i = 0; i < handles.size(); ++i) {
            const uint32_t waitHandle = handles[i];
            const auto* handle = lookupHandle(waitHandle);
            if (!handle) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
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
                thread.context.registers[resultRegister] = 0xffffffffu;
                events.push_back({WaitRefreshKind::WaitFailed, threadHandle, waitHandle, error});
                break;
            }

            allReady = allReady && ready;
            if (!thread.waitAll && ready) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[resultRegister] = uint32_t(i);
                events.push_back({WaitRefreshKind::WaitSatisfied, threadHandle, waitHandle, 0, i});
                break;
            }
        }
        if (thread.state == GuestThreadRunState::Waiting && thread.waitAll && allReady) {
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[resultRegister] = 0;
            events.push_back({WaitRefreshKind::WaitAllSatisfied, threadHandle, 0, 0, 0, handles.size()});
        }
    }
    return events;
}
