#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

template <typename T>
class CeSlotTable {
public:
    explicit CeSlotTable(uint32_t firstHandle = 1, uint32_t handleStride = 1)
        : firstHandle_(firstHandle), handleStride_(handleStride ? handleStride : 1) {}

    uint32_t insert(T value) {
        if (!freeSlots_.empty()) {
            const size_t index = freeSlots_.back();
            freeSlots_.pop_back();
            slots_[index] = std::move(value);
            return handleForIndex(index);
        }
        slots_.push_back(std::move(value));
        return handleForIndex(slots_.size() - 1);
    }

    bool insertAt(uint32_t handle, T value) {
        const auto index = indexForHandle(handle);
        if (!index) return false;
        if (*index >= slots_.size()) slots_.resize(*index + 1);
        if (!slots_[*index]) {
            removeFreeSlot(*index);
        }
        slots_[*index] = std::move(value);
        return true;
    }

    T* get(uint32_t handle) {
        const auto index = indexForHandle(handle);
        if (!index || *index >= slots_.size() || !slots_[*index]) return nullptr;
        return &*slots_[*index];
    }

    const T* get(uint32_t handle) const {
        const auto index = indexForHandle(handle);
        if (!index || *index >= slots_.size() || !slots_[*index]) return nullptr;
        return &*slots_[*index];
    }

    bool erase(uint32_t handle) {
        const auto index = indexForHandle(handle);
        if (!index || *index >= slots_.size() || !slots_[*index]) return false;
        slots_[*index].reset();
        freeSlots_.push_back(*index);
        return true;
    }

    bool contains(uint32_t handle) const {
        return get(handle) != nullptr;
    }

    size_t size() const noexcept {
        return slots_.size() - freeSlots_.size();
    }

    bool empty() const noexcept {
        return size() == 0;
    }

private:
    uint32_t handleForIndex(size_t index) const {
        return firstHandle_ + uint32_t(index) * handleStride_;
    }

    std::optional<size_t> indexForHandle(uint32_t handle) const {
        if (handle < firstHandle_) return std::nullopt;
        const uint32_t delta = handle - firstHandle_;
        if (delta % handleStride_ != 0) return std::nullopt;
        return size_t(delta / handleStride_);
    }

    void removeFreeSlot(size_t index) {
        for (auto it = freeSlots_.begin(); it != freeSlots_.end(); ++it) {
            if (*it == index) {
                freeSlots_.erase(it);
                return;
            }
        }
    }

    uint32_t firstHandle_{1};
    uint32_t handleStride_{1};
    std::vector<std::optional<T>> slots_;
    std::vector<size_t> freeSlots_;
};
