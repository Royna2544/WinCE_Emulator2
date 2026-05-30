#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

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

    static constexpr std::string_view name() noexcept { return "CE device manager"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for virtual CE device and serial state.";
    }

    void registerSerial(SerialState state) {
        if (!state.handle) return;
        state.open = true;
        serialStates_[state.handle] = std::move(state);
    }

    bool unregisterSerial(uint32_t handle) {
        return serialStates_.erase(handle) != 0;
    }

    SerialState* serialState(uint32_t handle) {
        auto it = serialStates_.find(handle);
        return it == serialStates_.end() ? nullptr : &it->second;
    }

    const SerialState* serialState(uint32_t handle) const {
        auto it = serialStates_.find(handle);
        return it == serialStates_.end() ? nullptr : &it->second;
    }

    bool hasSerial(uint32_t handle) const {
        return serialStates_.find(handle) != serialStates_.end();
    }

private:
    std::map<uint32_t, SerialState> serialStates_;
};
