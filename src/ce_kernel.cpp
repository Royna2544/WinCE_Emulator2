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
