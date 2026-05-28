#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <mmsystem.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <spdlog/spdlog.h>

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

struct HostWaveDoneContext {
    LONG done{};
    uint32_t guestHeader{};
    uint32_t guestEvent{};
    HANDLE hostEvent{};
};

std::atomic<uint32_t> g_waveOutDoneCallbacks{0};
std::atomic<uint64_t> g_waveOutDoneTick{0};

void CALLBACK hostWaveOutCallback(HWAVEOUT, UINT message, DWORD_PTR, DWORD_PTR param1, DWORD_PTR) {
    if (message != WOM_DONE || !param1) return;
    const auto* header = reinterpret_cast<const WAVEHDR*>(param1);
    g_waveOutDoneCallbacks.fetch_add(1, std::memory_order_relaxed);
    g_waveOutDoneTick.store(GetTickCount64(), std::memory_order_relaxed);
    auto* context = reinterpret_cast<HostWaveDoneContext*>(header->dwUser);
    if (context) {
        InterlockedExchange(&context->done, 1);
        if (context->hostEvent) {
            SetEvent(context->hostEvent);
        }
    }
}

void logPcmStats(const char* op, uint32_t guestData, const std::vector<uint8_t>& data, bool readOk) {
    uint32_t nonZeroBytes = 0;
    int peak = 0;
    uint64_t absSum = 0;
    uint32_t sampleCount = 0;

    for (uint8_t byte : data) {
        if (byte != 0) {
            ++nonZeroBytes;
        }
    }

    for (size_t i = 1; i < data.size(); i += 2) {
        const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(data[i - 1]) |
                                                     (static_cast<uint16_t>(data[i]) << 8));
        const int absValue = sample == -32768 ? 32768 : (sample < 0 ? -sample : sample);
        peak = std::max(peak, absValue);
        absSum += static_cast<uint64_t>(absValue);
        ++sampleCount;
    }

    std::array<char, 3 * 16 + 1> prefix{};
    const size_t prefixBytes = std::min<size_t>(16, data.size());
    for (size_t i = 0; i < prefixBytes; ++i) {
        std::snprintf(prefix.data() + i * 3, prefix.size() - i * 3, "%02x ", data[i]);
    }

    spdlog::debug("{} PCM readOk={} guestData=0x{:08x} bytes={} nonZeroBytes={} peak16={} avgAbs16={} first16={}",
                  op,
                  readOk,
                  guestData,
                  data.size(),
                  nonZeroBytes,
                  peak,
                  sampleCount ? absSum / sampleCount : 0,
                  prefix.data());
}

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

void SyntheticDllRuntime::closeHostWaveInHandle(uintptr_t hostValue) {
    auto& winmm = winmmBridge();
    if (winmm.waveInClose) winmm.waveInClose(reinterpret_cast<HWAVEIN>(hostValue));
}

void SyntheticDllRuntime::closeHostWaveOutHandle(uintptr_t hostValue) {
    auto& winmm = winmmBridge();
    if (winmm.waveOutClose) winmm.waveOutClose(reinterpret_cast<HWAVEOUT>(hostValue));
}

void SyntheticDllRuntime::refreshCompletedHostWaveBuffers() {
    for (auto& [guestHeader, stored] : hostWaveBuffers_) {
        auto* context = static_cast<HostWaveDoneContext*>(stored.completionContext.get());
        if (!context) continue;

        auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
        if ((header->dwFlags & WHDR_DONE) && InterlockedCompareExchange(&context->done, 1, 0) == 0) {
            // Native WinMM marked the header done before the callback path was observed.
        }

        if (InterlockedCompareExchange(&context->done, 2, 1) != 1) continue;

        header->dwFlags |= WHDR_DONE;
        header->dwFlags &= ~WHDR_INQUEUE;
        const uint32_t headerAddress = context->guestHeader ? context->guestHeader : guestHeader;
        writeU32(headerAddress + 16, uint32_t(header->dwFlags));

        if (context->guestEvent) {
            for (auto& [threadHandle, thread] : guestThreads_) {
                (void)threadHandle;
                if (thread.state == GuestThreadRunState::Waiting && thread.waitHandle == context->guestEvent) {
                    thread.state = GuestThreadRunState::Runnable;
                    thread.waitHandle = 0;
                    thread.context.registers[UC_MIPS_REG_V0] = 0;
                }
            }
        }

        spdlog::debug("waveOut completion guestHeader=0x{:08x} event=0x{:08x} flags=0x{:08x}",
                      headerAddress, context->guestEvent, uint32_t(header->dwFlags));
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
    spdlog::info("waveOutOpen format tag={} channels={} samplesPerSec={} avgBytesPerSec={} blockAlign={} bitsPerSample={} callback=0x{:08x} instance=0x{:08x} flags=0x{:08x}",
                 format.wFormatTag, format.nChannels, format.nSamplesPerSec,
                 format.nAvgBytesPerSec, format.nBlockAlign, format.wBitsPerSample,
                 args.a3, uint32_t(instance), flags);
    const DWORD callbackFlags = flags & CALLBACK_TYPEMASK;
    DWORD_PTR hostCallback = 0;
    DWORD hostFlags = flags;
    if (callbackFlags == CALLBACK_EVENT) {
        auto* event = lookupGuestHandle(args.a3);
        if (event && event->kind == GuestHandle::Kind::HostEvent && event->hostValue) {
            hostCallback = DWORD_PTR(event->hostValue);
        } else {
            hostFlags = (flags & ~CALLBACK_TYPEMASK) | CALLBACK_NULL;
        }
    } else if (callbackFlags == CALLBACK_FUNCTION) {
        hostCallback = DWORD_PTR(&hostWaveOutCallback);
        hostFlags = (flags & ~CALLBACK_TYPEMASK) | CALLBACK_FUNCTION;
    } else if (callbackFlags != CALLBACK_NULL) {
        hostFlags = (flags & ~CALLBACK_TYPEMASK) | CALLBACK_NULL;
    }
    const UINT hostDeviceId = args.a1 == 0 ? WAVE_MAPPER : UINT(args.a1);
    auto cached = std::find_if(cachedWaveOutDevices_.begin(), cachedWaveOutDevices_.end(),
                               [&](const CachedWaveOutDevice& candidate) {
                                   return candidate.hostValue &&
                                          candidate.deviceId == hostDeviceId &&
                                          candidate.hostFlags == hostFlags &&
                                          candidate.hostCallback == hostCallback &&
                                          candidate.formatTag == format.wFormatTag &&
                                          candidate.channels == format.nChannels &&
                                          candidate.samplesPerSec == format.nSamplesPerSec &&
                                          candidate.avgBytesPerSec == format.nAvgBytesPerSec &&
                                          candidate.blockAlign == format.nBlockAlign &&
                                          candidate.bitsPerSample == format.wBitsPerSample;
                               });
    const bool reused = cached != cachedWaveOutDevices_.end();
    if (reused) {
        host = reinterpret_cast<HWAVEOUT>(cached->hostValue);
        cachedWaveOutDevices_.erase(cached);
        ret = MMSYSERR_NOERROR;
    } else {
        ret = winmm.waveOutOpen(&host, hostDeviceId, &format, hostCallback, instance, hostFlags);
    }
    spdlog::info("waveOutOpen hostDevice={} hostFlags=0x{:08x} reused={} -> {}",
                 hostDeviceId, hostFlags, reused, ret);
    if (ret == MMSYSERR_NOERROR) {
        const uint32_t guest = makeGuestHandle({GuestHandle::Kind::HostWaveOut, reinterpret_cast<uintptr_t>(host), 0});
        waveOutStates_[guest] = {
            args.a3,
            uint32_t(instance),
            flags,
            format.nAvgBytesPerSec,
            hostDeviceId,
            hostFlags,
            hostCallback,
            format.wFormatTag,
            format.nChannels,
            format.nSamplesPerSec,
            format.nBlockAlign,
            format.wBitsPerSample,
        };
        writeU32(args.a0, guest);
    }
    return true;
}

bool SyntheticDllRuntime::handleWaveOutClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    auto& winmm = winmmBridge();
    if (handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutClose) {
        const uint64_t start = GetTickCount64();
        auto state = waveOutStates_.find(args.a0);
        HWAVEOUT host = reinterpret_cast<HWAVEOUT>(handle->hostValue);
        if (winmm.waveOutReset) winmm.waveOutReset(host);
        if (state != waveOutStates_.end()) {
            CachedWaveOutDevice cached{};
            cached.hostValue = handle->hostValue;
            cached.deviceId = state->second.deviceId;
            cached.hostFlags = state->second.hostFlags;
            cached.hostCallback = state->second.hostCallback;
            cached.formatTag = state->second.formatTag;
            cached.channels = state->second.channels;
            cached.samplesPerSec = state->second.samplesPerSec;
            cached.avgBytesPerSec = state->second.avgBytesPerSec;
            cached.blockAlign = state->second.blockAlign;
            cached.bitsPerSample = state->second.bitsPerSample;
            cachedWaveOutDevices_.push_back(cached);
            while (cachedWaveOutDevices_.size() > 4) {
                CachedWaveOutDevice old = cachedWaveOutDevices_.front();
                cachedWaveOutDevices_.erase(cachedWaveOutDevices_.begin());
                if (old.hostValue) winmm.waveOutClose(reinterpret_cast<HWAVEOUT>(old.hostValue));
            }
            ret = MMSYSERR_NOERROR;
        } else {
            ret = winmm.waveOutClose(host);
        }
        const uint64_t elapsed = GetTickCount64() - start;
        spdlog::info("waveOutClose handle=0x{:08x} cached={} cacheSize={} -> {} elapsedMs={}",
                     args.a0, state != waveOutStates_.end(), cachedWaveOutDevices_.size(), ret, elapsed);
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
    const uint32_t guestFlags = readU32(args.a1 + 16);
    if (!guestData || !guestLength || guestLength > 0x400000 || !winmm.waveOutPrepareHeader) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    spdlog::debug("waveOutPrepareHeader hdr=0x{:08x} data=0x{:08x} length={} user=0x{:08x} flags=0x{:08x}",
                  args.a1, guestData, guestLength, readU32(args.a1 + 12), guestFlags);
    auto& stored = hostWaveBuffers_[args.a1];
    if (stored.data.size() != guestLength) stored.data.assign(guestLength, 0);
    uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size());
    auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
    *header = {};
    header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
    header->dwBufferLength = guestLength;
    header->dwBytesRecorded = readU32(args.a1 + 8);
    header->dwUser = readU32(args.a1 + 12);
    header->dwFlags = guestFlags;
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
    const uint64_t start = GetTickCount64();
    ret = winmm.waveOutUnprepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
    const uint64_t elapsed = GetTickCount64() - start;
    writeU32(args.a1 + 16, uint32_t(header->dwFlags));
    spdlog::info("waveOutUnprepareHeader hdr=0x{:08x} flags=0x{:08x} -> {} elapsedMs={}",
                 args.a1, uint32_t(header->dwFlags), ret, elapsed);
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
    const uint32_t guestFlags = readU32(args.a1 + 16);
    if (!guestData || !guestLength || guestLength > 0x400000) {
        ret = MMSYSERR_INVALPARAM;
        return true;
    }
    const uint32_t guestUser = readU32(args.a1 + 12);
    spdlog::info("waveOutWrite hdr=0x{:08x} data=0x{:08x} length={} user=0x{:08x} flags=0x{:08x}",
                 args.a1, guestData, guestLength, guestUser, guestFlags);
    const bool hadStoredHeader = hostWaveBuffers_.find(args.a1) != hostWaveBuffers_.end();
    auto& stored = hostWaveBuffers_[args.a1];
    if (stored.data.size() != guestLength) stored.data.assign(guestLength, 0);
    const bool readOk = uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size()) == UC_ERR_OK;
    logPcmStats("waveOutWrite", guestData, stored.data, readOk);
    auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
    if (!hadStoredHeader) {
        *header = {};
        header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
        header->dwBufferLength = guestLength;
        header->dwBytesRecorded = readU32(args.a1 + 8);
        header->dwUser = guestUser;
        header->dwFlags = guestFlags & ~WHDR_PREPARED;
        header->dwLoops = readU32(args.a1 + 20);
    } else {
        header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
        header->dwBufferLength = guestLength;
        header->dwBytesRecorded = readU32(args.a1 + 8);
        header->dwUser = guestUser;
        header->dwLoops = readU32(args.a1 + 20);
    }
    auto state = waveOutStates_.find(args.a0);
    if (state != waveOutStates_.end() && (state->second.flags & CALLBACK_TYPEMASK) == CALLBACK_EVENT) {
        auto* event = lookupGuestHandle(state->second.callback);
        if (event && event->kind == GuestHandle::Kind::HostEvent && event->hostValue) {
            ResetEvent(reinterpret_cast<HANDLE>(event->hostValue));
        }
    } else if (state != waveOutStates_.end() && (state->second.flags & CALLBACK_TYPEMASK) == CALLBACK_FUNCTION) {
        auto* event = lookupGuestHandle(guestUser);
        if (event && event->kind == GuestHandle::Kind::HostEvent && event->hostValue) {
            ResetEvent(reinterpret_cast<HANDLE>(event->hostValue));
            auto context = std::make_shared<HostWaveDoneContext>();
            context->guestHeader = args.a1;
            context->guestEvent = guestUser;
            context->hostEvent = reinterpret_cast<HANDLE>(event->hostValue);
            stored.completionContext = context;
            header->dwUser = DWORD_PTR(context.get());
        }
    }
    if (winmm.waveOutPrepareHeader && !(header->dwFlags & WHDR_PREPARED)) {
        winmm.waveOutPrepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
    }
    header->dwFlags = (header->dwFlags & WHDR_PREPARED) | (guestFlags & ~(WHDR_DONE | WHDR_PREPARED));
    const uint32_t doneBefore = g_waveOutDoneCallbacks.load(std::memory_order_relaxed);
    const uint64_t start = GetTickCount64();
    ret = winmm.waveOutWrite
        ? winmm.waveOutWrite(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header))
        : MMSYSERR_ERROR;
    const uint64_t elapsed = GetTickCount64() - start;
    const uint32_t callbackMode = state == waveOutStates_.end() ? CALLBACK_NULL : (state->second.flags & CALLBACK_TYPEMASK);
    const uint32_t durationMs = state != waveOutStates_.end() && state->second.avgBytesPerSec
        ? uint32_t((uint64_t(guestLength) * 1000u) / state->second.avgBytesPerSec)
        : 0;
    spdlog::info("waveOutWrite host ret={} callbackMode=0x{:08x} durationMs={} flags=0x{:08x} doneCallbacksBefore={} doneCallbacksAfter={} lastDoneTick={} elapsedMs={}",
                 ret,
                 callbackMode,
                 durationMs,
                 uint32_t(header->dwFlags),
                 doneBefore,
                 g_waveOutDoneCallbacks.load(std::memory_order_relaxed),
                 g_waveOutDoneTick.load(std::memory_order_relaxed),
                 elapsed);
    if (ret == MMSYSERR_NOERROR && (header->dwFlags & WHDR_DONE)) {
        writeU32(args.a1 + 16, uint32_t(header->dwFlags));
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
