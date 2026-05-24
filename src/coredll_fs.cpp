#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <regex>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return text;
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

void writeGuestFileAttributeData(uc_engine* uc, uint32_t guestAddress, const WIN32_FILE_ATTRIBUTE_DATA& data) {
    if (!guestAddress) return;
    uc_mem_write(uc, guestAddress + 0, &data.dwFileAttributes, sizeof(data.dwFileAttributes));
    uc_mem_write(uc, guestAddress + 4, &data.ftCreationTime, sizeof(data.ftCreationTime));
    uc_mem_write(uc, guestAddress + 12, &data.ftLastAccessTime, sizeof(data.ftLastAccessTime));
    uc_mem_write(uc, guestAddress + 20, &data.ftLastWriteTime, sizeof(data.ftLastWriteTime));
    uc_mem_write(uc, guestAddress + 28, &data.nFileSizeHigh, sizeof(data.nFileSizeHigh));
    uc_mem_write(uc, guestAddress + 32, &data.nFileSizeLow, sizeof(data.nFileSizeLow));
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
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    if (isGuestDevicePath(guestPath)) {
        ret = openGuestSerialDevice(guestPath, args.a1, args.a2);
        return true;
    }

    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    HANDLE host = INVALID_HANDLE_VALUE;
    if (!hostPath.empty()) {
        host = CreateFileW(hostPath.wstring().c_str(), args.a1, args.a2, nullptr, stackArg(4), stackArg(5), nullptr);
    }
    if (host == INVALID_HANDLE_VALUE) {
        lastError_ = normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError());
        ret = 0xffffffffu;
        spdlog::warn("CreateFileW miss guest=\"{}\" host=\"{}\" access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x} lastError={}",
                     guestPath, pathToUtf8(hostPath), args.a1, args.a2, stackArg(4), stackArg(5), lastError_);
    } else {
        ret = makeGuestHandle({GuestHandle::Kind::HostFile, reinterpret_cast<uintptr_t>(host), 0});
        fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
        lastError_ = 0;
        spdlog::info("CreateFileW hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x}",
                     guestPath, pathToUtf8(hostPath), ret, args.a1, args.a2, stackArg(4), stackArg(5));
    }
    return true;
}

bool SyntheticDllRuntime::handleCreateDirectoryW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
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
    ret = hostPath.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(hostPath.wstring().c_str());
    lastError_ = ret == INVALID_FILE_ATTRIBUTES
        ? normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError())
        : 0;
    spdlog::info("GetFileAttributesW guest=\"{}\" host=\"{}\" -> 0x{:08x} lastError={}",
                 guestPath, pathToUtf8(hostPath), ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleGetFileAttributesExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string guestPath = readUtf16(args.a0);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const BOOL ok = !hostPath.empty() && args.a1 == GetFileExInfoStandard &&
                    GetFileAttributesExW(hostPath.wstring().c_str(), GetFileExInfoStandard, &data);
    if (ok) {
        writeGuestFileAttributeData(uc_, args.a2, data);
        ret = 1;
        lastError_ = 0;
    } else {
        ret = 0;
        const uint32_t error = hostPath.empty()
            ? ERROR_PATH_NOT_FOUND
            : (args.a1 == GetFileExInfoStandard ? GetLastError() : ERROR_INVALID_PARAMETER);
        lastError_ = normalizeVirtualFileMiss(hostPath, error);
    }
    spdlog::info("GetFileAttributesExW guest=\"{}\" host=\"{}\" level={} out=0x{:08x} -> {} lastError={}",
                 guestPath, pathToUtf8(hostPath), args.a1, args.a2, ret, lastError_);
    return true;
}

bool SyntheticDllRuntime::handleDeleteFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::filesystem::path hostPath = resolveGuestPath(readUtf16(args.a0));
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
        fileHandleDebugNames_[ret] = "\\*";
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
        fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
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
            auto debugName = fileHandleDebugNames_.find(args.a0);
            const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
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
        ret = 1;
        lastError_ = 0;
        auto debugName = fileHandleDebugNames_.find(args.a0);
        const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
        spdlog::info("ReadFile guest device handle=0x{:08x} name=\"{}\" requested={} transferred=0",
                     args.a0, debugPath, args.a2);
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
        const BOOL ok = ReadFile(reinterpret_cast<HANDLE>(handle->hostValue),
                                 bytes.empty() ? nullptr : bytes.data(),
                                 args.a2, &transferred, nullptr);
        if (ok && transferred) uc_mem_write(uc_, args.a1, bytes.data(), transferred);
        ret = ok ? 1 : 0;
        writeU32(args.a3, transferred);
        lastError_ = ret ? 0 : GetLastError();
        const uint32_t readCount = ++fileReadCounts_[args.a0];
        auto debugName = fileHandleDebugNames_.find(args.a0);
        const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
        if (readCount <= 32 || !ok || transferred != args.a2 || transferred == 0) {
            spdlog::debug("ReadFile handle=0x{:08x} path=\"{}\" requested={} transferred={} ok={} read#={}",
                          args.a0, debugPath, args.a2, transferred, ok ? 1 : 0, readCount);
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
        auto debugName = fileHandleDebugNames_.find(args.a0);
        spdlog::debug("GetFileSize handle=0x{:08x} path=\"{}\" size={} high={} lastError={}",
                      args.a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
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
        const uint32_t seekCount = ++fileSeekCounts_[args.a0];
        if (seekCount <= 32 || ret == 0xffffffffu) {
            auto debugName = fileHandleDebugNames_.find(args.a0);
            spdlog::debug("SetFilePointer handle=0x{:08x} path=\"{}\" distance={} method={} -> low={} high={} lastError={} seek#={}",
                          args.a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
                          int32_t(args.a1), args.a3, ret, uint32_t(high), lastError_, seekCount);
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
        auto debugName = fileHandleDebugNames_.find(args.a0);
        spdlog::info("FlushFileBuffers handle=0x{:08x} path=\"{}\" -> {} lastError={}",
                     args.a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
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
