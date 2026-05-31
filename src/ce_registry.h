#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

class CeRegistry {
public:
    static constexpr std::string_view name() noexcept { return "CE registry"; }
    static constexpr std::string_view role() noexcept {
        return "Owner for guest-visible registry database and open registry-key handles.";
    }

    std::filesystem::path& path() noexcept { return path_; }
    const std::filesystem::path& path() const noexcept { return path_; }
    nlohmann::json& database() noexcept { return database_; }
    const nlohmann::json& database() const noexcept { return database_; }
    bool& dirty() noexcept { return dirty_; }
    bool dirty() const noexcept { return dirty_; }
    std::map<uint32_t, std::string>& handles() noexcept { return handles_; }
    const std::map<uint32_t, std::string>& handles() const noexcept { return handles_; }

private:
    std::filesystem::path path_;
    nlohmann::json database_;
    bool dirty_{};
    std::map<uint32_t, std::string> handles_;
};
