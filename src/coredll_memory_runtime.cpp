#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <vector>

uint32_t SyntheticDllRuntime::reg(int regId) const {
    uint32_t value = 0;
    uc_reg_read(uc_, regId, &value);
    return value;
}

void SyntheticDllRuntime::setReg(int regId, uint32_t value) const {
    uc_reg_write(uc_, regId, &value);
}

uint32_t SyntheticDllRuntime::stackArg(uint32_t index) const {
    uint32_t value = 0;
    const uint32_t sp = reg(UC_MIPS_REG_SP);
    uc_mem_read(uc_, sp + index * 4, &value, sizeof(value));
    return value;
}

uint32_t SyntheticDllRuntime::allocate(uint32_t size, bool zeroFill) {
    size = std::max<uint32_t>(size, 1);
    const uint32_t capacity = (size + 0x0fu) & ~0x0fu;
    uint32_t address = 0;
    uint32_t blockCapacity = capacity;
    auto freeIt = freeBlocksBySize_.lower_bound(capacity);
    if (freeIt != freeBlocksBySize_.end()) {
        blockCapacity = freeIt->first;
        address = freeIt->second;
        freeBlocksBySize_.erase(freeIt);
        const uint32_t remainder = blockCapacity - capacity;
        blockCapacity = capacity;
        if (remainder >= 0x20u) {
            freeBlocksBySize_.emplace(remainder, address + capacity);
        } else {
            blockCapacity += remainder;
        }
    } else {
        if (nextHeap_ + capacity > heapLimit_) {
            spdlog::warn("guest heap exhausted requested={} capacity={} next=0x{:08x} limit=0x{:08x} freeBlocks={}",
                         size, capacity, nextHeap_, heapLimit_, freeBlocksBySize_.size());
            lastError_ = 14; // ERROR_OUTOFMEMORY
            return 0;
        }
        address = nextHeap_;
        nextHeap_ += capacity;
    }
    allocationSizes_[address] = size;
    allocationCapacities_[address] = blockCapacity;
    if (zeroFill) {
        std::vector<uint8_t> zeros(blockCapacity);
        uc_mem_write(uc_, address, zeros.data(), zeros.size());
    }
    lastError_ = 0;
    return address;
}

void SyntheticDllRuntime::releaseAllocation(uint32_t address) {
    if (!address) return;
    auto sizeIt = allocationSizes_.find(address);
    auto capacityIt = allocationCapacities_.find(address);
    if (sizeIt == allocationSizes_.end() || capacityIt == allocationCapacities_.end()) return;
    const uint32_t capacity = capacityIt->second;
    allocationSizes_.erase(sizeIt);
    allocationCapacities_.erase(capacityIt);
    if (capacity) freeBlocksBySize_.emplace(capacity, address);
}

uint32_t SyntheticDllRuntime::allocationSize(uint32_t address) const {
    auto it = allocationSizes_.find(address);
    return it == allocationSizes_.end() ? 0 : it->second;
}

uint32_t SyntheticDllRuntime::readU32(uint32_t address) const {
    uint32_t value = 0;
    if (address) uc_mem_read(uc_, address, &value, sizeof(value));
    return value;
}

void SyntheticDllRuntime::writeU32(uint32_t address, uint32_t value) const {
    if (address) uc_mem_write(uc_, address, &value, sizeof(value));
}

bool SyntheticDllRuntime::isGuestRangeReadable(uint32_t address, uint32_t size) const {
    if (!address) return false;
    if (!size) return true;
    uint8_t byte = 0;
    if (uc_mem_read(uc_, address, &byte, sizeof(byte)) != UC_ERR_OK) return false;
    const uint32_t last = address + size - 1;
    if (last < address) return false;
    return uc_mem_read(uc_, last, &byte, sizeof(byte)) == UC_ERR_OK;
}

bool SyntheticDllRuntime::copyGuest(uint32_t dst, uint32_t src, uint32_t size) const {
    if (!dst || !src || !size || size > 0x100000) return false;
    std::vector<uint8_t> bytes(size);
    if (uc_mem_read(uc_, src, bytes.data(), bytes.size()) != UC_ERR_OK) return false;
    return uc_mem_write(uc_, dst, bytes.data(), bytes.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::fillGuest(uint32_t dst, uint8_t value, uint32_t size) const {
    if (!dst || !size || size > 0x100000) return false;
    std::vector<uint8_t> bytes(size, value);
    return uc_mem_write(uc_, dst, bytes.data(), bytes.size()) == UC_ERR_OK;
}
