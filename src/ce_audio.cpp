#include "ce_audio.h"

#include <algorithm>
#include <utility>

static_assert(!CeAudio::name().empty());
static_assert(!CeAudio::role().empty());

namespace {
constexpr uint32_t kCallbackTypeMask = 0x00070000u;
constexpr uint32_t kCallbackEvent = 0x00050000u;
constexpr uint32_t kCallbackFunction = 0x00030000u;
}

void CeAudio::openStream(StreamConfig config) {
    if (!config.handle) return;
    std::lock_guard<std::mutex> lock(mutex_);
    StreamState state;
    state.config = config;
    streams_[config.handle] = std::move(state);
}

std::vector<CeAudio::Completion> CeAudio::closeStream(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Completion> completions;
    auto it = streams_.find(handle);
    if (it == streams_.end()) return completions;
    for (const QueuedBuffer& buffer : it->second.buffers) {
        completions.push_back({handle, buffer.guestHeader, buffer.completionEvent});
    }
    streams_.erase(it);
    return completions;
}

std::vector<CeAudio::Completion> CeAudio::resetStream(uint32_t handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Completion> completions;
    auto it = streams_.find(handle);
    if (it == streams_.end()) return completions;
    for (const QueuedBuffer& buffer : it->second.buffers) {
        completions.push_back({handle, buffer.guestHeader, buffer.completionEvent});
    }
    it->second.buffers.clear();
    it->second.nextStartMs = 0;
    return completions;
}

std::optional<CeAudio::QueueResult> CeAudio::queueBuffer(uint32_t handle,
                                                        uint32_t guestHeader,
                                                        uint32_t guestUser,
                                                        std::vector<uint8_t> pcm,
                                                        uint64_t nowMs) {
    if (!handle || !guestHeader || pcm.empty()) return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(handle);
    if (it == streams_.end()) return std::nullopt;

    StreamState& stream = it->second;
    const uint32_t durationMs = durationForBytes(stream.config.format, pcm.size());
    const uint64_t startMs = std::max(nowMs, stream.nextStartMs);
    const uint64_t endMs = startMs + std::max<uint32_t>(1, durationMs);
    const uint32_t completionEvent = completionEventFor(stream.config, guestUser);

    stream.buffers.push_back({guestHeader, completionEvent, std::move(pcm), startMs, endMs});
    stream.nextStartMs = endMs;

    return QueueResult{guestHeader, completionEvent, startMs, endMs, std::max<uint32_t>(1, durationMs)};
}

bool CeAudio::hasQueuedHeader(uint32_t handle, uint32_t guestHeader) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(handle);
    if (it == streams_.end()) return false;
    return std::any_of(it->second.buffers.begin(), it->second.buffers.end(),
                       [&](const QueuedBuffer& buffer) {
                           return buffer.guestHeader == guestHeader;
                       });
}

std::vector<CeAudio::Completion> CeAudio::completeReady(uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Completion> completions;
    for (auto& [handle, stream] : streams_) {
        while (!stream.buffers.empty() && stream.buffers.front().endMs <= nowMs) {
            const QueuedBuffer& buffer = stream.buffers.front();
            completions.push_back({handle, buffer.guestHeader, buffer.completionEvent});
            stream.buffers.pop_front();
        }
        if (stream.buffers.empty() && stream.nextStartMs < nowMs) {
            stream.nextStartMs = nowMs;
        }
    }
    return completions;
}

std::optional<CeAudio::LiveSlice> CeAudio::liveSlice(uint64_t cursorMs, uint32_t durationMs) const {
    if (!durationMs) return std::nullopt;
    std::lock_guard<std::mutex> lock(mutex_);

    const StreamState* selectedStream = nullptr;
    const QueuedBuffer* selectedBuffer = nullptr;
    uint64_t selectedStart = UINT64_MAX;

    for (const auto& [handle, stream] : streams_) {
        (void)handle;
        for (const QueuedBuffer& buffer : stream.buffers) {
            if (cursorMs >= buffer.endMs) continue;
            const uint64_t candidateStart = std::max(cursorMs, buffer.startMs);
            if (candidateStart < selectedStart) {
                selectedStart = candidateStart;
                selectedStream = &stream;
                selectedBuffer = &buffer;
            }
            break;
        }
    }

    if (!selectedStream || !selectedBuffer || selectedStart >= selectedBuffer->endMs) return std::nullopt;
    if (selectedStart > cursorMs) return std::nullopt;

    const Format& format = selectedStream->config.format;
    const uint16_t blockAlign = std::max<uint16_t>(1, format.blockAlign);
    size_t offset = byteOffsetForElapsed(format, selectedStart - selectedBuffer->startMs);
    offset = alignDown(offset, blockAlign);
    if (offset >= selectedBuffer->pcm.size()) return std::nullopt;

    const uint64_t sliceEndMs = std::min<uint64_t>(selectedBuffer->endMs, selectedStart + durationMs);
    size_t endOffset = byteOffsetForElapsed(format, sliceEndMs - selectedBuffer->startMs);
    endOffset = std::max(offset + blockAlign, alignDown(endOffset, blockAlign));
    endOffset = std::min(endOffset, selectedBuffer->pcm.size());
    if (endOffset <= offset) return std::nullopt;

    LiveSlice slice;
    slice.format = format;
    slice.startMs = selectedStart;
    slice.pcm.assign(selectedBuffer->pcm.begin() + offset, selectedBuffer->pcm.begin() + endOffset);
    slice.durationMs = durationForBytes(format, slice.pcm.size());
    return slice;
}

bool CeAudio::hasLiveAudioAtOrAfter(uint64_t cursorMs) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [handle, stream] : streams_) {
        (void)handle;
        for (const QueuedBuffer& buffer : stream.buffers) {
            if (cursorMs < buffer.endMs) return true;
        }
    }
    return false;
}

uint32_t CeAudio::durationForBytes(const Format& format, size_t bytes) {
    if (!bytes || !format.avgBytesPerSec) return 1;
    return static_cast<uint32_t>(
        std::max<uint64_t>(1, (uint64_t(bytes) * 1000ull) / uint64_t(format.avgBytesPerSec)));
}

size_t CeAudio::byteOffsetForElapsed(const Format& format, uint64_t elapsedMs) {
    if (!format.avgBytesPerSec) return 0;
    return static_cast<size_t>((elapsedMs * uint64_t(format.avgBytesPerSec)) / 1000ull);
}

size_t CeAudio::alignDown(size_t value, uint16_t alignment) {
    const size_t align = std::max<size_t>(1, alignment);
    return value - (value % align);
}

uint32_t CeAudio::completionEventFor(const StreamConfig& config, uint32_t guestUser) {
    const uint32_t callbackKind = config.flags & kCallbackTypeMask;
    if (callbackKind == kCallbackEvent) return config.callback;
    if (callbackKind == kCallbackFunction) return guestUser;
    return 0;
}
