#pragma once

#include <cstdint>
#include <map>
#include <string_view>
#include <unordered_map>

class CeMemory {
public:
    static constexpr std::string_view name() noexcept { return "CE memory"; }
    static constexpr std::string_view role() noexcept {
        return "Owner for guest heap bounds, allocation metadata, and reusable free blocks.";
    }

    uint32_t& heapBase() noexcept { return heapBase_; }
    uint32_t heapBase() const noexcept { return heapBase_; }
    uint32_t& heapLimit() noexcept { return heapLimit_; }
    uint32_t heapLimit() const noexcept { return heapLimit_; }
    uint32_t& nextHeap() noexcept { return nextHeap_; }
    uint32_t nextHeap() const noexcept { return nextHeap_; }
    std::unordered_map<uint32_t, uint32_t>& allocationSizes() noexcept { return allocationSizes_; }
    const std::unordered_map<uint32_t, uint32_t>& allocationSizes() const noexcept { return allocationSizes_; }
    std::unordered_map<uint32_t, uint32_t>& allocationCapacities() noexcept { return allocationCapacities_; }
    const std::unordered_map<uint32_t, uint32_t>& allocationCapacities() const noexcept {
        return allocationCapacities_;
    }
    std::multimap<uint32_t, uint32_t>& freeBlocksBySize() noexcept { return freeBlocksBySize_; }
    const std::multimap<uint32_t, uint32_t>& freeBlocksBySize() const noexcept { return freeBlocksBySize_; }

private:
    uint32_t heapBase_{0x30000000};
    uint32_t heapLimit_{0x34000000};
    uint32_t nextHeap_{0x30010000};
    std::unordered_map<uint32_t, uint32_t> allocationSizes_;
    std::unordered_map<uint32_t, uint32_t> allocationCapacities_;
    std::multimap<uint32_t, uint32_t> freeBlocksBySize_;
};
