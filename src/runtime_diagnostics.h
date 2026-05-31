#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

class RuntimeDiagnostics {
public:
    void setDumpsEnabled(bool enabled) noexcept;
    bool dumpsEnabled() const noexcept;

    bool shouldLog(std::string_view key, uint64_t nowMs, uint64_t intervalMs);

    void incrementMessageTransferWatchdogStops() noexcept;
    uint64_t messageTransferWatchdogStops() const noexcept;
    void resetMessageTransferWatchdogStops() noexcept;

private:
    bool dumpsEnabled_{};
    std::unordered_map<std::string, uint64_t> lastLogByKey_;
    uint64_t messageTransferWatchdogStops_{};
};
