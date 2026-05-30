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
    for (auto it = pendingSerialReadsByThread_.begin(); it != pendingSerialReadsByThread_.end();) {
        if (it->second.serialHandle == handle) it = pendingSerialReadsByThread_.erase(it);
        else ++it;
    }
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

CeDevice::NoDataReadDecision CeDevice::decideNoDataRead(uint32_t handle,
                                                        uint32_t requested,
                                                        uint64_t nowMs) const {
    constexpr uint32_t kMaxDwordTimeout = 0xffffffffu;
    const SerialState* state = serialState(handle);
    if (!state || !requested) return {};

    const CommTimeouts& timeouts = state->timeouts;
    const bool immediateReturn =
        timeouts.readIntervalTimeout == kMaxDwordTimeout &&
        timeouts.readTotalTimeoutMultiplier == 0 &&
        timeouts.readTotalTimeoutConstant == 0;
    if (immediateReturn) return {};

    const uint64_t totalTimeoutMs =
        uint64_t(timeouts.readTotalTimeoutMultiplier) * uint64_t(requested) +
        uint64_t(timeouts.readTotalTimeoutConstant);
    if (!totalTimeoutMs) {
        return {NoDataReadAction::WaitIndefinitely, 0};
    }
    return {NoDataReadAction::WaitUntilDeadline, nowMs + totalTimeoutMs};
}

void CeDevice::beginPendingSerialRead(PendingSerialRead read) {
    if (!read.threadHandle || !read.serialHandle) return;
    if (SerialState* state = serialState(read.serialHandle)) {
        state->emptyReadWaitUntilMs = read.deadlineMs;
    }
    pendingSerialReadsByThread_[read.threadHandle] = read;
}

std::optional<CeDevice::PendingSerialRead> CeDevice::pendingSerialRead(uint32_t threadHandle) const {
    auto it = pendingSerialReadsByThread_.find(threadHandle);
    if (it == pendingSerialReadsByThread_.end()) return std::nullopt;
    return it->second;
}

std::vector<CeDevice::PendingSerialRead> CeDevice::pendingSerialReads() const {
    std::vector<PendingSerialRead> reads;
    reads.reserve(pendingSerialReadsByThread_.size());
    for (const auto& [threadHandle, read] : pendingSerialReadsByThread_) {
        (void)threadHandle;
        reads.push_back(read);
    }
    return reads;
}

bool CeDevice::completePendingSerialRead(uint32_t threadHandle) {
    auto it = pendingSerialReadsByThread_.find(threadHandle);
    if (it == pendingSerialReadsByThread_.end()) return false;
    if (SerialState* state = serialState(it->second.serialHandle)) {
        state->emptyReadWaitUntilMs = 0;
    }
    pendingSerialReadsByThread_.erase(it);
    return true;
}
