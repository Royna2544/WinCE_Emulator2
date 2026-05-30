#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <string_view>

class CeGwe {
public:
    struct GuestMessage {
        uint32_t hwnd{};
        uint32_t message{};
        uint32_t wParam{};
        uint32_t lParam{};
        uint32_t time{};
        uint32_t x{};
        uint32_t y{};
        uint32_t synchronousSender{};
        bool crossProcess{};
    };

    static constexpr std::string_view name() noexcept { return "CE GWE"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for GWE message queues, windows, input, timers, and paint regions.";
    }

    std::deque<GuestMessage>& messages() noexcept { return messages_; }
    const std::deque<GuestMessage>& messages() const noexcept { return messages_; }
    size_t messageCount() const noexcept { return messages_.size(); }
    bool hasMessages() const noexcept { return !messages_.empty(); }
    void postMessage(const GuestMessage& message) { messages_.push_back(message); }
    void postMessage(GuestMessage&& message) { messages_.push_back(message); }
    void postFront(const GuestMessage& message) { messages_.push_front(message); }

    template <typename Predicate>
    bool anyMessage(Predicate predicate) const {
        for (const auto& message : messages_) {
            if (predicate(message)) return true;
        }
        return false;
    }

    template <typename Predicate>
    void postAfterLeadingMatches(const GuestMessage& message, Predicate predicate) {
        auto it = messages_.begin();
        while (it != messages_.end() && predicate(*it)) {
            ++it;
        }
        messages_.insert(it, message);
    }

    template <typename Predicate>
    void postBeforeFirstMatch(const GuestMessage& message, Predicate predicate) {
        auto it = messages_.begin();
        while (it != messages_.end() && !predicate(*it)) {
            ++it;
        }
        messages_.insert(it, message);
    }

    template <typename Predicate>
    size_t eraseIf(Predicate predicate) {
        const size_t oldSize = messages_.size();
        for (auto it = messages_.begin(); it != messages_.end();) {
            if (predicate(*it)) {
                it = messages_.erase(it);
            } else {
                ++it;
            }
        }
        return oldSize - messages_.size();
    }

    template <typename Predicate>
    std::deque<GuestMessage> takeIf(Predicate predicate) {
        std::deque<GuestMessage> selected;
        for (auto it = messages_.begin(); it != messages_.end();) {
            if (predicate(*it)) {
                selected.push_back(*it);
                it = messages_.erase(it);
            } else {
                ++it;
            }
        }
        return selected;
    }

    template <typename Predicate>
    size_t eraseReverseIf(Predicate predicate) {
        size_t erased = 0;
        for (auto it = messages_.rbegin(); it != messages_.rend();) {
            if (predicate(*it)) {
                auto eraseIt = std::next(it).base();
                it = std::make_reverse_iterator(messages_.erase(eraseIt));
                ++erased;
            } else {
                ++it;
            }
        }
        return erased;
    }

    template <typename Predicate>
    std::optional<GuestMessage> firstMatching(Predicate predicate, bool remove) {
        for (auto it = messages_.begin(); it != messages_.end(); ++it) {
            if (!predicate(*it)) continue;
            GuestMessage message = *it;
            if (remove) messages_.erase(it);
            return message;
        }
        return std::nullopt;
    }

    template <typename Visitor>
    void forEachMessage(Visitor visitor) const {
        size_t index = 0;
        for (const auto& message : messages_) {
            if (!visitor(message, index)) break;
            ++index;
        }
    }

private:
    std::deque<GuestMessage> messages_;
};
