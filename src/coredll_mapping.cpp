#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#endif

#include "synthetic_dll.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string lowerAscii(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

uint64_t fnv1a64(std::string_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : text) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string sanitizeFileNameFragment(std::string_view text) {
    std::string out;
    out.reserve(std::min<size_t>(text.size(), 48));
    for (unsigned char ch : text) {
        if (out.size() >= 48) break;
        if (std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-') {
            out.push_back(char(ch));
        } else {
            out.push_back('_');
        }
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) out = "unnamed";
    return out;
}

}

std::filesystem::path SyntheticDllRuntime::ensureSharedMappingDirectory() {
    if (!sharedMappingDirectory_.empty()) {
        return sharedMappingDirectory_;
    }
#if defined(_WIN32)
    wchar_t buffer[32768]{};
    DWORD chars = GetEnvironmentVariableW(L"INAVI_EMU_SHARED_MAPPING_DIR", buffer, DWORD(std::size(buffer)));
    if (chars && chars < DWORD(std::size(buffer))) {
        sharedMappingDirectory_ = std::filesystem::path(buffer);
    }
    if (sharedMappingDirectory_.empty()) {
        chars = GetEnvironmentVariableW(L"INAVI_EMU_WINDOW_REGISTRY", buffer, DWORD(std::size(buffer)));
        if (chars && chars < DWORD(std::size(buffer))) {
            sharedMappingDirectory_ = std::filesystem::path(buffer).parent_path() / L"shared_mappings";
        }
    }
    if (sharedMappingDirectory_.empty()) {
        chars = GetEnvironmentVariableW(L"INAVI_EMU_CHILD_LOG_DIR", buffer, DWORD(std::size(buffer)));
        if (chars && chars < DWORD(std::size(buffer))) {
            sharedMappingDirectory_ = std::filesystem::path(buffer) / L"shared_mappings";
        }
    }
#endif
    if (sharedMappingDirectory_.empty()) {
        std::error_code ignored;
        sharedMappingDirectory_ = std::filesystem::temp_directory_path(ignored) / "inavi_emu_shared_mappings";
    }
    std::error_code ignored;
    std::filesystem::create_directories(sharedMappingDirectory_, ignored);
#if defined(_WIN32)
    SetEnvironmentVariableW(L"INAVI_EMU_SHARED_MAPPING_DIR", sharedMappingDirectory_.wstring().c_str());
#endif
    return sharedMappingDirectory_;
}

std::filesystem::path SyntheticDllRuntime::sharedMappingBackingPath(const std::string& name) {
    const std::string key = lowerAscii(name);
    std::ostringstream stem;
    stem << "mapping_" << std::hex << std::setw(16) << std::setfill('0') << fnv1a64(key)
         << "_" << sanitizeFileNameFragment(key) << ".bin";
    return ensureSharedMappingDirectory() / stem.str();
}

uint64_t SyntheticDllRuntime::sharedMappingVersion(const std::filesystem::path& backingPath) const {
    std::ifstream input(backingPath.string() + ".version");
    uint64_t version = 0;
    input >> version;
    return version;
}

void SyntheticDllRuntime::writeSharedMappingVersion(const std::filesystem::path& backingPath, uint64_t version) const {
    std::ofstream output(backingPath.string() + ".version", std::ios::trunc);
    output << version << "\n";
}

bool SyntheticDllRuntime::ensureSharedMappingBacking(const std::filesystem::path& backingPath,
                                                     uint64_t requestedSize,
                                                     uint64_t& actualSize,
                                                     bool& existed) {
    std::error_code ec;
    existed = std::filesystem::exists(backingPath, ec);
    actualSize = existed ? std::filesystem::file_size(backingPath, ec) : 0;
    if (ec) {
        existed = false;
        actualSize = 0;
        ec.clear();
    }
    if (!existed) {
        std::ofstream create(backingPath, std::ios::binary);
        if (!create.good()) {
            return false;
        }
    }
    if (actualSize < requestedSize) {
        std::filesystem::resize_file(backingPath, requestedSize, ec);
        if (ec) {
            return false;
        }
        actualSize = requestedSize;
    }
    if (!existed) {
        writeSharedMappingVersion(backingPath, 1);
    } else if (!std::filesystem::exists(backingPath.string() + ".version", ec)) {
        writeSharedMappingVersion(backingPath, 1);
    }
    return true;
}

bool SyntheticDllRuntime::readSharedMappingBytes(const std::filesystem::path& backingPath,
                                                 uint64_t offset,
                                                 std::vector<uint8_t>& bytes) const {
    if (bytes.empty()) return true;
    std::ifstream input(backingPath, std::ios::binary);
    if (!input.good()) return false;
    input.seekg(std::streamoff(offset), std::ios::beg);
    input.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    const std::streamsize read = input.gcount();
    if (read < std::streamsize(bytes.size())) {
        std::fill(bytes.begin() + std::max<std::streamsize>(0, read), bytes.end(), 0);
    }
    return true;
}

bool SyntheticDllRuntime::writeSharedMappingBytes(const std::filesystem::path& backingPath,
                                                  uint64_t offset,
                                                  const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return true;
    std::fstream file(backingPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!file.good()) return false;
    file.seekp(std::streamoff(offset), std::ios::beg);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!file.good()) return false;
    writeSharedMappingVersion(backingPath, sharedMappingVersion(backingPath) + 1);
    return true;
}

bool SyntheticDllRuntime::syncNamedMappedView(uint32_t baseAddress, GuestMappedView& view, bool forceWrite) {
    auto mapping = fileMappings_.find(view.mappingHandle);
    if (mapping == fileMappings_.end() || !mapping->second.namedShared || mapping->second.backingPath.empty()) {
        return false;
    }
    const std::string& name = mapping->second.name;
    std::vector<uint8_t> current(view.size);
    if (view.size && uc_mem_read(uc_, baseAddress, current.data(), current.size()) != UC_ERR_OK) {
        return false;
    }
    if (forceWrite || current != view.shadow) {
        if (!writeSharedMappingBytes(mapping->second.backingPath, view.offset, current)) {
            return false;
        }
        view.shadow = std::move(current);
        view.backingVersion = sharedMappingVersion(mapping->second.backingPath);
        spdlog::info("shared mapping sync write name=\"{}\" base=0x{:08x} size={} version={} forced={}",
                     name, baseAddress, view.size, view.backingVersion, forceWrite);
        return true;
    }
    const uint64_t version = sharedMappingVersion(mapping->second.backingPath);
    if (version != view.backingVersion) {
        std::vector<uint8_t> remote(view.size);
        if (!readSharedMappingBytes(mapping->second.backingPath, view.offset, remote)) {
            return false;
        }
        if (view.size) uc_mem_write(uc_, baseAddress, remote.data(), remote.size());
        view.shadow = std::move(remote);
        view.backingVersion = version;
        spdlog::info("shared mapping sync read name=\"{}\" base=0x{:08x} size={} version={}",
                     name, baseAddress, view.size, view.backingVersion);
        return true;
    }
    return false;
}

void SyntheticDllRuntime::syncNamedMappedViews(bool forceWrite) {
    for (auto& [base, view] : mappedViews_) {
        syncNamedMappedView(base, view, forceWrite);
    }
}

uint32_t SyntheticDllRuntime::handleCreateFileMappingW(uint32_t fileHandle, uint32_t, uint32_t protect,
                                                       uint32_t sizeHigh) {
    const uint32_t sizeLow = stackArg(4);
    const uint32_t namePtr = stackArg(5);
    const std::string name = readUtf16(namePtr, 260);
    auto debugName = fileHandleDebugNames_.find(fileHandle);
    const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
    uint64_t mappingSize = (uint64_t(sizeHigh) << 32) | sizeLow;
    if (fileHandle != 0xffffffffu) {
        auto* file = lookupGuestHandle(fileHandle);
        if (!file || file->kind != GuestHandle::Kind::HostFile || !file->hostValue) {
            lastError_ = 6;
            spdlog::info("CreateFileMappingW failed invalid file handle=0x{:08x} path=\"{}\" protect=0x{:08x} size={} name=\"{}\" lastError={}",
                         fileHandle, debugPath, protect, mappingSize, name, lastError_);
            return 0;
        }
#if defined(_WIN32)
        if (!mappingSize) {
            LARGE_INTEGER size{};
            if (GetFileSizeEx(reinterpret_cast<HANDLE>(file->hostValue), &size)) {
                mappingSize = uint64_t(size.QuadPart);
            }
        }
#endif
    }
    if (!mappingSize) {
        lastError_ = 87;
        spdlog::info("CreateFileMappingW failed zero size file=0x{:08x} path=\"{}\" protect=0x{:08x} name=\"{}\" lastError={}",
                     fileHandle, debugPath, protect, name, lastError_);
        return 0;
    }
    std::filesystem::path backingPath;
    bool namedShared = false;
    bool alreadyExists = false;
    if (!name.empty()) {
        backingPath = sharedMappingBackingPath(name);
        uint64_t actualSize = mappingSize;
        if (!ensureSharedMappingBacking(backingPath, mappingSize, actualSize, alreadyExists)) {
            lastError_ = 8;
            spdlog::info("CreateFileMappingW failed shared backing file=0x{:08x} path=\"{}\" protect=0x{:08x} size={} name=\"{}\" backing=\"{}\" lastError={}",
                         fileHandle, debugPath, protect, mappingSize, name, pathToUtf8(backingPath), lastError_);
            return 0;
        }
        mappingSize = actualSize;
        namedShared = true;
    }
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestFileMapping, 0, 0});
    fileMappings_[handle] = GuestFileMapping{fileHandle, mappingSize, protect, name, backingPath, namedShared};
    fileHandleDebugNames_[handle] = "mapping name=\"" + name + "\" file=\"" + debugPath + "\" backing=\"" +
                                    pathToUtf8(backingPath) + "\"";
    lastError_ = alreadyExists ? 183 : 0;
    spdlog::info("CreateFileMappingW file=0x{:08x} path=\"{}\" protect=0x{:08x} size={} name=\"{}\" shared={} existing={} backing=\"{}\" -> 0x{:08x} lastError={}",
                 fileHandle, debugPath, protect, mappingSize, name, namedShared, alreadyExists,
                 pathToUtf8(backingPath), handle, lastError_);
    return handle;
}

uint32_t SyntheticDllRuntime::handleMapViewOfFile(uint32_t mappingHandle, uint32_t desiredAccess, uint32_t offsetHigh,
                                                  uint32_t offsetLow) {
    const uint32_t bytesToMap = stackArg(4);
    auto mapping = fileMappings_.find(mappingHandle);
    if (mapping == fileMappings_.end()) {
        lastError_ = 6;
        spdlog::info("MapViewOfFile failed unknown mapping=0x{:08x} offsetHigh=0x{:08x} offsetLow=0x{:08x} bytes={} lastError={}",
                     mappingHandle, offsetHigh, offsetLow, bytesToMap, lastError_);
        return 0;
    }
    const uint64_t offset = (uint64_t(offsetHigh) << 32) | offsetLow;
    if (offset >= mapping->second.size) {
        lastError_ = 87;
        spdlog::info("MapViewOfFile failed range mapping=0x{:08x} name=\"{}\" offset={} size={} bytes={} lastError={}",
                     mappingHandle, mapping->second.name, offset, mapping->second.size, bytesToMap, lastError_);
        return 0;
    }
    const uint64_t viewSize64 = bytesToMap ? bytesToMap : mapping->second.size - offset;
    if (!viewSize64 || viewSize64 > 0x02000000u) {
        lastError_ = 87;
        spdlog::info("MapViewOfFile failed view size mapping=0x{:08x} name=\"{}\" offset={} viewSize={} bytes={} lastError={}",
                     mappingHandle, mapping->second.name, offset, viewSize64, bytesToMap, lastError_);
        return 0;
    }
    const uint32_t viewSize = uint32_t(viewSize64);
    const uint32_t base = allocate(viewSize, true);
    if (!base) {
        spdlog::info("MapViewOfFile failed guest allocation mapping=0x{:08x} name=\"{}\" viewSize={}",
                     mappingHandle, mapping->second.name, viewSize);
        return 0;
    }
    GuestMappedView mappedView{mappingHandle, offset, viewSize};
    if (mapping->second.namedShared && !mapping->second.backingPath.empty()) {
        std::vector<uint8_t> bytes(viewSize);
        if (readSharedMappingBytes(mapping->second.backingPath, offset, bytes)) {
            if (!bytes.empty()) uc_mem_write(uc_, base, bytes.data(), bytes.size());
            mappedView.shadow = std::move(bytes);
            mappedView.backingVersion = sharedMappingVersion(mapping->second.backingPath);
        }
    } else {
#if defined(_WIN32)
        auto* file = lookupGuestHandle(mapping->second.fileHandle);
        if (file && file->kind == GuestHandle::Kind::HostFile && file->hostValue) {
            HANDLE host = reinterpret_cast<HANDLE>(file->hostValue);
            LARGE_INTEGER oldPos{};
            LARGE_INTEGER wanted{};
            wanted.QuadPart = LONGLONG(offset);
            SetFilePointerEx(host, {}, &oldPos, FILE_CURRENT);
            if (SetFilePointerEx(host, wanted, nullptr, FILE_BEGIN)) {
                std::vector<uint8_t> bytes(viewSize);
                DWORD read = 0;
                if (ReadFile(host, bytes.data(), viewSize, &read, nullptr) && read) {
                    uc_mem_write(uc_, base, bytes.data(), read);
                }
            }
            SetFilePointerEx(host, oldPos, nullptr, FILE_BEGIN);
        }
#endif
    }
    if (mappedView.shadow.empty()) {
        mappedView.shadow.resize(viewSize);
        if (viewSize) uc_mem_read(uc_, base, mappedView.shadow.data(), mappedView.shadow.size());
    }
    mappedViews_[base] = std::move(mappedView);
    lastError_ = 0;
    spdlog::info("MapViewOfFile mapping=0x{:08x} name=\"{}\" access=0x{:08x} offset={} bytes={} shared={} backing=\"{}\" -> base=0x{:08x} viewSize={}",
                 mappingHandle, mapping->second.name, desiredAccess, offset, bytesToMap,
                 mapping->second.namedShared, pathToUtf8(mapping->second.backingPath), base, viewSize);
    return base;
}

uint32_t SyntheticDllRuntime::handleUnmapViewOfFile(uint32_t baseAddress) {
    auto view = mappedViews_.find(baseAddress);
    if (view == mappedViews_.end()) {
        lastError_ = 487;
        spdlog::info("UnmapViewOfFile failed base=0x{:08x} lastError={}", baseAddress, lastError_);
        return 0;
    }
    const auto mapping = fileMappings_.find(view->second.mappingHandle);
    const std::string name = mapping == fileMappings_.end() ? std::string{} : mapping->second.name;
    const uint32_t mappingHandle = view->second.mappingHandle;
    const bool synced = syncNamedMappedView(baseAddress, view->second, true);
    spdlog::info("UnmapViewOfFile base=0x{:08x} mapping=0x{:08x} name=\"{}\" size={} synced={}",
                 baseAddress, mappingHandle, name, view->second.size, synced);
    mappedViews_.erase(view);
    const bool mappingHandleOpen = guestHandles_.find(mappingHandle) != guestHandles_.end();
    const bool hasMappedView = std::any_of(mappedViews_.begin(), mappedViews_.end(),
                                           [&](const auto& entry) {
                                               return entry.second.mappingHandle == mappingHandle;
                                           });
    if (!mappingHandleOpen && !hasMappedView) fileMappings_.erase(mappingHandle);
    lastError_ = 0;
    return 1;
}

uint32_t SyntheticDllRuntime::handleFlushViewOfFile(uint32_t baseAddress, uint32_t bytesToFlush) {
    auto view = mappedViews_.find(baseAddress);
    if (view == mappedViews_.end()) {
        lastError_ = 487;
        spdlog::info("FlushViewOfFile failed base=0x{:08x} bytes={} lastError={}",
                     baseAddress, bytesToFlush, lastError_);
        return 0;
    }
    const auto mapping = fileMappings_.find(view->second.mappingHandle);
    const uint32_t bytes = bytesToFlush ? std::min(bytesToFlush, view->second.size) : view->second.size;
    if (mapping == fileMappings_.end() || !bytes) {
        lastError_ = 0;
        spdlog::info("FlushViewOfFile ignored base=0x{:08x} bytes={} mappingKnown={} effectiveBytes={}",
                     baseAddress, bytesToFlush, mapping != fileMappings_.end(), bytes);
        return 1;
    }
    if (mapping->second.namedShared) {
        const bool synced = syncNamedMappedView(baseAddress, view->second, true);
        lastError_ = synced ? 0 : 1;
        spdlog::info("FlushViewOfFile base=0x{:08x} mapping=0x{:08x} name=\"{}\" shared=1 bytes={} effectiveBytes={} synced={}",
                     baseAddress, view->second.mappingHandle, mapping->second.name, bytesToFlush, bytes, synced);
        return synced ? 1 : 0;
    }
#if defined(_WIN32)
    auto* file = lookupGuestHandle(mapping->second.fileHandle);
    if (file && file->kind == GuestHandle::Kind::HostFile && file->hostValue) {
        std::vector<uint8_t> buffer(bytes);
        if (uc_mem_read(uc_, baseAddress, buffer.data(), buffer.size()) != UC_ERR_OK) {
            lastError_ = 487;
            return 0;
        }
        HANDLE host = reinterpret_cast<HANDLE>(file->hostValue);
        LARGE_INTEGER oldPos{};
        LARGE_INTEGER wanted{};
        wanted.QuadPart = LONGLONG(view->second.offset);
        SetFilePointerEx(host, {}, &oldPos, FILE_CURRENT);
        BOOL ok = FALSE;
        if (SetFilePointerEx(host, wanted, nullptr, FILE_BEGIN)) {
            DWORD written = 0;
            ok = WriteFile(host, buffer.data(), bytes, &written, nullptr) && written == bytes;
        }
        SetFilePointerEx(host, oldPos, nullptr, FILE_BEGIN);
        lastError_ = ok ? 0 : GetLastError();
        return ok ? 1 : 0;
    }
#endif
    lastError_ = 0;
    spdlog::info("FlushViewOfFile base=0x{:08x} mapping=0x{:08x} name=\"{}\" bytes={} effectiveBytes={}",
                 baseAddress, view->second.mappingHandle, mapping->second.name, bytesToFlush, bytes);
    return 1;
}
