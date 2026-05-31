#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CeDevice {
public:
    struct SerialMode {
        uint32_t baud{9600};
        uint8_t byteSize{8};
        uint8_t parity{};
        uint8_t stopBits{};
    };

    struct CommTimeouts {
        uint32_t readIntervalTimeout{};
        uint32_t readTotalTimeoutMultiplier{};
        uint32_t readTotalTimeoutConstant{};
        uint32_t writeTotalTimeoutMultiplier{};
        uint32_t writeTotalTimeoutConstant{};
    };

    struct SerialState {
        uint32_t handle{};
        std::string guestName;
        std::string deviceType;
        std::string hostName;
        std::string backend;
        SerialMode mode;
        CommTimeouts timeouts;
        uint32_t commMask{};
        uint32_t inQueueSize{};
        uint32_t outQueueSize{};
        uint32_t lastError{};
        uint64_t emptyReadWaitUntilMs{};
        bool virtualNoDataBackend{};
        bool open{};
    };

    struct SerialDeviceConfig {
        std::string guest;
        std::string type;
        std::string backend;
        std::string host;
        bool enabled{};
        uint32_t baud{9600};
        std::string mode{"8N1"};
        std::string note;
    };

    enum class NoDataReadAction {
        CompleteZero,
        WaitUntilDeadline,
        WaitIndefinitely,
    };

    struct NoDataReadDecision {
        NoDataReadAction action{NoDataReadAction::CompleteZero};
        uint64_t deadlineMs{};
    };

    struct PendingSerialRead {
        uint32_t threadHandle{};
        uint32_t serialHandle{};
        uint32_t buffer{};
        uint32_t requested{};
        uint32_t transferredPtr{};
        uint64_t deadlineMs{};
    };

    static constexpr std::string_view name() noexcept { return "CE device manager"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for virtual CE device and serial state.";
    }

    void registerSerial(SerialState state);
    bool unregisterSerial(uint32_t handle);
    SerialState* serialState(uint32_t handle);
    const SerialState* serialState(uint32_t handle) const;
    bool hasSerial(uint32_t handle) const;
    bool setSerialMode(uint32_t handle, SerialMode mode, uint32_t lastError = 0);
    bool setSerialTimeouts(uint32_t handle, CommTimeouts timeouts, uint32_t lastError = 0);
    bool setSerialMask(uint32_t handle, uint32_t mask, uint32_t lastError = 0);
    bool setSerialQueueSizes(uint32_t handle, uint32_t inQueueSize, uint32_t outQueueSize, uint32_t lastError = 0);
    bool markSerialPurged(uint32_t handle, uint32_t flags, uint32_t lastError = 0);
    NoDataReadDecision decideNoDataRead(uint32_t handle, uint32_t requested, uint64_t nowMs) const;
    void beginPendingSerialRead(PendingSerialRead read);
    std::optional<PendingSerialRead> pendingSerialRead(uint32_t threadHandle) const;
    std::vector<PendingSerialRead> pendingSerialReads() const;
    bool completePendingSerialRead(uint32_t threadHandle);
    std::filesystem::path& serialDeviceMapPath() noexcept { return serialDeviceMapPath_; }
    const std::filesystem::path& serialDeviceMapPath() const noexcept { return serialDeviceMapPath_; }
    std::map<std::string, SerialDeviceConfig>& serialDeviceConfigs() noexcept { return serialDevicesByGuest_; }
    const std::map<std::string, SerialDeviceConfig>& serialDeviceConfigs() const noexcept {
        return serialDevicesByGuest_;
    }
    uint32_t& defaultSerialBaud() noexcept { return defaultSerialBaud_; }
    uint32_t defaultSerialBaud() const noexcept { return defaultSerialBaud_; }
    std::string& defaultSerialMode() noexcept { return defaultSerialMode_; }
    const std::string& defaultSerialMode() const noexcept { return defaultSerialMode_; }

private:
    std::map<uint32_t, SerialState> serialStates_;
    std::map<uint32_t, PendingSerialRead> pendingSerialReadsByThread_;
    std::filesystem::path serialDeviceMapPath_;
    std::map<std::string, SerialDeviceConfig> serialDevicesByGuest_;
    uint32_t defaultSerialBaud_{9600};
    std::string defaultSerialMode_{"8N1"};
};
