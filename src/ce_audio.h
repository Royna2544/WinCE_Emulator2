#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <memory>
#include <string_view>
#include <vector>

class CeAudio {
public:
    struct Format {
        uint16_t formatTag{};
        uint16_t channels{};
        uint32_t samplesPerSec{};
        uint32_t avgBytesPerSec{};
        uint16_t blockAlign{};
        uint16_t bitsPerSample{};
    };

    struct StreamConfig {
        uint32_t handle{};
        uint32_t callback{};
        uint32_t instance{};
        uint32_t flags{};
        Format format;
    };

    struct QueueResult {
        uint32_t guestHeader{};
        uint32_t completionEvent{};
        uint64_t startMs{};
        uint64_t endMs{};
        uint32_t durationMs{};
    };

    struct Completion {
        uint32_t handle{};
        uint32_t guestHeader{};
        uint32_t completionEvent{};
    };

    struct LiveSlice {
        std::vector<uint8_t> pcm;
        Format format;
        uint64_t startMs{};
        uint32_t durationMs{};
    };

    struct HostWaveBuffer {
        std::vector<uint8_t> data;
        std::array<uint8_t, 64> header{};
        std::shared_ptr<void> completionContext;
    };

    struct HostAudioBackendChunk {
        uint32_t guestHandle{};
        uintptr_t hostValue{};
        std::vector<uint8_t> pcm;
        uint32_t avgBytesPerSec{};
        uint16_t blockAlign{};
    };

    struct GuestWaveOutState {
        uint32_t callback{};
        uint32_t instance{};
        uint32_t flags{};
        uint32_t avgBytesPerSec{};
        uint32_t deviceId{};
        uint32_t hostFlags{};
        uintptr_t hostCallback{};
        uint16_t formatTag{};
        uint16_t channels{};
        uint32_t samplesPerSec{};
        uint16_t blockAlign{};
        uint16_t bitsPerSample{};
    };

    struct CachedWaveOutDevice {
        uintptr_t hostValue{};
        uint32_t deviceId{};
        uint32_t hostFlags{};
        uintptr_t hostCallback{};
        uint16_t formatTag{};
        uint16_t channels{};
        uint32_t samplesPerSec{};
        uint32_t avgBytesPerSec{};
        uint16_t blockAlign{};
        uint16_t bitsPerSample{};
    };

    static constexpr std::string_view name() noexcept { return "CE audio stream"; }
    static constexpr std::string_view role() noexcept {
        return "Owner for virtual waveOut stream timing and live playback taps.";
    }

    void openStream(StreamConfig config);
    std::vector<Completion> closeStream(uint32_t handle);
    std::vector<Completion> resetStream(uint32_t handle);
    std::optional<QueueResult> queueBuffer(uint32_t handle,
                                           uint32_t guestHeader,
                                           uint32_t guestUser,
                                           std::vector<uint8_t> pcm,
                                           uint64_t nowMs);
    bool hasQueuedHeader(uint32_t handle, uint32_t guestHeader) const;
    std::vector<Completion> completeReady(uint64_t nowMs);
    std::optional<LiveSlice> liveSlice(uint64_t cursorMs, uint32_t durationMs) const;
    bool hasLiveAudioAtOrAfter(uint64_t cursorMs) const;

private:
    struct QueuedBuffer {
        uint32_t guestHeader{};
        uint32_t completionEvent{};
        std::vector<uint8_t> pcm;
        uint64_t startMs{};
        uint64_t endMs{};
    };

    struct StreamState {
        StreamConfig config;
        uint64_t nextStartMs{};
        std::deque<QueuedBuffer> buffers;
    };

    static uint32_t durationForBytes(const Format& format, size_t bytes);
    static size_t byteOffsetForElapsed(const Format& format, uint64_t elapsedMs);
    static size_t alignDown(size_t value, uint16_t alignment);
    static uint32_t completionEventFor(const StreamConfig& config, uint32_t guestUser);

    mutable std::mutex mutex_;
    std::map<uint32_t, StreamState> streams_;
};
