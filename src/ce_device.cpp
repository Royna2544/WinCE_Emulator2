#include "ce_device.h"

#include <utility>

static_assert(!CeDevice::name().empty());
static_assert(!CeDevice::role().empty());

void CeDevice::registerSerial(SerialState state) {
    if (!state.handle) return;
    state.open = true;
    serialStates_[state.handle] = std::move(state);
}

bool CeDevice::unregisterSerial(uint32_t handle) {
    return serialStates_.erase(handle) != 0;
}

CeDevice::SerialState* CeDevice::serialState(uint32_t handle) {
    auto it = serialStates_.find(handle);
    return it == serialStates_.end() ? nullptr : &it->second;
}

const CeDevice::SerialState* CeDevice::serialState(uint32_t handle) const {
    auto it = serialStates_.find(handle);
    return it == serialStates_.end() ? nullptr : &it->second;
}

bool CeDevice::hasSerial(uint32_t handle) const {
    return serialStates_.find(handle) != serialStates_.end();
}

bool CeDevice::setSerialMode(uint32_t handle, SerialMode mode, uint32_t lastError) {
    SerialState* state = serialState(handle);
    if (!state) return false;
    state->mode = mode;
    state->lastError = lastError;
    return true;
}

bool CeDevice::setSerialTimeouts(uint32_t handle, CommTimeouts timeouts, uint32_t lastError) {
    SerialState* state = serialState(handle);
    if (!state) return false;
    state->timeouts = timeouts;
    state->lastError = lastError;
    return true;
}

bool CeDevice::setSerialMask(uint32_t handle, uint32_t mask, uint32_t lastError) {
    SerialState* state = serialState(handle);
    if (!state) return false;
    state->commMask = mask;
    state->lastError = lastError;
    return true;
}

bool CeDevice::setSerialQueueSizes(uint32_t handle,
                                   uint32_t inQueueSize,
                                   uint32_t outQueueSize,
                                   uint32_t lastError) {
    SerialState* state = serialState(handle);
    if (!state) return false;
    state->inQueueSize = inQueueSize;
    state->outQueueSize = outQueueSize;
    state->lastError = lastError;
    return true;
}

bool CeDevice::markSerialPurged(uint32_t handle, uint32_t flags, uint32_t lastError) {
    (void)flags;
    SerialState* state = serialState(handle);
    if (!state) return false;
    state->emptyReadWaitUntilMs = 0;
    state->lastError = lastError;
    return true;
}
