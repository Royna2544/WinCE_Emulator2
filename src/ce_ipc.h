#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

class CeIpc {
public:
    struct GuestFileMapping {
        uint32_t fileHandle{};
        uint64_t size{};
        uint32_t protect{};
        std::string name;
        std::filesystem::path backingPath;
        bool namedShared{};
    };

    struct GuestMappedView {
        uint32_t mappingHandle{};
        uint64_t offset{};
        uint32_t size{};
        std::vector<uint8_t> shadow;
        uint64_t backingVersion{};
        uint32_t refCount{1};
    };

    static constexpr std::string_view name() noexcept { return "CE IPC/mapfile"; }
    static constexpr std::string_view role() noexcept {
        return "Owner for guest-visible file mapping and mapped-view state.";
    }

    std::map<uint32_t, GuestFileMapping>& fileMappings() noexcept { return fileMappings_; }
    const std::map<uint32_t, GuestFileMapping>& fileMappings() const noexcept { return fileMappings_; }
    std::map<uint32_t, GuestMappedView>& mappedViews() noexcept { return mappedViews_; }
    const std::map<uint32_t, GuestMappedView>& mappedViews() const noexcept { return mappedViews_; }

private:
    std::map<uint32_t, GuestFileMapping> fileMappings_;
    std::map<uint32_t, GuestMappedView> mappedViews_;
};
