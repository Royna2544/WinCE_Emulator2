#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

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
};
