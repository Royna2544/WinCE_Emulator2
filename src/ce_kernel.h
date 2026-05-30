#pragma once

#include <cstdint>
#include <map>
#include <string_view>

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

    static constexpr std::string_view name() noexcept { return "CE virtual kernel"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for CE handles, processes, threads, waits, and kernel-call dispatch.";
    }

    uint32_t makeHandle(GuestHandle handle);
    GuestHandle* lookupHandle(uint32_t guestHandle);
    const GuestHandle* lookupHandle(uint32_t guestHandle) const;
    bool eraseHandle(uint32_t guestHandle);
    bool containsHandle(uint32_t guestHandle) const;

    std::map<uint32_t, GuestHandle>& handles() noexcept { return guestHandles_; }
    const std::map<uint32_t, GuestHandle>& handles() const noexcept { return guestHandles_; }

private:
    uint32_t nextHandle_{0x10000};
    std::map<uint32_t, GuestHandle> guestHandles_;
};
