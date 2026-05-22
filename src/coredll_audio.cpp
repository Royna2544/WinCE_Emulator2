#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <mmsystem.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <array>

namespace {
struct WinmmBridge {
    HMODULE module{};
    decltype(&waveInOpen) waveInOpen{};
    decltype(&waveInAddBuffer) waveInAddBuffer{};
    decltype(&waveInUnprepareHeader) waveInUnprepareHeader{};
    decltype(&waveInReset) waveInReset{};
    decltype(&waveInClose) waveInClose{};
    decltype(&waveInGetID) waveInGetID{};
    decltype(&waveInMessage) waveInMessage{};
    decltype(&waveOutGetNumDevs) waveOutGetNumDevs{};
    decltype(&waveOutOpen) waveOutOpen{};
    decltype(&waveOutClose) waveOutClose{};
    decltype(&waveOutSetVolume) waveOutSetVolume{};
    decltype(&waveOutReset) waveOutReset{};
    decltype(&waveOutPrepareHeader) waveOutPrepareHeader{};
    decltype(&waveOutUnprepareHeader) waveOutUnprepareHeader{};
    decltype(&waveOutWrite) waveOutWrite{};
    decltype(&mixerGetControlDetailsW) mixerGetControlDetailsW{};
    bool attempted{};
};

WinmmBridge& winmmBridge() {
    static WinmmBridge bridge;
    if (bridge.attempted) return bridge;
    bridge.attempted = true;
    bridge.module = LoadLibraryW(L"winmm.dll");
    if (!bridge.module) return bridge;
    bridge.waveInOpen = reinterpret_cast<decltype(bridge.waveInOpen)>(GetProcAddress(bridge.module, "waveInOpen"));
    bridge.waveInAddBuffer = reinterpret_cast<decltype(bridge.waveInAddBuffer)>(GetProcAddress(bridge.module, "waveInAddBuffer"));
    bridge.waveInUnprepareHeader = reinterpret_cast<decltype(bridge.waveInUnprepareHeader)>(GetProcAddress(bridge.module, "waveInUnprepareHeader"));
    bridge.waveInReset = reinterpret_cast<decltype(bridge.waveInReset)>(GetProcAddress(bridge.module, "waveInReset"));
    bridge.waveInClose = reinterpret_cast<decltype(bridge.waveInClose)>(GetProcAddress(bridge.module, "waveInClose"));
    bridge.waveInGetID = reinterpret_cast<decltype(bridge.waveInGetID)>(GetProcAddress(bridge.module, "waveInGetID"));
    bridge.waveInMessage = reinterpret_cast<decltype(bridge.waveInMessage)>(GetProcAddress(bridge.module, "waveInMessage"));
    bridge.waveOutGetNumDevs = reinterpret_cast<decltype(bridge.waveOutGetNumDevs)>(GetProcAddress(bridge.module, "waveOutGetNumDevs"));
    bridge.waveOutOpen = reinterpret_cast<decltype(bridge.waveOutOpen)>(GetProcAddress(bridge.module, "waveOutOpen"));
    bridge.waveOutClose = reinterpret_cast<decltype(bridge.waveOutClose)>(GetProcAddress(bridge.module, "waveOutClose"));
    bridge.waveOutSetVolume = reinterpret_cast<decltype(bridge.waveOutSetVolume)>(GetProcAddress(bridge.module, "waveOutSetVolume"));
    bridge.waveOutReset = reinterpret_cast<decltype(bridge.waveOutReset)>(GetProcAddress(bridge.module, "waveOutReset"));
    bridge.waveOutPrepareHeader = reinterpret_cast<decltype(bridge.waveOutPrepareHeader)>(GetProcAddress(bridge.module, "waveOutPrepareHeader"));
    bridge.waveOutUnprepareHeader = reinterpret_cast<decltype(bridge.waveOutUnprepareHeader)>(GetProcAddress(bridge.module, "waveOutUnprepareHeader"));
    bridge.waveOutWrite = reinterpret_cast<decltype(bridge.waveOutWrite)>(GetProcAddress(bridge.module, "waveOutWrite"));
    bridge.mixerGetControlDetailsW = reinterpret_cast<decltype(bridge.mixerGetControlDetailsW)>(GetProcAddress(bridge.module, "mixerGetControlDetailsW"));
    return bridge;
}
}

void SyntheticDllRuntime::registerCoredllAudioExports(SyntheticModule& module) {
    struct CoreDllAudio {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.audio",
                {
                    {0x017B, {"waveOutGetNumDevs", Code::CoreDllWaveOutGetNumDevs, &SyntheticDllRuntime::handleWaveOutGetNumDevs}},
                    {0x017E, {"waveOutSetVolume", Code::CoreDllWaveOutSetVolume, &SyntheticDllRuntime::handleWaveOutSetVolume}},
                    {0x0180, {"waveOutClose", Code::CoreDllWaveOutClose, &SyntheticDllRuntime::handleWaveOutClose}},
                    {0x0181, {"waveOutPrepareHeader", Code::CoreDllWaveOutPrepareHeader, &SyntheticDllRuntime::handleWaveOutPrepareHeader}},
                    {0x0182, {"waveOutUnprepareHeader", Code::CoreDllWaveOutUnprepareHeader, &SyntheticDllRuntime::handleWaveOutUnprepareHeader}},
                    {0x0183, {"waveOutWrite", Code::CoreDllWaveOutWrite, &SyntheticDllRuntime::handleWaveOutWrite}},
                    {0x0186, {"waveOutReset", Code::CoreDllWaveOutReset, &SyntheticDllRuntime::handleWaveOutReset}},
                    {0x018F, {"waveOutOpen", Code::CoreDllWaveOutOpen, &SyntheticDllRuntime::handleWaveOutOpen}},
                    {0x0193, {"waveInClose", Code::CoreDllWaveInClose, &SyntheticDllRuntime::handleWaveInClose}},
                    {0x0195, {"waveInUnprepareHeader", Code::CoreDllWaveInUnprepareHeader, &SyntheticDllRuntime::handleWaveInUnprepareHeader}},
                    {0x0196, {"waveInAddBuffer", Code::CoreDllWaveInAddBuffer, &SyntheticDllRuntime::handleWaveInAddBuffer}},
                    {0x0199, {"waveInReset", Code::CoreDllWaveInReset, &SyntheticDllRuntime::handleWaveInReset}},
                    {0x019B, {"waveInGetID", Code::CoreDllWaveInGetID, &SyntheticDllRuntime::handleWaveInGetID}},
                    {0x019C, {"waveInMessage", Code::CoreDllWaveInMessage, &SyntheticDllRuntime::handleWaveInMessage}},
                    {0x019D, {"waveInOpen", Code::CoreDllWaveInOpen, &SyntheticDllRuntime::handleWaveInOpen}},
                    {0x0635, {"mixerGetControlDetails", Code::CoreDllMixerGetControlDetails, &SyntheticDllRuntime::handleMixerGetControlDetails}},
                },
            };
        }
    };

    const CoreDllAudio audio;
    registerHandlers(module, audio.group());
}

bool SyntheticDllRuntime::handleWaveInOpen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto& winmm = winmmBridge();
    if (!winmm.waveInOpen || !args.a0 || !args.a2) {
        ret = MMSYSERR_ERROR;
        return true;
    }
    WAVEFORMATEX format{};
    if (uc_mem_read(uc_, args.a2, &format, sizeof(format)) != UC_ERR_OK) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    HWAVEIN host{};
    ret = winmm.waveInOpen(&host, args.a1, &format, 0, 0, CALLBACK_NULL);
    if (ret == MMSYSERR_NOERROR) {
        const uint32_t guest = makeGuestHandle({GuestHandle::Kind::HostWaveIn, reinterpret_cast<uintptr_t>(host), 0});
        writeU32(args.a0, guest);
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveInClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (handle && handle->kind == GuestHandle::Kind::HostWaveIn && handle->hostValue && winmm.waveInClose) {
        ret = winmm.waveInClose(reinterpret_cast<HWAVEIN>(handle->hostValue));
        if (ret == MMSYSERR_NOERROR) handle->hostValue = 0;
    } else {
        ret = MMSYSERR_INVALHANDLE;
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveInReset(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    ret = handle && handle->kind == GuestHandle::Kind::HostWaveIn && handle->hostValue && winmm.waveInReset
        ? winmm.waveInReset(reinterpret_cast<HWAVEIN>(handle->hostValue))
        : MMSYSERR_INVALHANDLE;
    return true;
}

bool SyntheticDllRuntime::handleWaveInGetID(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (!args.a1) {
        ret = MMSYSERR_INVALPARAM;
    } else if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !winmm.waveInGetID) {
        ret = MMSYSERR_INVALHANDLE;
    } else {
        UINT id = 0;
        ret = winmm.waveInGetID(reinterpret_cast<HWAVEIN>(handle->hostValue), &id);
        if (ret == MMSYSERR_NOERROR) writeU32(args.a1, id);
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveInAddBuffer(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !args.a1) {
        ret = MMSYSERR_INVALHANDLE;
        return true;
    }
    uint32_t guestData = readU32(args.a1);
    uint32_t guestLength = readU32(args.a1 + 4);
    uint32_t guestBytesRecorded = readU32(args.a1 + 8);
    uint32_t guestUser = readU32(args.a1 + 12);
    uint32_t guestFlags = readU32(args.a1 + 16);
    uint32_t guestLoops = readU32(args.a1 + 20);
    if (!winmm.waveInAddBuffer || !guestData || !guestLength || guestLength > 0x100000) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    auto& stored = hostWaveBuffers_[args.a1];
    stored.data.assign(guestLength, 0);
    uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size());
    auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
    *header = {};
    header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
    header->dwBufferLength = guestLength;
    header->dwBytesRecorded = guestBytesRecorded;
    header->dwUser = guestUser;
    header->dwFlags = guestFlags;
    header->dwLoops = guestLoops;
    ret = winmm.waveInAddBuffer(reinterpret_cast<HWAVEIN>(handle->hostValue), header, sizeof(*header));
    writeU32(args.a1 + 8, header->dwBytesRecorded);
    writeU32(args.a1 + 16, header->dwFlags);
    return true;
}

bool SyntheticDllRuntime::handleWaveInUnprepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !args.a1) {
        ret = MMSYSERR_INVALHANDLE;
        return true;
    }
    auto it = hostWaveBuffers_.find(args.a1);
    if (it == hostWaveBuffers_.end() || !winmm.waveInUnprepareHeader) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    auto* header = reinterpret_cast<WAVEHDR*>(it->second.header.data());
    ret = winmm.waveInUnprepareHeader(reinterpret_cast<HWAVEIN>(handle->hostValue), header, sizeof(*header));
    const uint32_t guestData = readU32(args.a1);
    if (guestData && header->dwBytesRecorded) {
        uc_mem_write(uc_, guestData, it->second.data.data(),
                     std::min<size_t>(it->second.data.size(), header->dwBytesRecorded));
    }
    writeU32(args.a1 + 8, header->dwBytesRecorded);
    writeU32(args.a1 + 16, header->dwFlags);
    hostWaveBuffers_.erase(it);
    return true;
}

bool SyntheticDllRuntime::handleWaveInMessage(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    ret = handle && handle->kind == GuestHandle::Kind::HostWaveIn && handle->hostValue && winmm.waveInMessage
        ? winmm.waveInMessage(reinterpret_cast<HWAVEIN>(handle->hostValue), args.a1, args.a2, args.a3)
        : MMSYSERR_INVALHANDLE;
    return true;
}

bool SyntheticDllRuntime::handleWaveOutGetNumDevs(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    auto& winmm = winmmBridge();
    ret = winmm.waveOutGetNumDevs ? winmm.waveOutGetNumDevs() : 0;
    return true;
}

bool SyntheticDllRuntime::handleWaveOutOpen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto& winmm = winmmBridge();
    if (!winmm.waveOutOpen || !args.a0 || !args.a2) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    WAVEFORMATEX format{};
    if (uc_mem_read(uc_, args.a2, &format, sizeof(format)) != UC_ERR_OK) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    HWAVEOUT host{};
    const DWORD_PTR instance = DWORD_PTR(stackArg(4));
    const DWORD flags = stackArg(5);
    const DWORD callbackFlags = flags & CALLBACK_TYPEMASK;
    const DWORD hostFlags = callbackFlags == CALLBACK_NULL ? flags : ((flags & ~CALLBACK_TYPEMASK) | CALLBACK_NULL);
    ret = winmm.waveOutOpen(&host, args.a1, &format, 0, instance, hostFlags);
    if (ret == MMSYSERR_NOERROR) {
        const uint32_t guest = makeGuestHandle({GuestHandle::Kind::HostWaveOut, reinterpret_cast<uintptr_t>(host), 0});
        waveOutStates_[guest] = {args.a3, uint32_t(instance), flags};
        writeU32(args.a0, guest);
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveOutClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutClose) {
        ret = winmm.waveOutClose(reinterpret_cast<HWAVEOUT>(handle->hostValue));
        if (ret == MMSYSERR_NOERROR) {
            handle->hostValue = 0;
            waveOutStates_.erase(args.a0);
        }
    } else {
        ret = MMSYSERR_INVALHANDLE;
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveOutSetVolume(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    ret = handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutSetVolume
        ? winmm.waveOutSetVolume(reinterpret_cast<HWAVEOUT>(handle->hostValue), args.a1)
        : MMSYSERR_INVALHANDLE;
    return true;
}

bool SyntheticDllRuntime::handleWaveOutReset(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    ret = handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutReset
        ? winmm.waveOutReset(reinterpret_cast<HWAVEOUT>(handle->hostValue))
        : MMSYSERR_INVALHANDLE;
    return true;
}

bool SyntheticDllRuntime::handleWaveOutPrepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveOut || !handle->hostValue || !args.a1 || args.a2 < 32) {
        ret = MMSYSERR_INVALHANDLE;
        return true;
    }
    const uint32_t guestData = readU32(args.a1);
    const uint32_t guestLength = readU32(args.a1 + 4);
    if (!guestData || !guestLength || guestLength > 0x400000 || !winmm.waveOutPrepareHeader) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    auto& stored = hostWaveBuffers_[args.a1];
    if (stored.data.size() != guestLength) stored.data.assign(guestLength, 0);
    uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size());
    auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
    *header = {};
    header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
    header->dwBufferLength = guestLength;
    header->dwBytesRecorded = readU32(args.a1 + 8);
    header->dwUser = readU32(args.a1 + 12);
    header->dwFlags = readU32(args.a1 + 16);
    header->dwLoops = readU32(args.a1 + 20);
    ret = winmm.waveOutPrepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
    writeU32(args.a1 + 16, uint32_t(header->dwFlags));
    return true;
}

bool SyntheticDllRuntime::handleWaveOutUnprepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    auto it = hostWaveBuffers_.find(args.a1);
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveOut || !handle->hostValue || it == hostWaveBuffers_.end() ||
        !winmm.waveOutUnprepareHeader) {
        ret = MMSYSERR_INVALHANDLE;
        return true;
    }
    auto* header = reinterpret_cast<WAVEHDR*>(it->second.header.data());
    ret = winmm.waveOutUnprepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
    writeU32(args.a1 + 16, uint32_t(header->dwFlags));
    if (ret == MMSYSERR_NOERROR) hostWaveBuffers_.erase(it);
    return true;
}

bool SyntheticDllRuntime::handleWaveOutWrite(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveOut || !handle->hostValue || !args.a1 || args.a2 < 32) {
        ret = MMSYSERR_INVALHANDLE;
        return true;
    }
    const uint32_t guestData = readU32(args.a1);
    const uint32_t guestLength = readU32(args.a1 + 4);
    if (!guestData || !guestLength || guestLength > 0x400000) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    auto& stored = hostWaveBuffers_[args.a1];
    if (stored.data.size() != guestLength) stored.data.assign(guestLength, 0);
    uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size());
    auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
    *header = {};
    header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
    header->dwBufferLength = guestLength;
    header->dwBytesRecorded = readU32(args.a1 + 8);
    header->dwUser = readU32(args.a1 + 12);
    header->dwFlags = readU32(args.a1 + 16);
    header->dwLoops = readU32(args.a1 + 20);
    if (winmm.waveOutPrepareHeader && !(header->dwFlags & WHDR_PREPARED)) {
        winmm.waveOutPrepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
    }
    ret = winmm.waveOutWrite
        ? winmm.waveOutWrite(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header))
        : MMSYSERR_ERROR;
    if (ret == MMSYSERR_NOERROR) {
        header->dwFlags |= WHDR_DONE;
        auto state = waveOutStates_.find(args.a0);
        if (state != waveOutStates_.end()) {
            const uint32_t callbackType = state->second.flags & CALLBACK_TYPEMASK;
            const uint32_t eventHandle = callbackType == CALLBACK_EVENT
                ? state->second.callback
                : uint32_t(header->dwUser);
            auto* event = lookupGuestHandle(eventHandle);
            if (event && event->kind == GuestHandle::Kind::HostEvent && event->hostValue) {
                SetEvent(reinterpret_cast<HANDLE>(event->hostValue));
            }
        }
    }
    writeU32(args.a1 + 16, uint32_t(header->dwFlags));
    return true;
}

bool SyntheticDllRuntime::handleMixerGetControlDetails(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto& winmm = winmmBridge();
    ret = winmm.mixerGetControlDetailsW && args.a1 ? MMSYSERR_INVALPARAM : MMSYSERR_ERROR;
    return true;
}
