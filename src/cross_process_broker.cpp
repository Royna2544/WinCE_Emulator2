#include "cross_process_broker.h"

void CrossProcessBroker::setWindowRegistryPath(std::filesystem::path path) {
    windowRegistryPath_ = std::move(path);
}

const std::filesystem::path& CrossProcessBroker::windowRegistryPath() const noexcept {
    return windowRegistryPath_;
}

void CrossProcessBroker::setMessageQueuePath(std::filesystem::path path) {
    messageQueuePath_ = std::move(path);
}

const std::filesystem::path& CrossProcessBroker::messageQueuePath() const noexcept {
    return messageQueuePath_;
}

void CrossProcessBroker::setSharedMappingDirectory(std::filesystem::path path) {
    sharedMappingDirectory_ = std::move(path);
}

const std::filesystem::path& CrossProcessBroker::sharedMappingDirectory() const noexcept {
    return sharedMappingDirectory_;
}

void CrossProcessBroker::rememberImportedWindow(uint32_t processId,
                                                uint32_t externalHwnd,
                                                uint32_t guestHwnd) {
    if (!processId || !externalHwnd || !guestHwnd) return;
    importedWindows_[{processId, externalHwnd}] = guestHwnd;
}

std::optional<uint32_t> CrossProcessBroker::importedGuestWindow(uint32_t processId,
                                                                uint32_t externalHwnd) const {
    const auto it = importedWindows_.find({processId, externalHwnd});
    if (it == importedWindows_.end()) return std::nullopt;
    return it->second;
}

void CrossProcessBroker::forgetImportedWindow(uint32_t guestHwnd) {
    for (auto it = importedWindows_.begin(); it != importedWindows_.end();) {
        if (it->second == guestHwnd) {
            it = importedWindows_.erase(it);
        } else {
            ++it;
        }
    }
}

std::filesystem::file_time_type CrossProcessBroker::lastMessageQueueWrite() const noexcept {
    return lastMessageQueueWrite_;
}

void CrossProcessBroker::setLastMessageQueueWrite(std::filesystem::file_time_type value) noexcept {
    lastMessageQueueWrite_ = value;
}

std::uintmax_t CrossProcessBroker::lastMessageQueueSize() const noexcept {
    return lastMessageQueueSize_;
}

void CrossProcessBroker::setLastMessageQueueSize(std::uintmax_t value) noexcept {
    lastMessageQueueSize_ = value;
}

std::chrono::steady_clock::time_point CrossProcessBroker::lastMessagePollAt() const noexcept {
    return lastMessagePollAt_;
}

void CrossProcessBroker::setLastMessagePollAt(std::chrono::steady_clock::time_point value) noexcept {
    lastMessagePollAt_ = value;
}

bool CrossProcessBroker::hasMessageQueueStat() const noexcept {
    return hasMessageQueueStat_;
}

void CrossProcessBroker::setHasMessageQueueStat(bool value) noexcept {
    hasMessageQueueStat_ = value;
}
