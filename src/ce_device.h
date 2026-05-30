#pragma once

#include <cstdint>
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

private:
    std::map<uint32_t, SerialState> serialStates_;
    std::map<uint32_t, PendingSerialRead> pendingSerialReadsByThread_;
};
