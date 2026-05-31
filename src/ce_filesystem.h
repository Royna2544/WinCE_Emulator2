#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

class CeFilesystem {
public:
    struct CachedFileAttributes {
        bool ok{};
        uint32_t error{};
        uint32_t attributes{};
        uint32_t creationLow{};
        uint32_t creationHigh{};
        uint32_t accessLow{};
        uint32_t accessHigh{};
        uint32_t writeLow{};
        uint32_t writeHigh{};
        uint32_t sizeHigh{};
        uint32_t sizeLow{};
    };

    static constexpr std::string_view name() noexcept { return "CE filesystem"; }
    static constexpr std::string_view role() noexcept {
        return "Owner for guest-visible file-handle bookkeeping and file attribute cache state.";
    }

    std::map<uint32_t, std::string>& fileHandleDebugNames() noexcept { return fileHandleDebugNames_; }
    const std::map<uint32_t, std::string>& fileHandleDebugNames() const noexcept {
        return fileHandleDebugNames_;
    }
    std::map<std::wstring, CachedFileAttributes>& fileAttributeCache() noexcept {
        return fileAttributeCache_;
    }
    const std::map<std::wstring, CachedFileAttributes>& fileAttributeCache() const noexcept {
        return fileAttributeCache_;
    }
    std::unordered_map<uint32_t, uint32_t>& fileReadCounts() noexcept { return fileReadCounts_; }
    const std::unordered_map<uint32_t, uint32_t>& fileReadCounts() const noexcept { return fileReadCounts_; }
    std::unordered_map<uint32_t, uint32_t>& fileSeekCounts() noexcept { return fileSeekCounts_; }
    const std::unordered_map<uint32_t, uint32_t>& fileSeekCounts() const noexcept { return fileSeekCounts_; }

private:
    std::map<uint32_t, std::string> fileHandleDebugNames_;
    std::map<std::wstring, CachedFileAttributes> fileAttributeCache_;
    std::unordered_map<uint32_t, uint32_t> fileReadCounts_;
    std::unordered_map<uint32_t, uint32_t> fileSeekCounts_;
};
