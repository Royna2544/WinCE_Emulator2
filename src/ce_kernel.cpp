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
