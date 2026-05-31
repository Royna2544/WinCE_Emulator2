#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

template <typename HandlerSpec>
class OrdinalDispatchTable {
public:
    void resize(uint16_t maxOrdinal) {
        slots_.resize(size_t(maxOrdinal) + 1);
    }

    void set(uint16_t ordinal, HandlerSpec spec) {
        if (ordinal >= slots_.size()) slots_.resize(size_t(ordinal) + 1);
        slots_[ordinal] = std::move(spec);
    }

    const HandlerSpec* get(uint16_t ordinal) const {
        if (ordinal >= slots_.size()) return nullptr;
        const auto& slot = slots_[ordinal];
        return slot ? &*slot : nullptr;
    }

    HandlerSpec* get(uint16_t ordinal) {
        if (ordinal >= slots_.size()) return nullptr;
        auto& slot = slots_[ordinal];
        return slot ? &*slot : nullptr;
    }

    uint16_t maxOrdinal() const noexcept {
        return slots_.empty() ? 0 : uint16_t(slots_.size() - 1);
    }

    bool empty() const noexcept {
        return slots_.empty();
    }

private:
    std::vector<std::optional<HandlerSpec>> slots_;
};
