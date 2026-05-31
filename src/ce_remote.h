#pragma once

#include <cstdint>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

class CeRemote {
public:
    struct TouchEvent {
        uint32_t message{};
        int32_t x{};
        int32_t y{};
    };

    struct KeyEvent {
        uint32_t message{};
        uint32_t vk{};
    };

    struct AudioChunk {
        std::vector<uint8_t> payload;
        uint64_t sequence{};
        uint64_t ptsMs{};
        uint32_t durationMs{};
    };

    static constexpr std::string_view name() noexcept { return "CE remote endpoint"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for remote input, serial injection, audio tap, and API status state.";
    }

    std::mutex& mutex() const noexcept { return mutex_; }
    std::condition_variable& audioCv() const noexcept { return audioCv_; }

    std::deque<TouchEvent>& touchEvents() noexcept { return touchEvents_; }
    std::deque<KeyEvent>& keyEvents() noexcept { return keyEvents_; }
    std::deque<uint8_t>& serialBytes() noexcept { return serialBytes_; }
    std::deque<AudioChunk>& audioChunks() noexcept { return audioChunks_; }

    const std::deque<uint8_t>& serialBytes() const noexcept { return serialBytes_; }
    const std::deque<AudioChunk>& audioChunks() const noexcept { return audioChunks_; }

    size_t& audioClientCount() noexcept { return audioClientCount_; }
    size_t audioClientCount() const noexcept { return audioClientCount_; }
    uint64_t& audioSequence() noexcept { return audioSequence_; }
    uint64_t audioSequence() const noexcept { return audioSequence_; }
    uint64_t& audioNextPtsMs() noexcept { return audioNextPtsMs_; }
    uint64_t audioNextPtsMs() const noexcept { return audioNextPtsMs_; }

    nlohmann::json& imuState() noexcept { return imuState_; }
    const nlohmann::json& imuState() const noexcept { return imuState_; }

    bool& paused() noexcept { return paused_; }
    bool paused() const noexcept { return paused_; }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable audioCv_;
    std::deque<TouchEvent> touchEvents_;
    std::deque<KeyEvent> keyEvents_;
    std::deque<uint8_t> serialBytes_;
    std::deque<AudioChunk> audioChunks_;
    size_t audioClientCount_{};
    uint64_t audioSequence_{};
    uint64_t audioNextPtsMs_{};
    nlohmann::json imuState_;
    bool paused_{};
};
