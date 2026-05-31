#include "runtime_diagnostics.h"

void RuntimeDiagnostics::setDumpsEnabled(bool enabled) noexcept {
    dumpsEnabled_ = enabled;
}

bool RuntimeDiagnostics::dumpsEnabled() const noexcept {
    return dumpsEnabled_;
}

bool RuntimeDiagnostics::shouldLog(std::string_view key, uint64_t nowMs, uint64_t intervalMs) {
    uint64_t& lastLog = lastLogByKey_[std::string(key)];
    if (lastLog && nowMs - lastLog < intervalMs) return false;
    lastLog = nowMs;
    return true;
}

void RuntimeDiagnostics::incrementMessageTransferWatchdogStops() noexcept {
    ++messageTransferWatchdogStops_;
}

uint64_t RuntimeDiagnostics::messageTransferWatchdogStops() const noexcept {
    return messageTransferWatchdogStops_;
}

void RuntimeDiagnostics::resetMessageTransferWatchdogStops() noexcept {
    messageTransferWatchdogStops_ = 0;
}
