#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>

class CrossProcessBroker {
public:
    struct ImportedWindow {
        uint32_t guestHwnd{};
        uint32_t processId{};
        uint32_t externalHwnd{};
    };

    void setWindowRegistryPath(std::filesystem::path path);
    const std::filesystem::path& windowRegistryPath() const noexcept;

    void setMessageQueuePath(std::filesystem::path path);
    const std::filesystem::path& messageQueuePath() const noexcept;

    void setSharedMappingDirectory(std::filesystem::path path);
    const std::filesystem::path& sharedMappingDirectory() const noexcept;

    void rememberImportedWindow(uint32_t processId, uint32_t externalHwnd, uint32_t guestHwnd);
    std::optional<uint32_t> importedGuestWindow(uint32_t processId, uint32_t externalHwnd) const;
    void forgetImportedWindow(uint32_t guestHwnd);

    std::filesystem::file_time_type lastMessageQueueWrite() const noexcept;
    void setLastMessageQueueWrite(std::filesystem::file_time_type value) noexcept;
    std::uintmax_t lastMessageQueueSize() const noexcept;
    void setLastMessageQueueSize(std::uintmax_t value) noexcept;
    std::chrono::steady_clock::time_point lastMessagePollAt() const noexcept;
    void setLastMessagePollAt(std::chrono::steady_clock::time_point value) noexcept;
    bool hasMessageQueueStat() const noexcept;
    void setHasMessageQueueStat(bool value) noexcept;

private:
    std::filesystem::path windowRegistryPath_;
    std::filesystem::path messageQueuePath_;
    std::filesystem::path sharedMappingDirectory_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> importedWindows_;
    std::filesystem::file_time_type lastMessageQueueWrite_{};
    std::uintmax_t lastMessageQueueSize_{};
    std::chrono::steady_clock::time_point lastMessagePollAt_{};
    bool hasMessageQueueStat_{};
};
