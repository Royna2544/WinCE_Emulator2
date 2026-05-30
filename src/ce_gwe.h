#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <string_view>

class CeGwe {
public:
    static constexpr uint32_t kNoOwnerThread = 0;
    static constexpr uint64_t kFirstMessageId = 1;

    enum class MessageQueueKind {
        Posted,
        Sent,
        Input,
        Timer,
        Thread,
    };

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
        uint64_t queueId{};
        MessageQueueKind queueKind{MessageQueueKind::Posted};
    };

    struct ThreadQueue {
        uint32_t ownerThread{};
        std::deque<GuestMessage> posted;
        std::deque<GuestMessage> sent;
        std::deque<GuestMessage> input;
        std::deque<GuestMessage> timers;
        std::deque<GuestMessage> thread;
    };

    struct Rect {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    struct WindowRegionState {
        uint32_t hwnd{};
        uint32_t ownerThread{};
        uint32_t parent{};
        uint32_t style{};
        uint32_t exStyle{};
        Rect windowRect;
        Rect clientRect;
        Rect visibleRect;
        Rect updateRect;
        Rect clientVisibleRect;
        Rect clientUpdateRect;
        bool visible{};
        bool destroyed{};
        bool hasVisibleRegion{};
        bool hasUpdateRegion{};
        bool hasClientVisibleRegion{};
        bool hasClientUpdateRegion{};
    };

    static constexpr std::string_view name() noexcept { return "CE GWE"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for GWE message queues, windows, input, timers, and paint regions.";
    }

    std::deque<GuestMessage>& messages() noexcept { return messages_; }
    const std::deque<GuestMessage>& messages() const noexcept { return messages_; }
    size_t messageCount() const noexcept { return messages_.size(); }
    bool hasMessages() const noexcept { return !messages_.empty(); }
    void ensureThreadQueue(uint32_t ownerThread) {
        if (!ownerThread) return;
        threadQueues_.try_emplace(ownerThread, ThreadQueue{ownerThread});
    }
    void registerWindowOwner(uint32_t hwnd, uint32_t ownerThread) {
        if (!hwnd) return;
        if (ownerThread) ensureThreadQueue(ownerThread);
        windowOwners_[hwnd] = ownerThread;
    }
    void unregisterWindow(uint32_t hwnd) {
        windowOwners_.erase(hwnd);
        windowRegions_.erase(hwnd);
    }
    uint32_t ownerForWindow(uint32_t hwnd) const {
        auto it = windowOwners_.find(hwnd);
        return it == windowOwners_.end() ? kNoOwnerThread : it->second;
    }
    void updateWindowState(uint32_t hwnd,
                           uint32_t ownerThread,
                           uint32_t parent,
                           uint32_t style,
                           uint32_t exStyle,
                           int32_t x,
                           int32_t y,
                           int32_t width,
                           int32_t height,
                           bool visible,
                           bool destroyed) {
        if (!hwnd) return;
        registerWindowOwner(hwnd, ownerThread);
        auto& state = windowRegions_[hwnd];
        state.hwnd = hwnd;
        state.ownerThread = ownerThread;
        state.parent = parent;
        state.style = style;
        state.exStyle = exStyle;
        state.windowRect = Rect{x, y, x + width, y + height};
        state.clientRect = Rect{0, 0, width, height};
        state.visible = visible;
        state.destroyed = destroyed;
        state.hasVisibleRegion = visible && !destroyed;
        state.visibleRect = state.hasVisibleRegion ? state.windowRect : Rect{};
        state.hasClientVisibleRegion = state.hasVisibleRegion;
        state.clientVisibleRect = state.hasClientVisibleRegion ? state.clientRect : Rect{};
        if (destroyed) {
            state.hasUpdateRegion = false;
            state.hasClientUpdateRegion = false;
            state.updateRect = Rect{};
            state.clientUpdateRect = Rect{};
        }
    }
    void invalidateWindow(uint32_t hwnd, std::optional<Rect> clientRect = std::nullopt) {
        auto it = windowRegions_.find(hwnd);
        if (it == windowRegions_.end()) return;
        auto& state = it->second;
        state.hasUpdateRegion = true;
        state.hasClientUpdateRegion = true;
        state.clientUpdateRect = clientRect.value_or(state.clientRect);
        state.updateRect = Rect{
            state.windowRect.left + state.clientUpdateRect.left,
            state.windowRect.top + state.clientUpdateRect.top,
            state.windowRect.left + state.clientUpdateRect.right,
            state.windowRect.top + state.clientUpdateRect.bottom,
        };
    }
    void validateWindow(uint32_t hwnd) {
        auto it = windowRegions_.find(hwnd);
        if (it == windowRegions_.end()) return;
        it->second.hasUpdateRegion = false;
        it->second.hasClientUpdateRegion = false;
        it->second.updateRect = Rect{};
        it->second.clientUpdateRect = Rect{};
    }
    const WindowRegionState* windowRegionState(uint32_t hwnd) const {
        auto it = windowRegions_.find(hwnd);
        return it == windowRegions_.end() ? nullptr : &it->second;
    }
    uint32_t ownerForMessage(const GuestMessage& message) const {
        const auto owner = messageOwners_.find(message.queueId);
        if (owner != messageOwners_.end()) return owner->second;
        return message.hwnd ? ownerForWindow(message.hwnd) : kNoOwnerThread;
    }
    const std::map<uint32_t, ThreadQueue>& threadQueues() const noexcept { return threadQueues_; }
    static size_t laneMessageCount(const ThreadQueue& queue, MessageQueueKind kind) noexcept {
        switch (kind) {
        case MessageQueueKind::Sent:
            return queue.sent.size();
        case MessageQueueKind::Input:
            return queue.input.size();
        case MessageQueueKind::Timer:
            return queue.timers.size();
        case MessageQueueKind::Thread:
            return queue.thread.size();
        case MessageQueueKind::Posted:
        default:
            return queue.posted.size();
        }
    }
    size_t messageCountForOwner(uint32_t ownerThread) const noexcept {
        const auto queue = threadQueues_.find(ownerThread);
        if (queue == threadQueues_.end()) return 0;
        return queue->second.posted.size() +
               queue->second.sent.size() +
               queue->second.input.size() +
               queue->second.timers.size() +
               queue->second.thread.size();
    }
    bool hasMessagesForOwner(uint32_t ownerThread) const noexcept {
        return messageCountForOwner(ownerThread) != 0;
    }
    void postMessage(const GuestMessage& message) { postBack(message, MessageQueueKind::Posted); }
    void postMessage(GuestMessage&& message) { postBack(message, MessageQueueKind::Posted); }
    void postPostedMessage(const GuestMessage& message) { postBack(message, MessageQueueKind::Posted); }
    void postInputMessage(const GuestMessage& message) { postBack(message, MessageQueueKind::Input); }
    void postThreadMessage(const GuestMessage& message, uint32_t ownerThread = kNoOwnerThread) {
        postBack(message, MessageQueueKind::Thread, ownerThread);
    }
    void postTimerMessage(const GuestMessage& message) { postBack(message, MessageQueueKind::Timer); }
    void postFront(const GuestMessage& message) { postAtFront(message, message.queueKind); }

    template <typename Predicate>
    bool anyMessage(Predicate predicate) const {
        for (const auto& message : messages_) {
            if (predicate(message)) return true;
        }
        return false;
    }

    template <typename Predicate>
    void postAfterLeadingMatches(const GuestMessage& message, Predicate predicate) {
        GuestMessage queued = prepareForQueue(message, message.queueKind);
        auto it = messages_.begin();
        while (it != messages_.end() && predicate(*it)) {
            ++it;
        }
        addToOwnerQueue(queued);
        messages_.insert(it, queued);
    }

    template <typename Predicate>
    void postBeforeFirstMatch(const GuestMessage& message, Predicate predicate) {
        GuestMessage queued = prepareForQueue(message, message.queueKind);
        auto it = messages_.begin();
        while (it != messages_.end() && !predicate(*it)) {
            ++it;
        }
        addToOwnerQueue(queued);
        messages_.insert(it, queued);
    }

    template <typename Predicate>
    void postSentBeforeFirstMatch(const GuestMessage& message, Predicate predicate) {
        GuestMessage queued = prepareForQueue(message, MessageQueueKind::Sent);
        auto it = messages_.begin();
        while (it != messages_.end() && !predicate(*it)) {
            ++it;
        }
        addToOwnerQueue(queued);
        messages_.insert(it, queued);
    }

    template <typename Predicate>
    size_t eraseIf(Predicate predicate) {
        const size_t oldSize = messages_.size();
        for (auto it = messages_.begin(); it != messages_.end();) {
            if (predicate(*it)) {
                removeFromOwnerQueue(*it);
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
                removeFromOwnerQueue(*it);
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
                removeFromOwnerQueue(*eraseIt);
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
            if (remove) {
                removeFromOwnerQueue(message);
                messages_.erase(it);
            }
            return message;
        }
        return std::nullopt;
    }

    template <typename Predicate>
    std::optional<GuestMessage> firstMatchingForOwner(uint32_t ownerThread,
                                                      Predicate predicate,
                                                      bool remove) {
        return firstMatching([&](const GuestMessage& message) {
            if (ownerThread != kNoOwnerThread && ownerForMessage(message) != ownerThread) {
                return false;
            }
            return predicate(message);
        }, remove);
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
    std::deque<GuestMessage>& queueFor(ThreadQueue& queue, MessageQueueKind kind) {
        switch (kind) {
        case MessageQueueKind::Sent:
            return queue.sent;
        case MessageQueueKind::Input:
            return queue.input;
        case MessageQueueKind::Timer:
            return queue.timers;
        case MessageQueueKind::Thread:
            return queue.thread;
        case MessageQueueKind::Posted:
        default:
            return queue.posted;
        }
    }

    GuestMessage prepareForQueue(const GuestMessage& message,
                                 MessageQueueKind kind,
                                 uint32_t ownerThread = kNoOwnerThread) {
        GuestMessage queued = message;
        if (!queued.queueId) {
            queued.queueId = nextMessageId_++;
        }
        queued.queueKind = kind;
        if (ownerThread) {
            messageOwners_[queued.queueId] = ownerThread;
        } else {
            const uint32_t messageOwner = ownerForMessage(queued);
            if (messageOwner != kNoOwnerThread) {
                messageOwners_[queued.queueId] = messageOwner;
            }
        }
        return queued;
    }

    void postBack(const GuestMessage& message,
                  MessageQueueKind kind,
                  uint32_t ownerThread = kNoOwnerThread) {
        GuestMessage queued = prepareForQueue(message, kind, ownerThread);
        addToOwnerQueue(queued);
        messages_.push_back(queued);
    }

    void postAtFront(const GuestMessage& message, MessageQueueKind kind) {
        GuestMessage queued = prepareForQueue(message, kind);
        addToOwnerQueue(queued);
        messages_.push_front(queued);
    }

    void addToOwnerQueue(const GuestMessage& message) {
        const auto owner = messageOwners_.find(message.queueId);
        if (owner == messageOwners_.end() || !owner->second) return;
        ensureThreadQueue(owner->second);
        queueFor(threadQueues_[owner->second], message.queueKind).push_back(message);
    }

    void removeFromOwnerQueue(const GuestMessage& message) {
        const auto owner = messageOwners_.find(message.queueId);
        if (owner == messageOwners_.end()) return;
        auto queue = threadQueues_.find(owner->second);
        if (queue != threadQueues_.end()) {
            auto& lane = queueFor(queue->second, message.queueKind);
            for (auto it = lane.begin(); it != lane.end(); ++it) {
                if (it->queueId == message.queueId) {
                    lane.erase(it);
                    break;
                }
            }
        }
        messageOwners_.erase(owner);
    }

    std::deque<GuestMessage> messages_;
    std::map<uint32_t, ThreadQueue> threadQueues_;
    std::map<uint32_t, uint32_t> windowOwners_;
    std::map<uint32_t, WindowRegionState> windowRegions_;
    std::map<uint64_t, uint32_t> messageOwners_;
    uint64_t nextMessageId_{kFirstMessageId};
};
