#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <array>

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

    struct GuestWindowClass {
        std::array<uint8_t, 40> bytes{};
        std::string name;
        uint16_t atom{};
    };

    struct GuestWindow {
        uint32_t hwnd{};
        std::string className;
        std::string title;
        uint32_t style{};
        uint32_t exStyle{};
        uint32_t parent{};
        uint32_t menu{};
        uint32_t instance{};
        uint32_t param{};
        uint32_t wndProc{};
        uint32_t userData{};
        uint32_t createStruct{};
        uint32_t ownerThread{};
        uint64_t zOrder{};
        int32_t x{};
        int32_t y{};
        int32_t width{800};
        int32_t height{480};
        uintptr_t hostHwnd{};
        bool visible{};
        bool enabled{true};
        bool destroyed{};
        bool externalProcess{};
        uint32_t externalProcessId{};
        uint32_t externalHwnd{};
        bool paintBoundsValid{};
        int32_t paintLeft{};
        int32_t paintTop{};
        int32_t paintRight{};
        int32_t paintBottom{};
        bool backingValid{};
        int32_t backingX{};
        int32_t backingY{};
        int32_t backingWidth{};
        int32_t backingHeight{};
        std::vector<uint32_t> backingPixels;
        std::map<int32_t, uint32_t> extraLongs;
    };

    struct GuestTimer {
        uint32_t hwnd{};
        uint32_t id{};
        uint32_t intervalMs{};
        uint32_t callback{};
        uint64_t nextDueMs{};
    };

    struct PendingDestroyWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t originalGp{};
        uint32_t stage{};
        uint32_t parent{};
        bool wasVisible{};
    };

    struct PendingCreateWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t originalGp{};
        uint32_t createStruct{};
        uint32_t stage{};
    };

    struct PendingUpdateWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t originalGp{};
        uint32_t eraseDc{};
        uint32_t stage{};
        bool deferredHostPresent{};
        std::string sourceName;
    };

    struct PendingMessageTransfer {
        uint32_t hwnd{};
        uint32_t message{};
        uint32_t originalRa{};
        uint32_t originalGp{};
        uint32_t outerReturnRa{};
        uint32_t synchronousSender{};
        uint32_t ownerThread{};
        uint32_t startPc{};
        uint64_t startedMs{};
        size_t queuedAtStart{};
        bool releaseHostPresentAfterPaint{};
        std::string sourceName;
    };

    struct ThreadQueue {
        uint32_t ownerThread{};
        std::deque<GuestMessage> posted;
        std::deque<GuestMessage> sent;
        std::deque<GuestMessage> input;
        std::deque<GuestMessage> timers;
        std::deque<GuestMessage> thread;
    };

    struct OwnerQueueSnapshot {
        uint32_t ownerThread{};
        size_t posted{};
        size_t sent{};
        size_t input{};
        size_t timers{};
        size_t thread{};
        size_t total{};
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
        uint32_t owner{};
        uint32_t root{};
        uint32_t style{};
        uint32_t exStyle{};
        uint64_t zOrder{};
        Rect windowRect;
        Rect clientRect;
        Rect visibleRect;
        Rect updateRect;
        Rect clientVisibleRect;
        Rect clientUpdateRect;
        bool visible{};
        bool enabled{true};
        bool destroyed{};
        bool hasVisibleRegion{};
        bool hasUpdateRegion{};
        bool hasClientVisibleRegion{};
        bool hasClientUpdateRegion{};
    };

    struct HitTestResult {
        uint32_t hwnd{};
        uint32_t blocker{};
        int32_t clientX{};
        int32_t clientY{};
        bool blockedByModal{};
    };

    static constexpr std::string_view name() noexcept { return "CE GWE"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for GWE message queues, windows, input, timers, and paint regions.";
    }

    std::deque<GuestMessage>& messages() noexcept { return messages_; }
    const std::deque<GuestMessage>& messages() const noexcept { return messages_; }
    std::map<std::string, GuestWindowClass>& windowClassesByName() noexcept { return windowClassesByName_; }
    const std::map<std::string, GuestWindowClass>& windowClassesByName() const noexcept { return windowClassesByName_; }
    std::map<uint16_t, std::string>& windowClassNamesByAtom() noexcept { return windowClassNamesByAtom_; }
    const std::map<uint16_t, std::string>& windowClassNamesByAtom() const noexcept { return windowClassNamesByAtom_; }
    std::map<uint32_t, GuestWindow>& windows() noexcept { return windows_; }
    const std::map<uint32_t, GuestWindow>& windows() const noexcept { return windows_; }
    std::map<uint64_t, GuestTimer>& timers() noexcept { return timers_; }
    const std::map<uint64_t, GuestTimer>& timers() const noexcept { return timers_; }
    std::vector<PendingDestroyWindow>& pendingDestroyWindows() noexcept { return pendingDestroyWindows_; }
    const std::vector<PendingDestroyWindow>& pendingDestroyWindows() const noexcept { return pendingDestroyWindows_; }
    std::vector<PendingCreateWindow>& pendingCreateWindows() noexcept { return pendingCreateWindows_; }
    const std::vector<PendingCreateWindow>& pendingCreateWindows() const noexcept { return pendingCreateWindows_; }
    std::vector<PendingUpdateWindow>& pendingUpdateWindows() noexcept { return pendingUpdateWindows_; }
    const std::vector<PendingUpdateWindow>& pendingUpdateWindows() const noexcept { return pendingUpdateWindows_; }
    std::vector<PendingMessageTransfer>& pendingMessageTransfers() noexcept { return pendingMessageTransfers_; }
    const std::vector<PendingMessageTransfer>& pendingMessageTransfers() const noexcept { return pendingMessageTransfers_; }
    std::map<uint32_t, uint32_t>& retrievedSyncSendersByMsgPtr() noexcept {
        return retrievedSyncSendersByMsgPtr_;
    }
    const std::map<uint32_t, uint32_t>& retrievedSyncSendersByMsgPtr() const noexcept {
        return retrievedSyncSendersByMsgPtr_;
    }
    uint32_t& lastMessagePos() noexcept { return lastMessagePos_; }
    uint32_t lastMessagePos() const noexcept { return lastMessagePos_; }
    uint32_t& lastMessageTime() noexcept { return lastMessageTime_; }
    uint32_t lastMessageTime() const noexcept { return lastMessageTime_; }
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
                           uint32_t owner,
                           uint32_t root,
                           uint32_t style,
                           uint32_t exStyle,
                           uint64_t zOrder,
                           int32_t x,
                           int32_t y,
                           int32_t width,
                           int32_t height,
                           bool visible,
                           bool enabled,
                           bool destroyed) {
        if (!hwnd) return;
        registerWindowOwner(hwnd, ownerThread);
        auto& state = windowRegions_[hwnd];
        state.hwnd = hwnd;
        state.ownerThread = ownerThread;
        state.parent = parent;
        state.owner = owner;
        state.root = root ? root : hwnd;
        state.style = style;
        state.exStyle = exStyle;
        state.zOrder = zOrder;
        state.windowRect = Rect{x, y, x + width, y + height};
        state.clientRect = Rect{0, 0, width, height};
        state.visible = visible;
        state.enabled = enabled;
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
    static bool rectContainsPoint(const Rect& rect, int32_t x, int32_t y) noexcept {
        return x >= rect.left && y >= rect.top && x < rect.right && y < rect.bottom;
    }
    bool visibleRegionContainsPoint(uint32_t hwnd, int32_t x, int32_t y) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        return state && state->hasVisibleRegion && rectContainsPoint(state->visibleRect, x, y);
    }
    std::optional<Rect> visibleRectForWindow(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        if (!state || !state->hasVisibleRegion) return std::nullopt;
        return state->visibleRect;
    }
    uint32_t ownerForStack(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        return state ? state->owner : 0;
    }
    uint32_t rootForStack(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        return state ? state->root : 0;
    }
    bool isChildWindow(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        return state && (state->style & kWindowStyleChild) != 0;
    }
    bool isPopupWindow(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        return state && (state->style & kWindowStylePopup) != 0 &&
               (state->style & kWindowStyleChild) == 0;
    }
    bool isVisibleEnabled(uint32_t hwnd) const {
        const WindowRegionState* state = windowRegionState(hwnd);
        if (!state || state->destroyed || !state->visible || !state->enabled || !state->hasVisibleRegion) {
            return false;
        }
        for (uint32_t current = (state->style & kWindowStyleChild) ? state->parent : 0; current;) {
            const WindowRegionState* parentState = windowRegionState(current);
            if (!parentState) break;
            if (parentState->destroyed || !parentState->visible || !parentState->enabled ||
                !parentState->hasVisibleRegion) {
                return false;
            }
            current = (parentState->style & kWindowStyleChild) ? parentState->parent : 0;
        }
        return true;
    }
    bool isWindowInStack(uint32_t hwnd, uint32_t ancestor) const {
        if (!hwnd || !ancestor) return false;
        for (uint32_t current = hwnd; current;) {
            if (current == ancestor) return true;
            const WindowRegionState* state = windowRegionState(current);
            if (!state) break;
            current = (state->style & kWindowStyleChild) ? state->parent : state->owner;
        }
        return false;
    }
    std::vector<uint32_t> orderedWindowsTopToBottom(uint32_t root = 0) const {
        std::vector<uint32_t> ordered;
        ordered.reserve(windowRegions_.size());
        for (const auto& [hwnd, state] : windowRegions_) {
            if (state.destroyed) continue;
            if (root && state.root != root && hwnd != root && !isWindowInStack(hwnd, root)) continue;
            ordered.push_back(hwnd);
        }
        std::sort(ordered.begin(), ordered.end(), [&](uint32_t left, uint32_t right) {
            const WindowRegionState* leftState = windowRegionState(left);
            const WindowRegionState* rightState = windowRegionState(right);
            const int leftGroup = stackPriority(leftState);
            const int rightGroup = stackPriority(rightState);
            if (leftGroup != rightGroup) return leftGroup < rightGroup;
            const uint64_t leftZ = leftState ? leftState->zOrder : 0;
            const uint64_t rightZ = rightState ? rightState->zOrder : 0;
            if (leftZ != rightZ) return leftZ > rightZ;
            return left > right;
        });
        return ordered;
    }
    uint32_t activePopupForRoot(uint32_t root) const {
        for (uint32_t hwnd : orderedWindowsTopToBottom(root)) {
            const WindowRegionState* state = windowRegionState(hwnd);
            if (!state || !isVisibleEnabled(hwnd)) continue;
            if (isPopupWindow(hwnd) && state->root == root) return hwnd;
        }
        return 0;
    }
    HitTestResult hitTest(uint32_t root, int32_t x, int32_t y) const {
        HitTestResult result{};
        uint32_t effectiveRoot = root;
        const WindowRegionState* rootState = windowRegionState(effectiveRoot);
        if (!rootState || rootState->destroyed) {
            effectiveRoot = 0;
        }
        const uint32_t modalPopup = effectiveRoot ? activePopupForRoot(effectiveRoot) : 0;
        if (modalPopup && !visibleRegionContainsPoint(modalPopup, x, y)) {
            result.blocker = modalPopup;
            result.blockedByModal = true;
            const WindowRegionState* blocker = windowRegionState(modalPopup);
            if (blocker) {
                result.clientX = x - blocker->windowRect.left;
                result.clientY = y - blocker->windowRect.top;
            }
            return result;
        }
        for (uint32_t hwnd : orderedWindowsTopToBottom(effectiveRoot)) {
            if (!isVisibleEnabled(hwnd) || !visibleRegionContainsPoint(hwnd, x, y)) continue;
            if (modalPopup && !isWindowInStack(hwnd, modalPopup)) continue;
            const WindowRegionState* state = windowRegionState(hwnd);
            if (!state) continue;
            result.hwnd = hwnd;
            result.clientX = x - state->windowRect.left;
            result.clientY = y - state->windowRect.top;
            return result;
        }
        return result;
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
    OwnerQueueSnapshot ownerQueueSnapshot(uint32_t ownerThread) const noexcept {
        OwnerQueueSnapshot snapshot{};
        snapshot.ownerThread = ownerThread;
        const auto queue = threadQueues_.find(ownerThread);
        if (queue == threadQueues_.end()) return snapshot;
        snapshot.posted = queue->second.posted.size();
        snapshot.sent = queue->second.sent.size();
        snapshot.input = queue->second.input.size();
        snapshot.timers = queue->second.timers.size();
        snapshot.thread = queue->second.thread.size();
        snapshot.total = snapshot.posted + snapshot.sent + snapshot.input + snapshot.timers + snapshot.thread;
        return snapshot;
    }
    std::optional<uint32_t> oldestPendingOwner() const {
        for (const GuestMessage& message : messages_) {
            const uint32_t ownerThread = ownerForMessage(message);
            if (ownerThread != kNoOwnerThread) return ownerThread;
        }
        return std::nullopt;
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
    static constexpr uint32_t kWindowStylePopup = 0x80000000u;
    static constexpr uint32_t kWindowStyleChild = 0x40000000u;

    static int stackPriority(const WindowRegionState* state) noexcept {
        if (!state) return 4;
        if ((state->style & kWindowStyleChild) == 0 && (state->style & kWindowStylePopup)) return 0;
        if ((state->style & kWindowStyleChild) == 0 && state->owner) return 1;
        if (state->style & kWindowStyleChild) return 2;
        return 3;
    }

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
    std::map<std::string, GuestWindowClass> windowClassesByName_;
    std::map<uint16_t, std::string> windowClassNamesByAtom_;
    std::map<uint32_t, GuestWindow> windows_;
    std::map<uint64_t, GuestTimer> timers_;
    std::vector<PendingDestroyWindow> pendingDestroyWindows_;
    std::vector<PendingCreateWindow> pendingCreateWindows_;
    std::vector<PendingUpdateWindow> pendingUpdateWindows_;
    std::vector<PendingMessageTransfer> pendingMessageTransfers_;
    std::map<uint32_t, uint32_t> retrievedSyncSendersByMsgPtr_;
    uint32_t lastMessagePos_{};
    uint32_t lastMessageTime_{};
    std::map<uint32_t, ThreadQueue> threadQueues_;
    std::map<uint32_t, uint32_t> windowOwners_;
    std::map<uint32_t, WindowRegionState> windowRegions_;
    std::map<uint64_t, uint32_t> messageOwners_;
    uint64_t nextMessageId_{kFirstMessageId};
};
