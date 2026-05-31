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
    auto freeIt = ceMemory_.freeBlocksBySize().lower_bound(capacity);
    if (freeIt != ceMemory_.freeBlocksBySize().end()) {
        blockCapacity = freeIt->first;
        address = freeIt->second;
        ceMemory_.freeBlocksBySize().erase(freeIt);
        const uint32_t remainder = blockCapacity - capacity;
        blockCapacity = capacity;
        if (remainder >= 0x20u) {
            ceMemory_.freeBlocksBySize().emplace(remainder, address + capacity);
        } else {
            blockCapacity += remainder;
        }
    } else {
        if (ceMemory_.nextHeap() + capacity > ceMemory_.heapLimit()) {
            spdlog::warn("guest heap exhausted requested={} capacity={} next=0x{:08x} limit=0x{:08x} freeBlocks={}",
                         size, capacity, ceMemory_.nextHeap(), ceMemory_.heapLimit(), ceMemory_.freeBlocksBySize().size());
            lastError_ = 14; // ERROR_OUTOFMEMORY
            return 0;
        }
        address = ceMemory_.nextHeap();
        ceMemory_.nextHeap() += capacity;
    }
    ceMemory_.allocationSizes()[address] = size;
    ceMemory_.allocationCapacities()[address] = blockCapacity;
    if (zeroFill) {
        std::vector<uint8_t> zeros(blockCapacity);
        uc_mem_write(uc_, address, zeros.data(), zeros.size());
    }
    lastError_ = 0;
    return address;
}

void SyntheticDllRuntime::releaseAllocation(uint32_t address) {
    if (!address) return;
    auto sizeIt = ceMemory_.allocationSizes().find(address);
    auto capacityIt = ceMemory_.allocationCapacities().find(address);
    if (sizeIt == ceMemory_.allocationSizes().end() || capacityIt == ceMemory_.allocationCapacities().end()) return;
    const uint32_t capacity = capacityIt->second;
    ceMemory_.allocationSizes().erase(sizeIt);
    ceMemory_.allocationCapacities().erase(capacityIt);
    if (capacity) ceMemory_.freeBlocksBySize().emplace(capacity, address);
}

uint32_t SyntheticDllRuntime::allocationSize(uint32_t address) const {
    auto it = ceMemory_.allocationSizes().find(address);
    return it == ceMemory_.allocationSizes().end() ? 0 : it->second;
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
