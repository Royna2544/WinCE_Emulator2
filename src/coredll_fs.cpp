#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

uint64_t hostTickMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string pathToUtf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

bool isProfileTracePath(const std::string& lowerPath) {
    return lowerPath.find("inavidata\\config.bin") != std::string::npos ||
           lowerPath.find("inavidata/config.bin") != std::string::npos ||
           lowerPath.find("inavi\\res\\values.dat") != std::string::npos ||
           lowerPath.find("inavi/res/values.dat") != std::string::npos;
}

std::string wideZToUtf8(const wchar_t* text, size_t maxChars) {
    std::string out;
    for (size_t i = 0; i < maxChars && text[i]; ++i) {
        out.push_back(text[i] >= 0 && text[i] <= 0x7f ? char(text[i]) : '?');
    }
    return out;
}

bool isGuestDevicePath(const std::string& guestPath) {
    if (guestPath.size() < 4 || guestPath.back() != ':') return false;
    if (guestPath.find('\\') != std::string::npos || guestPath.find('/') != std::string::npos) return false;
    bool hasDigit = false;
    for (char ch : guestPath.substr(0, guestPath.size() - 1)) {
        if (std::isdigit(static_cast<unsigned char>(ch))) hasDigit = true;
        if (!std::isalnum(static_cast<unsigned char>(ch))) return false;
    }
    return hasDigit;
}

bool isRootWildcardPattern(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (!path.empty() && path.front() == '\\') path.erase(path.begin());
    return path == "*" || path == "*.*";
}

std::string serialAsciiPreview(const uint8_t* data, size_t size) {
    const size_t limit = std::min<size_t>(size, 120);
    std::string out;
    out.reserve(limit);
    for (size_t i = 0; i < limit; ++i) {
        const uint8_t ch = data[i];
        if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (std::isprint(ch)) {
            out.push_back(static_cast<char>(ch));
        } else {
            char buf[5]{};
            std::snprintf(buf, sizeof(buf), "\\x%02x", ch);
            out += buf;
        }
    }
    if (size > limit) out += "...";
    return out;
}

void writeGuestFileAttributeData(uc_engine* uc, uint32_t guestAddress, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    if (!guestAddress) return;
    uc_mem_write(uc, guestAddress + 0, &data.dwFileAttributes, sizeof(data.dwFileAttributes));
    uc_mem_write(uc, guestAddress + 4, &data.ftCreationTime, sizeof(data.ftCreationTime));
    uc_mem_write(uc, guestAddress + 12, &data.ftLastAccessTime, sizeof(data.ftLastAccessTime));
    uc_mem_write(uc, guestAddress + 20, &data.ftLastWriteTime, sizeof(data.ftLastWriteTime));
    uc_mem_write(uc, guestAddress + 28, &data.nFileSizeHigh, sizeof(data.nFileSizeHigh));
    uc_mem_write(uc, guestAddress + 32, &data.nFileSizeLow, sizeof(data.nFileSizeLow));
}

std::wstring attributeCacheKey(const std::filesystem::path& path) {
    std::wstring key = path.lexically_normal().wstring();
    std::replace(key.begin(), key.end(), L'/', L'\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](wchar_t ch) { return wchar_t(std::towlower(ch)); });
    return key;
}

SyntheticDllRuntime::CachedFileAttributes makeCachedAttributes(const WIN32_FILE_ATTRIBUTE_DATA& data) {
    SyntheticDllRuntime::CachedFileAttributes cached{};
    cached.ok = true;
    cached.attributes = data.dwFileAttributes;
    cached.creationLow = data.ftCreationTime.dwLowDateTime;
    cached.creationHigh = data.ftCreationTime.dwHighDateTime;
    cached.accessLow = data.ftLastAccessTime.dwLowDateTime;
    cached.accessHigh = data.ftLastAccessTime.dwHighDateTime;
    cached.writeLow = data.ftLastWriteTime.dwLowDateTime;
    cached.writeHigh = data.ftLastWriteTime.dwHighDateTime;
    cached.sizeHigh = data.nFileSizeHigh;
    cached.sizeLow = data.nFileSizeLow;
    return cached;
}

WIN32_FILE_ATTRIBUTE_DATA makeFileAttributeData(const SyntheticDllRuntime::CachedFileAttributes& cached) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    data.dwFileAttributes = cached.attributes;
    data.ftCreationTime.dwLowDateTime = cached.creationLow;
    data.ftCreationTime.dwHighDateTime = cached.creationHigh;
    data.ftLastAccessTime.dwLowDateTime = cached.accessLow;
    data.ftLastAccessTime.dwHighDateTime = cached.accessHigh;
    data.ftLastWriteTime.dwLowDateTime = cached.writeLow;
    data.ftLastWriteTime.dwHighDateTime = cached.writeHigh;
    data.nFileSizeHigh = cached.sizeHigh;
    data.nFileSizeLow = cached.sizeLow;
    return data;
}

bool createMayMutateFile(uint32_t desiredAccess, uint32_t creationDisposition) {
    constexpr uint32_t kGenericWrite = 0x40000000u;
    constexpr uint32_t kGenericAll = 0x10000000u;
    constexpr uint32_t kFileWriteData = 0x00000002u;
    constexpr uint32_t kFileAppendData = 0x00000004u;
    constexpr uint32_t kDelete = 0x00010000u;
    return (desiredAccess & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData | kDelete)) != 0 ||
           creationDisposition != OPEN_EXISTING;
}

void writeGuestFindData(uc_engine* uc, uint32_t guestAddress, const WIN32_FIND_DATAW& data) {
    if (!guestAddress) return;
    auto writeU32 = [&](uint32_t offset, uint32_t value) {
        uc_mem_write(uc, guestAddress + offset, &value, sizeof(value));
    };
    writeU32(0, data.dwFileAttributes);
    uc_mem_write(uc, guestAddress + 4, &data.ftCreationTime, sizeof(data.ftCreationTime));
    uc_mem_write(uc, guestAddress + 12, &data.ftLastAccessTime, sizeof(data.ftLastAccessTime));
    uc_mem_write(uc, guestAddress + 20, &data.ftLastWriteTime, sizeof(data.ftLastWriteTime));
    writeU32(28, data.nFileSizeHigh);
    writeU32(32, data.nFileSizeLow);
    writeU32(36, 0);
    for (uint32_t i = 0; i < 260; ++i) {
        const uint16_t ch = uint16_t(data.cFileName[i]);
        uc_mem_write(uc, guestAddress + 40 + i * 2, &ch, sizeof(ch));
        if (!ch) break;
    }
}

WIN32_FIND_DATAW makeVirtualRootFindData(const std::string& name) {
    WIN32_FIND_DATAW data{};
    data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_TEMPORARY;
    for (size_t i = 0; i < name.size() && i + 1 < std::size(data.cFileName); ++i) {
        data.cFileName[i] = wchar_t(static_cast<unsigned char>(name[i]));
    }
    return data;
}

WIN32_FIND_DATAW translateGuestFindData(WIN32_FIND_DATAW data, bool rootEnumeration) {
    if (!rootEnumeration) return data;
    std::wstring name(data.cFileName);
    std::replace(name.begin(), name.end(), L'/', L'\\');
    const size_t slash = name.find_last_of(L'\\');
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    std::fill(std::begin(data.cFileName), std::end(data.cFileName), 0);
    for (size_t i = 0; i < name.size() && i + 1 < std::size(data.cFileName); ++i) {
        data.cFileName[i] = name[i];
    }
    return data;
}

}

bool SyntheticDllRuntime::tryParkGuestSerialRead(const GuestCallArgs& args, uint32_t pc, uint32_t returnPc) {
    (void)pc;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::GuestSerialDevice) return false;

    CeDevice::SerialState* serial = ceDevice_.serialState(args.a0);
    if (!serial || !serial->virtualNoDataBackend || serial->deviceType != "serial") return false;
    if (!args.a2 || remoteSerialByteCount() != 0) return false;

    const uint64_t nowMs = hostTickMilliseconds();
    const CeDevice::NoDataReadDecision decision = ceDevice_.decideNoDataRead(args.a0, args.a2, nowMs);
    if (decision.action == CeDevice::NoDataReadAction::CompleteZero) return false;

    const uint32_t activeThread = ceKernel_.activeGuestThread();
    if (!activeThread) return false;
    auto active = ceKernel_.threads().find(activeThread);
    if (active == ceKernel_.threads().end()) return false;

    active->second.context = captureGuestCpuContext();
    active->second.context.registers[UC_MIPS_REG_PC] = returnPc;
    active->second.context.registers[UC_MIPS_REG_GP] = guestGpForCodeAddress(returnPc);
    active->second.context.registers[UC_MIPS_REG_V0] = 1;
    active->second.state = GuestThreadRunState::WaitingForSerialRead;
    active->second.waitHandle = args.a0;
    active->second.waitHandles.clear();
    active->second.waitAll = false;
    active->second.waitForMessages = false;
    active->second.waitWakeMask = 0;
    active->second.waitTimeoutResult = 0;
    active->second.sleepUntilMs = decision.deadlineMs;

    ceDevice_.beginPendingSerialRead(CeDevice::PendingSerialRead{
        activeThread,
        args.a0,
        args.a1,
        args.a2,
        args.a3,
        decision.deadlineMs,
    });

    auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
    const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
    spdlog::info("ReadFile virtual serial no-data wait handle=0x{:08x} thread=0x{:08x} name=\"{}\" requested={} deadlineMs={} return=0x{:08x}",
                 args.a0,
                 activeThread,
                 debugPath,
                 args.a2,
                 decision.deadlineMs,
                 returnPc);

    ceKernel_.activeGuestThread() = 0;
    if (!pendingBlockingApis_.empty()) {
        uc_emu_stop(uc_);
    } else if (!restoreMainThreadContextIfRunnable("ReadFile-serial-wait")) {
        switchToRunnableGuestThread("ReadFile-serial-wait");
    }
    pumpHostMessages();
    return true;
}

std::vector<std::string> SyntheticDllRuntime::virtualRootNames() const {
    std::vector<std::string> names;
    auto add = [&](std::string name) {
        std::replace(name.begin(), name.end(), '/', '\\');
        while (!name.empty() && name.front() == '\\') name.erase(name.begin());
        while (!name.empty() && name.back() == '\\') name.pop_back();
        if (name.empty()) return;
        const std::string key = lowerAscii(name);
        for (const auto& existing : names) {
            if (lowerAscii(existing) == key) return;
        }
        names.push_back(std::move(name));
    };
    add(sdmmcGuestRoot_);
    add("ResidentFlash");
    add("Windows");
    return names;
}

void SyntheticDllRuntime::registerCoredllFsExports(SyntheticModule& module) {
    struct CoreDllFs {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.fs",
                {
                    {0x00A0, {"CreateDirectoryW", Code::CoreDllCreateDirectoryW, &SyntheticDllRuntime::handleCreateDirectoryW}},
                    {0x00A5, {"DeleteFileW", Code::CoreDllDeleteFileW, &SyntheticDllRuntime::handleDeleteFileW}},
                    {0x00A6, {"GetFileAttributesW", Code::CoreDllGetFileAttributesW, &SyntheticDllRuntime::handleGetFileAttributesW}},
                    {0x00A7, {"FindFirstFileW", Code::CoreDllFindFirstFileW, &SyntheticDllRuntime::handleFindFirstFileW}},
                    {0x00A8, {"CreateFileW", Code::CoreDllCreateFileW, &SyntheticDllRuntime::handleCreateFileW}},
                    {0x048F, {"CreateFileForMappingW", Code::CoreDllCreateFileForMappingW, &SyntheticDllRuntime::handleCreateFileW}},
                    {0x00AA, {"ReadFile", Code::CoreDllReadFile, &SyntheticDllRuntime::handleReadFile}},
                    {0x00AB, {"WriteFile", Code::CoreDllWriteFile, &SyntheticDllRuntime::handleWriteFile}},
                    {0x00AC, {"GetFileSize", Code::CoreDllGetFileSize, &SyntheticDllRuntime::handleGetFileSize}},
                    {0x00AD, {"SetFilePointer", Code::CoreDllSetFilePointer, &SyntheticDllRuntime::handleSetFilePointer}},
                    {0x00AF, {"FlushFileBuffers", Code::CoreDllFlushFileBuffers, &SyntheticDllRuntime::handleFlushFileBuffers}},
                    {0x00B1, {"SetFileTime", Code::CoreDllSetFileTime, &SyntheticDllRuntime::handleSetFileTime}},
                    {0x00B4, {"FindClose", Code::CoreDllFindClose, &SyntheticDllRuntime::handleFindClose}},
                    {0x00B5, {"FindNextFileW", Code::CoreDllFindNextFileW, &SyntheticDllRuntime::handleFindNextFileW}},
                    {0x04D5, {"GetFileAttributesExW", Code::CoreDllGetFileAttributesExW, &SyntheticDllRuntime::handleGetFileAttributesExW}},
                },
            };
        }
    };

    const CoreDllFs fs;
    registerHandlers(module, fs.group());
}

bool SyntheticDllRuntime::handleCreateFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    const char* apiName = code == SyntheticExportCode::CoreDllCreateFileForMappingW
        ? "CreateFileForMappingW"
        : "CreateFileW";
    const std::string guestPath = readUtf16(args.a0);
    const std::string guestLowerForTrace = lowerAscii(guestPath);
    if (isGuestDevicePath(guestPath)) {
        ret = openGuestSerialDevice(guestPath, args.a1, args.a2);
        return true;
    }

    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    HANDLE host = INVALID_HANDLE_VALUE;
    if (!hostPath.empty()) {
        host = CreateFileW(hostPath.wstring().c_str(), args.a1, args.a2, nullptr, stackArg(4), stackArg(5), nullptr);
    }
    if (!hostPath.empty() && createMayMutateFile(args.a1, stackArg(4))) {
        ceFilesystem_.fileAttributeCache().erase(attributeCacheKey(hostPath));
    }
    if (host == INVALID_HANDLE_VALUE) {
        lastError_ = normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError());
        ret = 0xffffffffu;
        spdlog::warn("{} miss guest=\"{}\" host=\"{}\" access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x} lastError={}",
                     apiName, guestPath, pathToUtf8(hostPath), args.a1, args.a2, stackArg(4), stackArg(5), lastError_);
    } else {
        ret = makeGuestHandle({GuestHandle::Kind::HostFile, reinterpret_cast<uintptr_t>(host), 0});
        const std::string hostText = pathToUtf8(hostPath);
        ceFilesystem_.fileHandleDebugNames()[ret] = hostText;
        lastError_ = 0;
        const std::string guestLower = guestLowerForTrace;
        const std::string hostLower = lowerAscii(hostText);
        const bool traceMapFile = guestLower.find("mapdata") != std::string::npos ||
                                  hostLower.find("mapdata") != std::string::npos;
        const bool traceRouteFile = guestLower.find("route") != std::string::npos ||
                                    guestLower.find("routetest") != std::string::npos ||
                                    hostLower.find("route") != std::string::npos ||
                                    hostLower.find("routetest") != std::string::npos;
        const bool traceProfileFile = isProfileTracePath(guestLower) ||
                                      isProfileTracePath(hostLower);
        if (traceProfileFile) {
            spdlog::info("{} profile hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x} ra=0x{:08x}",
                         apiName, guestPath, hostText, ret, args.a1, args.a2,
                         stackArg(4), stackArg(5), args.ra);
        } else if (traceRouteFile) {
            spdlog::info("{} route hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x}",
                         apiName, guestPath, hostText, ret, args.a1, args.a2, stackArg(4), stackArg(5));
        } else if (traceMapFile) {
            spdlog::debug("{} map hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x}",
                          apiName, guestPath, hostText, ret, args.a1, args.a2, stackArg(4), stackArg(5));
        } else {
            spdlog::debug("{} hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x}",
                          apiName, guestPath, hostText, ret, args.a1, args.a2, stackArg(4), stackArg(5));
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleCreateDirectoryW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    if (!hostPath.empty()) ceFilesystem_.fileAttributeCache().erase(attributeCacheKey(hostPath));
    const BOOL ok = !hostPath.empty() && CreateDirectoryW(hostPath.wstring().c_str(), nullptr);
    if (ok) {
        ret = 1;
        lastError_ = 0;
    } else {
        const DWORD err = hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError();
        lastError_ = err;
        ret = (err == ERROR_ALREADY_EXISTS && std::filesystem::is_directory(hostPath)) ? 1 : 0;
        if (ret) lastError_ = 0;
    }
    spdlog::info("CreateDirectoryW guest=\"{}\" host=\"{}\" -> {} lastError={}",
                 guestPath, pathToUtf8(hostPath), ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleGetFileAttributesW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    const std::wstring key = hostPath.empty() ? std::wstring{} : attributeCacheKey(hostPath);
    auto cached = key.empty() ? ceFilesystem_.fileAttributeCache().end() : ceFilesystem_.fileAttributeCache().find(key);
    if (cached != ceFilesystem_.fileAttributeCache().end()) {
        ret = cached->second.ok ? cached->second.attributes : INVALID_FILE_ATTRIBUTES;
        lastError_ = cached->second.ok ? 0 : cached->second.error;
    } else {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const BOOL ok = !hostPath.empty() &&
                        GetFileAttributesExW(hostPath.wstring().c_str(), GetFileExInfoStandard, &data);
        if (ok) {
            auto value = makeCachedAttributes(data);
            ret = value.attributes;
            lastError_ = 0;
            if (!key.empty()) ceFilesystem_.fileAttributeCache()[key] = value;
        } else {
            ret = INVALID_FILE_ATTRIBUTES;
            lastError_ = normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError());
            if (!key.empty()) ceFilesystem_.fileAttributeCache()[key] = CachedFileAttributes{false, lastError_};
        }
    }
    if (ret == INVALID_FILE_ATTRIBUTES) {
        spdlog::warn("GetFileAttributesW miss guest=\"{}\" host=\"{}\" lastError={}",
                     guestPath, pathToUtf8(hostPath), lastError_);
    } else {
        spdlog::debug("GetFileAttributesW guest=\"{}\" host=\"{}\" -> 0x{:08x}",
                      guestPath, pathToUtf8(hostPath), ret);
    }
    return true;
}

bool SyntheticDllRuntime::handleGetFileAttributesExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const std::wstring key = hostPath.empty() ? std::wstring{} : attributeCacheKey(hostPath);
    auto cached = key.empty() ? ceFilesystem_.fileAttributeCache().end() : ceFilesystem_.fileAttributeCache().find(key);
    bool ok = false;
    if (args.a1 == GetFileExInfoStandard && cached != ceFilesystem_.fileAttributeCache().end()) {
        ok = cached->second.ok;
        if (ok) {
            data = makeFileAttributeData(cached->second);
        } else {
            lastError_ = cached->second.error;
        }
    } else if (!hostPath.empty() && args.a1 == GetFileExInfoStandard &&
               GetFileAttributesExW(hostPath.wstring().c_str(), GetFileExInfoStandard, &data)) {
        ok = true;
        if (!key.empty()) ceFilesystem_.fileAttributeCache()[key] = makeCachedAttributes(data);
    }
    if (ok) {
        writeGuestFileAttributeData(uc_, args.a2, data);
        ret = 1;
        lastError_ = 0;
    } else {
        ret = 0;
        if (cached == ceFilesystem_.fileAttributeCache().end()) {
            const uint32_t error = hostPath.empty()
                ? ERROR_PATH_NOT_FOUND
                : (args.a1 == GetFileExInfoStandard ? GetLastError() : ERROR_INVALID_PARAMETER);
            lastError_ = normalizeVirtualFileMiss(hostPath, error);
            if (!key.empty() && args.a1 == GetFileExInfoStandard) {
                ceFilesystem_.fileAttributeCache()[key] = CachedFileAttributes{false, lastError_};
            }
        }
    }
    if (ret) {
        spdlog::debug("GetFileAttributesExW guest=\"{}\" host=\"{}\" level={} out=0x{:08x} -> 1",
                      guestPath, pathToUtf8(hostPath), args.a1, args.a2);
    } else {
        spdlog::warn("GetFileAttributesExW miss guest=\"{}\" host=\"{}\" level={} out=0x{:08x} lastError={}",
                     guestPath, pathToUtf8(hostPath), args.a1, args.a2, lastError_);
    }
    return true;
}

bool SyntheticDllRuntime::handleDeleteFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::filesystem::path hostPath = resolveGuestPath(readUtf16(args.a0));
    if (!hostPath.empty()) ceFilesystem_.fileAttributeCache().erase(attributeCacheKey(hostPath));
    const BOOL ok = !hostPath.empty() && DeleteFileW(hostPath.wstring().c_str());
    ret = ok ? 1 : 0;
    lastError_ = ret ? 0 : GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleFindFirstFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    if (isRootWildcardPattern(guestPath)) {
        const auto roots = virtualRootNames();
        if (roots.empty()) {
            lastError_ = ERROR_FILE_NOT_FOUND;
            ret = 0xffffffffu;
            return true;
        }
        writeGuestFindData(uc_, args.a1, makeVirtualRootFindData(roots.front()));
        ret = makeGuestHandle({GuestHandle::Kind::GuestFind, 0, 1});
        ceFilesystem_.fileHandleDebugNames()[ret] = "\\*";
        lastError_ = 0;
        spdlog::info("FindFirstFileW virtual-root guest=\"{}\" guestHandle=0x{:08x} file=\"{}\"",
                     guestPath, ret, roots.front());
        return true;
    }

    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    WIN32_FIND_DATAW data{};
    HANDLE host = INVALID_HANDLE_VALUE;
    if (!hostPath.empty()) {
        host = FindFirstFileW(hostPath.wstring().c_str(), &data);
    }
    if (host == INVALID_HANDLE_VALUE) {
        lastError_ = normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError());
        ret = 0xffffffffu;
        spdlog::warn("FindFirstFileW miss guest=\"{}\" host=\"{}\" lastError={}",
                     guestPath, pathToUtf8(hostPath), lastError_);
    } else {
        const bool rootEnumeration = isRootWildcardPattern(guestPath);
        data = translateGuestFindData(data, rootEnumeration);
        writeGuestFindData(uc_, args.a1, data);
        ret = makeGuestHandle({GuestHandle::Kind::HostFind, reinterpret_cast<uintptr_t>(host),
                               rootEnumeration ? 1u : 0u});
        ceFilesystem_.fileHandleDebugNames()[ret] = pathToUtf8(hostPath);
        lastError_ = 0;
        spdlog::info("FindFirstFileW hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} file=\"{}\" attr=0x{:08x} size={}",
                     guestPath, pathToUtf8(hostPath), ret,
                     wideZToUtf8(data.cFileName, 260),
                     data.dwFileAttributes,
                     (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
    }
    return true;
}

bool SyntheticDllRuntime::handleFindNextFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (handle && handle->kind == GuestHandle::Kind::GuestFind) {
        const auto roots = virtualRootNames();
        if (handle->filePointer < roots.size()) {
            writeGuestFindData(uc_, args.a1, makeVirtualRootFindData(roots[handle->filePointer]));
            spdlog::info("FindNextFileW virtual-root handle=0x{:08x} file=\"{}\"",
                         args.a0, roots[handle->filePointer]);
            ++handle->filePointer;
            ret = 1;
            lastError_ = 0;
        } else {
            ret = 0;
            lastError_ = ERROR_NO_MORE_FILES;
            spdlog::info("FindNextFileW virtual-root done handle=0x{:08x} lastError={}", args.a0, lastError_);
        }
        return true;
    }
    if (!handle || handle->kind != GuestHandle::Kind::HostFind || !handle->hostValue) {
        lastError_ = 6;
        ret = 0;
    } else {
        WIN32_FIND_DATAW data{};
        const BOOL ok = FindNextFileW(reinterpret_cast<HANDLE>(handle->hostValue), &data);
        ret = ok ? 1 : 0;
        lastError_ = ok ? 0 : GetLastError();
        if (ok) {
            data = translateGuestFindData(data, handle->filePointer != 0);
            writeGuestFindData(uc_, args.a1, data);
            auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
            const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
            spdlog::info("FindNextFileW hit handle=0x{:08x} path=\"{}\" file=\"{}\" attr=0x{:08x} size={}",
                         args.a0, debugPath, wideZToUtf8(data.cFileName, 260),
                         data.dwFileAttributes,
                         (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
        } else {
            spdlog::info("FindNextFileW miss handle=0x{:08x} lastError={}", args.a0, lastError_);
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleFindClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = closeGuestHandle(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleReadFile(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    writeU32(args.a3, 0);
    if (handle && handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        if (!args.a1 && args.a2) {
            lastError_ = 87;
            ret = 0;
            return true;
        }
        const DWORD requested = std::min<DWORD>(args.a2, 1024);
        static thread_local std::vector<uint8_t> injected;
        injected.resize(requested);
        const size_t transferred = readRemoteSerialBytes(injected.data(), injected.size());
        if (transferred) {
            uc_mem_write(uc_, args.a1, injected.data(), transferred);
            writeU32(args.a3, static_cast<uint32_t>(transferred));
        }
        ret = 1;
        lastError_ = 0;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        const CeDevice::SerialState* serial = ceDevice_.serialState(args.a0);
        spdlog::info("ReadFile guest device handle=0x{:08x} name=\"{}\" requested={} transferred={} virtualNoDataBackend={}",
                     args.a0, debugPath, args.a2, transferred,
                     serial && serial->virtualNoDataBackend ? 1 : 0);
    } else if (!handle || (handle->kind != GuestHandle::Kind::HostFile &&
                           handle->kind != GuestHandle::Kind::HostSerialDevice) ||
               !handle->hostValue) {
        lastError_ = 6;
        ret = 0;
    } else if (!args.a1 && args.a2) {
        lastError_ = 87;
        ret = 0;
    } else {
        DWORD transferred = 0;
        const DWORD requested = handle->kind == GuestHandle::Kind::HostSerialDevice
            ? std::min<DWORD>(args.a2, 1024)
            : args.a2;
        static thread_local std::vector<uint8_t> bytes;
        bytes.resize(requested);
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        if (handle->kind == GuestHandle::Kind::HostSerialDevice) {
            const size_t injected = readRemoteSerialBytes(bytes.data(), bytes.size());
            if (injected) {
                uc_mem_write(uc_, args.a1, bytes.data(), injected);
                writeU32(args.a3, static_cast<uint32_t>(injected));
                ret = 1;
                lastError_ = 0;
                const uint32_t readCount = ++ceFilesystem_.fileReadCounts()[args.a0];
                spdlog::info("ReadFile remote serial handle=0x{:08x} path=\"{}\" guestRequested={} transferred={} read#={} data=\"{}\"",
                             args.a0, debugPath, args.a2, injected, readCount,
                             serialAsciiPreview(bytes.data(), injected));
                return true;
            }
        }
        const std::string debugLower = lowerAscii(debugPath);
        const bool traceProfileFile = isProfileTracePath(debugLower);
        DWORD fileOffsetBefore = 0xffffffffu;
        if (handle->kind == GuestHandle::Kind::HostFile && traceProfileFile) {
            fileOffsetBefore = SetFilePointer(reinterpret_cast<HANDLE>(handle->hostValue), 0, nullptr, FILE_CURRENT);
        }
        const BOOL ok = ReadFile(reinterpret_cast<HANDLE>(handle->hostValue),
                                 bytes.empty() ? nullptr : bytes.data(),
                                 requested, &transferred, nullptr);
        if (ok && transferred) uc_mem_write(uc_, args.a1, bytes.data(), transferred);
        ret = ok ? 1 : 0;
        writeU32(args.a3, transferred);
        lastError_ = ret ? 0 : GetLastError();
        const uint32_t readCount = ++ceFilesystem_.fileReadCounts()[args.a0];
        constexpr uint32_t gpsPortSlot = 0x0079233c;
        if (ok && transferred && args.a1 <= gpsPortSlot &&
            gpsPortSlot + sizeof(uint16_t) <= args.a1 + transferred) {
            uint16_t slotValue = 0;
            uc_mem_read(uc_, gpsPortSlot, &slotValue, sizeof(slotValue));
            const uint32_t offset = gpsPortSlot - args.a1;
            spdlog::info("diag gps-port slot filled by ReadFile handle=0x{:08x} path=\"{}\" buffer=0x{:08x} guestRequested={} hostRequested={} transferred={} offset=0x{:x} bytes={} {} slotNow={}",
                         args.a0, debugPath, args.a1, args.a2, requested, transferred, offset,
                         offset < bytes.size() ? unsigned(bytes[offset]) : 0u,
                         offset + 1 < bytes.size() ? unsigned(bytes[offset + 1]) : 0u,
                         int16_t(slotValue));
        }
        if (traceProfileFile) {
            spdlog::info("ReadFile profile handle=0x{:08x} path=\"{}\" guestBuffer=0x{:08x} requested={} transferred={} fileOffset=0x{:08x} ok={} lastError={} ra=0x{:08x}",
                         args.a0, debugPath, args.a1, args.a2, transferred,
                         fileOffsetBefore, ok ? 1 : 0, lastError_, args.ra);
        }
        if (handle->kind == GuestHandle::Kind::HostSerialDevice) {
            const std::string preview = transferred ? serialAsciiPreview(bytes.data(), transferred) : std::string{};
            spdlog::info("ReadFile host serial handle=0x{:08x} path=\"{}\" guestRequested={} hostRequested={} transferred={} ok={} lastError={} read#={} data=\"{}\"",
                         args.a0, debugPath, args.a2, requested, transferred, ok ? 1 : 0, lastError_, readCount, preview);
        }
        if (readCount <= 32 || !ok || transferred != args.a2 || transferred == 0) {
            spdlog::debug("ReadFile handle=0x{:08x} path=\"{}\" guestRequested={} hostRequested={} transferred={} ok={} read#={}",
                          args.a0, debugPath, args.a2, requested, transferred, ok ? 1 : 0, readCount);
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWriteFile(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    writeU32(args.a3, 0);
    if (handle && handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        ret = 1;
        lastError_ = 0;
    } else if (!handle || (handle->kind != GuestHandle::Kind::HostFile &&
                           handle->kind != GuestHandle::Kind::HostSerialDevice) ||
               !handle->hostValue) {
        lastError_ = 6;
        ret = 0;
    } else if (!args.a1 && args.a2) {
        lastError_ = 87;
        ret = 0;
    } else {
        DWORD transferred = 0;
        static thread_local std::vector<uint8_t> bytes;
        bytes.resize(args.a2);
        if (args.a2) uc_mem_read(uc_, args.a1, bytes.data(), bytes.size());
        const BOOL ok = WriteFile(reinterpret_cast<HANDLE>(handle->hostValue),
                                  bytes.empty() ? nullptr : bytes.data(),
                                  args.a2, &transferred, nullptr);
        ret = ok ? 1 : 0;
        writeU32(args.a3, transferred);
        lastError_ = ret ? 0 : GetLastError();
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? std::string{} : debugName->second;
        const std::string debugLower = lowerAscii(debugPath);
        const bool traceProfileFile = isProfileTracePath(debugLower);
        DWORD fileOffsetBefore = 0xffffffffu;
        if (handle->kind == GuestHandle::Kind::HostFile && traceProfileFile) {
            fileOffsetBefore = SetFilePointer(reinterpret_cast<HANDLE>(handle->hostValue), 0, nullptr, FILE_CURRENT);
        }
        if (traceProfileFile) {
            spdlog::info("WriteFile profile handle=0x{:08x} path=\"{}\" guestBuffer=0x{:08x} requested={} transferred={} fileOffset=0x{:08x} ok={} lastError={} ra=0x{:08x}",
                         args.a0, debugPath, args.a1, args.a2, transferred,
                         fileOffsetBefore, ok ? 1 : 0, lastError_, args.ra);
        } else if (debugLower.find("route") != std::string::npos ||
            debugLower.find("routetest") != std::string::npos) {
            spdlog::info("WriteFile route handle=0x{:08x} path=\"{}\" requested={} transferred={} ok={} lastError={}",
                         args.a0, debugPath, args.a2, transferred, ok ? 1 : 0, lastError_);
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleGetFileSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
        lastError_ = 6;
        ret = 0xffffffffu;
    } else {
        DWORD high = 0;
        ret = GetFileSize(reinterpret_cast<HANDLE>(handle->hostValue), args.a1 ? &high : nullptr);
        if (args.a1) writeU32(args.a1, high);
        lastError_ = ret == 0xffffffffu ? GetLastError() : 0;
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        spdlog::debug("GetFileSize handle=0x{:08x} path=\"{}\" size={} high={} lastError={}",
                      args.a0, debugName == ceFilesystem_.fileHandleDebugNames().end() ? "" : debugName->second,
                      ret, high, lastError_);
    }
    return true;
}

bool SyntheticDllRuntime::handleSetFilePointer(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
        lastError_ = 6;
        ret = 0xffffffffu;
    } else {
        LONG high = 0;
        if (args.a2) uc_mem_read(uc_, args.a2, &high, sizeof(high));
        ret = SetFilePointer(reinterpret_cast<HANDLE>(handle->hostValue), LONG(args.a1), args.a2 ? &high : nullptr, args.a3);
        if (args.a2) writeU32(args.a2, uint32_t(high));
        lastError_ = ret == 0xffffffffu ? GetLastError() : 0;
        const uint32_t seekCount = ++ceFilesystem_.fileSeekCounts()[args.a0];
        if (seekCount <= 32 || ret == 0xffffffffu) {
            auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
            const std::string debugPath = debugName == ceFilesystem_.fileHandleDebugNames().end() ? "" : debugName->second;
            const bool traceProfileFile = isProfileTracePath(lowerAscii(debugPath));
            if (traceProfileFile) {
                spdlog::info("SetFilePointer profile handle=0x{:08x} path=\"{}\" distance={} method={} -> low={} high={} lastError={} seek#={} ra=0x{:08x}",
                             args.a0, debugPath, int32_t(args.a1), args.a3,
                             ret, uint32_t(high), lastError_, seekCount, args.ra);
            } else {
                spdlog::debug("SetFilePointer handle=0x{:08x} path=\"{}\" distance={} method={} -> low={} high={} lastError={} seek#={}",
                              args.a0, debugPath, int32_t(args.a1), args.a3,
                              ret, uint32_t(high), lastError_, seekCount);
            }
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleFlushFileBuffers(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || (handle->kind != GuestHandle::Kind::HostFile &&
                    handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        !handle->hostValue) {
        lastError_ = 6;
        ret = 0;
    } else {
        const BOOL ok = FlushFileBuffers(reinterpret_cast<HANDLE>(handle->hostValue));
        ret = ok ? 1 : 0;
        lastError_ = ret ? 0 : GetLastError();
        auto debugName = ceFilesystem_.fileHandleDebugNames().find(args.a0);
        spdlog::info("FlushFileBuffers handle=0x{:08x} path=\"{}\" -> {} lastError={}",
                     args.a0, debugName == ceFilesystem_.fileHandleDebugNames().end() ? "" : debugName->second,
                     ret, lastError_);
    }
    return true;
}

bool SyntheticDllRuntime::handleSetFileTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
        lastError_ = 6;
        ret = 0;
    } else {
        FILETIME creation{}, access{}, write{};
        FILETIME* creationPtr = nullptr;
        FILETIME* accessPtr = nullptr;
        FILETIME* writePtr = nullptr;
        if (args.a1 && uc_mem_read(uc_, args.a1, &creation, sizeof(creation)) == UC_ERR_OK) creationPtr = &creation;
        if (args.a2 && uc_mem_read(uc_, args.a2, &access, sizeof(access)) == UC_ERR_OK) accessPtr = &access;
        if (args.a3 && uc_mem_read(uc_, args.a3, &write, sizeof(write)) == UC_ERR_OK) writePtr = &write;
        ret = SetFileTime(reinterpret_cast<HANDLE>(handle->hostValue), creationPtr, accessPtr, writePtr) ? 1 : 0;
        lastError_ = ret ? 0 : GetLastError();
    }
    return true;
}
