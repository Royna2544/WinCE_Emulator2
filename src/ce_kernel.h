#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CeKernel {
public:
    struct GuestHandle {
        enum class Kind {
            HostFile,
            HostFind,
            HostCrtFile,
            HostWaveIn,
            HostWaveOut,
            HostEvent,
            HostMutex,
            HostMenu,
            HostAccelerator,
            HostIcon,
            HostCursor,
            HostBitmap,
            HostRegion,
            HostSocket,
            HostComInterface,
            HostSerialDevice,
            GuestFileMapping,
            GuestPropertySheetPage,
            GuestHeap,
            GuestResource,
            GuestRegistryKey,
            GuestProcess,
            GuestThread,
            GuestSerialDevice,
            GuestFind,
            GuestWindow,
            GuestDc,
            GuestBrush,
            GuestPen,
            GuestFont,
        };

        Kind kind{Kind::GuestHeap};
        uintptr_t hostValue{};
        uint32_t filePointer{};
    };

    struct GuestCpuContext {
        std::map<int, uint32_t> registers;
        bool valid{};
    };

    enum class GuestThreadRunState {
        Suspended,
        Runnable,
        Running,
        Waiting,
        WaitingForMessage,
        WaitingForSendMessage,
        WaitingForSerialRead,
        Terminated,
    };

    struct GuestThreadState {
        uint32_t handle{};
        uint32_t threadId{};
        uint32_t startAddress{};
        uint32_t parameter{};
        uint32_t stackBase{};
        uint32_t stackSize{};
        uint32_t tlsBase{};
        uint32_t suspendCount{};
        uint32_t exitCode{};
        uint32_t processHandle{};
        uint32_t processId{};
        uint32_t moduleBase{};
        std::string modulePath;
        uint32_t waitHandle{};
        uint64_t sleepUntilMs{};
        uint32_t waitTimeoutResult{};
        std::vector<uint32_t> waitHandles;
        bool waitAll{};
        bool waitForMessages{};
        uint32_t waitWakeMask{};
        GuestThreadRunState state{GuestThreadRunState::Suspended};
        GuestCpuContext context;
    };

    struct HostWaitResult {
        bool ready{};
        bool failed{};
        uint32_t error{};
    };

    enum class EncodedKernelCallKind {
        Unknown,
        TerminateProcess,
    };

    struct EncodedKernelCall {
        EncodedKernelCallKind kind{EncodedKernelCallKind::Unknown};
        uint32_t target{};
        uint32_t apiSet{};
        uint32_t method{};
        bool oldEncoding{};
    };

    static constexpr uint32_t kWaitObject0 = 0x00000000u;
    static constexpr uint32_t kWaitTimeout = 0x00000102u;
    static constexpr uint32_t kWaitFailed = 0xffffffffu;

    struct WaitQueryResult {
        uint32_t result{kWaitFailed};
        uint32_t error{};
        uint32_t preferredThread{};
    };

    enum class WaitRefreshKind {
        SleepSatisfied,
        InvalidHandle,
        WaitFailed,
        WaitSatisfied,
        WaitAllSatisfied,
        QueueEventSatisfied,
        MessageWaitSatisfied,
    };

    struct WaitRefreshEvent {
        WaitRefreshKind kind{};
        uint32_t threadHandle{};
        uint32_t waitHandle{};
        uint32_t error{};
        size_t index{};
        size_t count{};
    };

    using HostWaitProbe = std::function<HostWaitResult(const GuestHandle&)>;
    using MessageWaitProbe = std::function<bool(uint32_t threadHandle)>;

    static constexpr std::string_view name() noexcept { return "CE virtual kernel"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for CE handles, processes, threads, waits, and kernel-call dispatch.";
    }

    uint32_t makeHandle(GuestHandle handle);
    GuestHandle* lookupHandle(uint32_t guestHandle);
    const GuestHandle* lookupHandle(uint32_t guestHandle) const;
    bool eraseHandle(uint32_t guestHandle);
    bool containsHandle(uint32_t guestHandle) const;
    bool hasRunnableThread() const;
    std::vector<uint32_t> wakeThreadsWaitingForMessage(const MessageWaitProbe& hasMessagesForThread);
    WaitQueryResult queryWaitObject(uint32_t guestHandle,
                                    const HostWaitProbe& hostWaitProbe,
                                    bool failOnHostError) const;
    WaitQueryResult queryWaitObjects(const std::vector<uint32_t>& guestHandles,
                                     bool waitAll,
                                     const HostWaitProbe& hostWaitProbe,
                                     bool failOnHostError) const;
    std::vector<WaitRefreshEvent> refreshSignaledWaits(
        uint64_t nowMs,
        int resultRegister,
        const HostWaitProbe& hostWaitProbe,
        const MessageWaitProbe& hasMessagesForThread = MessageWaitProbe{});
    static std::optional<EncodedKernelCall> decodeMipsKernelCall(uint32_t target);
    void terminateCurrentProcess(uint32_t exitCode);
    bool processTerminated() const noexcept { return processTerminated_; }
    uint32_t processExitCode() const noexcept { return processExitCode_; }

    std::map<uint32_t, GuestHandle>& handles() noexcept { return guestHandles_; }
    const std::map<uint32_t, GuestHandle>& handles() const noexcept { return guestHandles_; }
    std::map<uint32_t, GuestThreadState>& threads() noexcept { return guestThreads_; }
    const std::map<uint32_t, GuestThreadState>& threads() const noexcept { return guestThreads_; }
    GuestCpuContext& mainThreadContext() noexcept { return mainThreadContext_; }
    const GuestCpuContext& mainThreadContext() const noexcept { return mainThreadContext_; }
    uint32_t& activeGuestThread() noexcept { return activeGuestThread_; }
    uint32_t activeGuestThread() const noexcept { return activeGuestThread_; }
    uint32_t& lastScheduledGuestThread() noexcept { return lastScheduledGuestThread_; }
    uint32_t lastScheduledGuestThread() const noexcept { return lastScheduledGuestThread_; }
    uint32_t& nextGuestThreadId() noexcept { return nextGuestThreadId_; }
    uint32_t& mainThreadPseudoHandle() noexcept { return mainThreadPseudoHandle_; }
    uint32_t mainThreadPseudoHandle() const noexcept { return mainThreadPseudoHandle_; }
    uint32_t& mainProcessPseudoHandle() noexcept { return mainProcessPseudoHandle_; }
    uint32_t mainProcessPseudoHandle() const noexcept { return mainProcessPseudoHandle_; }
    uint32_t& mainProcessId() noexcept { return mainProcessId_; }
    uint32_t mainProcessId() const noexcept { return mainProcessId_; }
    uint32_t& nextGuestProcessId() noexcept { return nextGuestProcessId_; }
    uint32_t& mainThreadTls() noexcept { return mainThreadTls_; }
    uint32_t mainThreadTls() const noexcept { return mainThreadTls_; }

private:
    uint32_t nextHandle_{0x10000};
    std::map<uint32_t, GuestHandle> guestHandles_;
    std::map<uint32_t, GuestThreadState> guestThreads_;
    GuestCpuContext mainThreadContext_;
    uint32_t activeGuestThread_{};
    uint32_t lastScheduledGuestThread_{};
    uint32_t nextGuestThreadId_{1};
    uint32_t mainThreadPseudoHandle_{0xfffffffeu};
    uint32_t mainProcessPseudoHandle_{0xffffffffu};
    uint32_t mainProcessId_{1};
    uint32_t nextGuestProcessId_{2};
    uint32_t mainThreadTls_{};
    bool processTerminated_{};
    uint32_t processExitCode_{};
};
