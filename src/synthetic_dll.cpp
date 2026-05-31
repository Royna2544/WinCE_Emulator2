#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#endif

#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <oleauto.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorPathNotFound = 3;
constexpr uint32_t kCoredllMsgWaitForMultipleObjectsExOrdinal = 0x0367;
constexpr uint32_t kWindowStyleChild = 0x40000000u; // WS_CHILD

bool tracePrivateUiMessage(uint32_t msg) {
    return msg == 0x057c9 || // route-search handoff payload
           msg == 0x057cc || // route/result transition
           msg == 0x057ed || // route completion/update post
           msg == 0x057f5;   // route traffic/status update
}

bool traceGuestWindowMessage(uint32_t msg) {
    return msg == 0x0007 || msg == 0x0008 ||
           msg == 0x0401 ||
           msg == 0x0200 || msg == 0x0201 || msg == 0x0202 ||
           msg == 0x032f0 || tracePrivateUiMessage(msg);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#if defined(_WIN32)
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

void writeGuestFileAttributeData(uc_engine* uc, uint32_t guestAddress, const WIN32_FILE_ATTRIBUTE_DATA& data) {
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
}
#endif

uint64_t hostTickMilliseconds() {
#if defined(_WIN32)
    return GetTickCount64();
#else
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

uint64_t guestTimerKey(uint32_t hwnd, uint32_t id) {
    return (uint64_t(hwnd) << 32) | id;
}

bool pathHasWildcard(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring text = path.filename().wstring();
    return text.find(L'*') != std::wstring::npos || text.find(L'?') != std::wstring::npos;
#else
    const std::string text = path.filename().string();
    return text.find('*') != std::string::npos || text.find('?') != std::string::npos;
#endif
}

bool pathExistsForLookup(const std::filesystem::path& path) {
    std::error_code ec;
    if (pathHasWildcard(path)) {
        const std::filesystem::path parent = path.parent_path();
        return parent.empty() || std::filesystem::exists(parent, ec);
    }
    return std::filesystem::exists(path, ec);
}

bool parentExistsForLookup(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    return !parent.empty() && std::filesystem::exists(parent, ec);
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

std::string normalizeGuestDeviceName(std::string guestPath) {
    while (!guestPath.empty() && std::isspace(static_cast<unsigned char>(guestPath.front()))) guestPath.erase(guestPath.begin());
    while (!guestPath.empty() && std::isspace(static_cast<unsigned char>(guestPath.back()))) guestPath.pop_back();
    if (guestPath.rfind("\\\\.\\", 0) == 0) guestPath.erase(0, 4);
    if (!guestPath.empty() && guestPath.back() != ':') guestPath.push_back(':');
    return lowerAscii(std::move(guestPath));
}

#if defined(_WIN32)
std::wstring normalizeHostCommPort(std::string port) {
    while (!port.empty() && std::isspace(static_cast<unsigned char>(port.front()))) port.erase(port.begin());
    while (!port.empty() && std::isspace(static_cast<unsigned char>(port.back()))) port.pop_back();
    if (!port.empty() && port.back() == ':') port.pop_back();
    if (port.rfind("\\\\.\\", 0) == 0) {
        std::wstring out;
        for (char ch : port) out.push_back(wchar_t(static_cast<unsigned char>(ch)));
        return out;
    }
    for (char& ch : port) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (port.rfind("COM", 0) != 0) port = "COM" + port;
    std::wstring out = L"\\\\.\\";
    for (char ch : port) out.push_back(wchar_t(static_cast<unsigned char>(ch)));
    return out;
}

bool applySerialModeToDcb(DCB& dcb, uint32_t baud, const std::string& mode) {
    if (!baud) return false;
    std::string upper = mode.empty() ? "8N1" : mode;
    for (char& ch : upper) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (upper.size() != 3 || upper[0] < '5' || upper[0] > '8') return false;
    dcb.BaudRate = baud;
    dcb.ByteSize = BYTE(upper[0] - '0');
    switch (upper[1]) {
    case 'N': dcb.Parity = NOPARITY; dcb.fParity = FALSE; break;
    case 'O': dcb.Parity = ODDPARITY; dcb.fParity = TRUE; break;
    case 'E': dcb.Parity = EVENPARITY; dcb.fParity = TRUE; break;
    default: return false;
    }
    switch (upper[2]) {
    case '1': dcb.StopBits = ONESTOPBIT; break;
    case '2': dcb.StopBits = TWOSTOPBITS; break;
    default: return false;
    }
    dcb.fBinary = TRUE;
    return true;
}

std::string narrowAsciiLossy(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) out.push_back(ch >= 0 && ch <= 0x7f ? char(ch) : '?');
    return out;
}
#endif

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string utf8Path;
    utf8Path.resize(text.size());
    std::memcpy(utf8Path.data(), text.data(), text.size());
    return std::filesystem::path(utf8Path);
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string utf8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
#else
    return path.string();
#endif
}

bool coredllOrdinalTouchesSharedMappingBoundary(uint16_t ordinal) {
    switch (ordinal) {
    case 0x011d: // SendMessageW
    case 0x011e: // FindWindowW
    case 0x01ed: // CreateProcessW
    case 0x01f0: // Sleep
    case 0x01f1: // WaitForSingleObject
    case 0x0224: // CreateFileMappingW
    case 0x0225: // MapViewOfFile
    case 0x0226: // UnmapViewOfFile
    case 0x0227: // FlushViewOfFile
    case 0x035b: // DispatchMessageW
    case 0x0361: // PostMessageW
        return true;
    default:
        return false;
    }
}

bool coredllOrdinalUsesInlineDispatch(uint16_t ordinal) {
    switch (ordinal) {
    case 0x0006: // ExitThread
    case 0x00aa: // ReadFile virtual serial wait parking
    case 0x00f6: // CreateWindowExW synchronous create messages
    case 0x0109: // DestroyWindow synchronous destroy messages
    case 0x010b: // UpdateWindow synchronous paint messages
    case 0x011d: // CallWindowProcW
    case 0x01f0: // Sleep cooperative scheduler path
    case 0x01f1: // WaitForSingleObject cooperative scheduler path
    case 0x01f2: // WaitForMultipleObjects cooperative scheduler path
    case 0x035b: // DispatchMessageW
    case 0x035d: // GetMessageW
    case 0x0364: // SendMessageW
    case 0x039d: // CreatePatternBrush
    case 0x03af: // SetBrushOrgEx
    case 0x03cb: // GetClipBox
        return true;
    default:
        return false;
    }
}

std::string normalizedPathKey(const std::filesystem::path& path) {
    std::string text = pathToUtf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '/', '\\');
    while (text.size() > 3 && text.back() == '\\') text.pop_back();
    return lowerAscii(text);
}

bool startsWithPathKey(const std::string& pathKey, const std::string& rootKey) {
    return pathKey == rootKey ||
           (pathKey.size() > rootKey.size() &&
            pathKey.compare(0, rootKey.size(), rootKey) == 0 &&
            (rootKey.empty() || rootKey.back() == '\\' || pathKey[rootKey.size()] == '\\'));
}

std::string pathWithBackslashes(const std::filesystem::path& path) {
    std::string text = pathToUtf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '/', '\\');
    const size_t first = text.find_first_not_of('\\');
    if (first == std::string::npos) return {};
    if (first != 0) text.erase(0, first);
    return text;
}

std::string normalizeGuestRootPath(std::string text) {
    std::replace(text.begin(), text.end(), '/', '\\');
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
    while (text.size() > 1 && text.back() == '\\') text.pop_back();
    if (text.empty()) return "\\SDMMC Disk";
    if (text.front() != '\\') text.insert(text.begin(), '\\');
    return text;
}

bool isStorageMountName(const std::string& component) {
    std::string name = lowerAscii(component);
    std::replace(name.begin(), name.end(), '_', ' ');
    while (name.find("  ") != std::string::npos) name.replace(name.find("  "), 2, " ");
    return name == "sdmmc disk" || name == "sdmmc" || name == "storage card";
}

#if defined(_WIN32)
bool isRootWildcardPattern(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    while (!path.empty() && path.front() == '\\') path.erase(path.begin());
    return path == "*" || path == "*.*";
}

bool isDotDirectoryName(const wchar_t* name) {
    return name && name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0));
}

WIN32_FIND_DATAW translateGuestFindData(WIN32_FIND_DATAW data, bool rootEnumeration) {
    if (rootEnumeration &&
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !isDotDirectoryName(data.cFileName)) {
        // Windows CE marks mounted storage roots with FILE_ATTRIBUTE_TEMPORARY.
        // The target uses that bit to distinguish storage volumes from normal folders.
        data.dwFileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
    }
    return data;
}
#endif

double doubleFromGuestPair(uint32_t low, uint32_t high) {
    const uint64_t bits = (uint64_t(high) << 32) | uint64_t(low);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void setGuestDoubleReturn(uc_engine* uc, double value, uint32_t& ret) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    ret = uint32_t(bits);
    const uint32_t high = uint32_t(bits >> 32);
    uc_reg_write(uc, UC_MIPS_REG_V1, &high);
}

bool envFlagEnabled(const char* name) {
    char* rawValue = nullptr;
    size_t valueSize = 0;
    if (_dupenv_s(&rawValue, &valueSize, name) != 0 || !rawValue) return false;
    std::string value(rawValue);
    std::free(rawValue);
    if (value.empty()) return false;
    std::string text = lowerAscii(value);
    return text != "0" && text != "false" && text != "no" && text != "off";
}

}

SyntheticDllRuntime::SyntheticDllRuntime(uc_engine* uc) : uc_(uc) {
    if (!uc_) throw std::runtime_error("SyntheticDllRuntime requires a Unicorn engine");
#if defined(NDEBUG)
    diagnostics_.setDumpsEnabled(false);
#else
    diagnostics_.setDumpsEnabled(envFlagEnabled("INAVI_EMU_DUMPS"));
#endif
    uc_mem_map(uc_, heapBase_, heapLimit_ - heapBase_, UC_PROT_ALL);
    uc_mem_map(uc_, 0x00005000, 0x00001000, UC_PROT_ALL);
    initializeUserKData();
}

void SyntheticDllRuntime::initializeUserKData() {
    ceKernel_.mainThreadTls() = allocate(64 * sizeof(uint32_t), true);

    // Windows CE exposes KDataStruct at PUserKData. On non-ARM builds this is
    // 0x00005800; application code reads SH_CURTHREAD/SH_CURPROC directly.
    writeU32(0x00005800, ceKernel_.mainThreadTls());          // lpvTls
    writeU32(0x00005804, 0);                       // SH_WIN32 API set handle placeholder
    writeU32(0x00005808, ceKernel_.mainThreadPseudoHandle()); // SH_CURTHREAD
    writeU32(0x0000580c, ceKernel_.mainProcessPseudoHandle());// SH_CURPROC
    writeU32(0x00005810, 0);                       // SH_KWIN32 placeholder
}

void SyntheticDllRuntime::updateCurrentThreadKData(uint32_t currentThreadValue, uint32_t tlsBase) {
    writeU32(0x00005800, tlsBase ? tlsBase : ceKernel_.mainThreadTls());
    writeU32(0x00005808, currentThreadValue ? currentThreadValue : ceKernel_.mainThreadPseudoHandle());
    writeU32(0x0000580c, ceKernel_.mainProcessPseudoHandle());
}

bool SyntheticDllRuntime::handleEncodedKernelCall(uint32_t target, uint32_t arg0, uint32_t arg1,
                                                  uint32_t callerPc, uint32_t returnPc,
                                                  uint32_t& exitCode) {
    const auto call = CeKernel::decodeMipsKernelCall(target);
    if (!call) return false;

    if (call->kind == CeKernel::EncodedKernelCallKind::TerminateProcess) {
        exitCode = arg1;
        ceKernel_.terminateCurrentProcess(exitCode);
        quitPosted_ = true;
        spdlog::info("decoded MIPS CE kernel call TerminateProcess target=0x{:08x} "
                     "apiSet={} method={} oldEncoding={} caller=0x{:08x} return=0x{:08x} "
                     "process=0x{:08x} exitCode=0x{:08x}",
                     target, call->apiSet, call->method, call->oldEncoding,
                     callerPc, returnPc, arg0, exitCode);
        return true;
    }

    return false;
}

void SyntheticDllRuntime::setMainModulePath(std::string path) {
    if (path.empty()) return;
    hostMainModulePath_ = std::filesystem::path(path);
    hostBaseDir_ = hostMainModulePath_.parent_path();
    refreshGuestMainModulePath();
    loadMainResources(path);
}

void SyntheticDllRuntime::setMainModuleBase(uint32_t base) {
    mainModuleBase_ = base;
}

void SyntheticDllRuntime::setSdmmcHostPath(const std::filesystem::path& path) {
    sdmmcHostRoot_ = path;
    refreshGuestMainModulePath();
}

void SyntheticDllRuntime::setSerialDeviceMapPath(const std::filesystem::path& path) {
    serialDeviceMapPath_ = path;
    serialDevicesByGuest_.clear();
    defaultSerialBaud_ = 9600;
    defaultSerialMode_ = "8N1";
    if (serialDeviceMapPath_.empty()) return;

    std::ifstream input(serialDeviceMapPath_);
    if (!input) {
        throw std::runtime_error("failed to open serial device map: " + pathToUtf8(serialDeviceMapPath_));
    }

    nlohmann::json doc;
    input >> doc;
    if (!doc.is_object()) {
        throw std::runtime_error("serial device map must be a JSON object: " + pathToUtf8(serialDeviceMapPath_));
    }
    if (!doc.contains("version") || !doc["version"].is_number_integer() || doc["version"].get<int>() != 1) {
        throw std::runtime_error("unsupported serial device map version in " + pathToUtf8(serialDeviceMapPath_));
    }

    const std::set<std::string> rootFields{"version", "defaults", "devices"};
    for (const auto& item : doc.items()) {
        if (!rootFields.count(item.key())) {
            throw std::runtime_error("unknown serial device map field: " + item.key());
        }
    }

    if (doc.contains("defaults")) {
        if (!doc["defaults"].is_object()) {
            throw std::runtime_error("serial device map defaults must be an object");
        }
        const std::set<std::string> defaultFields{"baud", "mode"};
        for (const auto& item : doc["defaults"].items()) {
            if (!defaultFields.count(item.key())) {
                throw std::runtime_error("unknown serial device map defaults field: " + item.key());
            }
        }
        if (doc["defaults"].contains("baud")) defaultSerialBaud_ = doc["defaults"]["baud"].get<uint32_t>();
        if (doc["defaults"].contains("mode")) defaultSerialMode_ = doc["defaults"]["mode"].get<std::string>();
    }

    if (!doc.contains("devices")) {
        spdlog::info("serial device map: {} devices=0", pathToUtf8(serialDeviceMapPath_));
        return;
    }
    if (!doc["devices"].is_array()) {
        throw std::runtime_error("serial device map devices must be an array");
    }

    const std::set<std::string> deviceFields{"guest", "type", "backend", "host", "enabled", "note", "baud", "mode"};
    for (const auto& device : doc["devices"]) {
        if (!device.is_object()) {
            throw std::runtime_error("serial device map devices entries must be objects");
        }
        for (const auto& item : device.items()) {
            if (!deviceFields.count(item.key())) {
                throw std::runtime_error("unknown serial device map device field: " + item.key());
            }
        }
        if (!device.contains("guest") || !device["guest"].is_string()) {
            throw std::runtime_error("serial device map device is missing string field: guest");
        }
        if (!device.contains("type") || !device["type"].is_string()) {
            throw std::runtime_error("serial device map device is missing string field: type");
        }

        SerialDeviceConfig config;
        config.guest = device["guest"].get<std::string>();
        config.type = lowerAscii(device["type"].get<std::string>());
        if (config.type == "ioctl") config.type = "ioctl_device";
        config.backend = lowerAscii(device.value("backend", std::string("stub")));
        config.host = device.value("host", std::string{});
        config.enabled = device.value("enabled", false);
        config.note = device.value("note", std::string{});
        config.baud = device.value("baud", defaultSerialBaud_);
        config.mode = device.value("mode", defaultSerialMode_);

        if (config.type != "serial" && config.type != "ioctl_device") {
            throw std::runtime_error("unsupported serial device map type for " + config.guest + ": " + config.type);
        }
        if (config.type == "serial") {
            if (config.backend != "stub" && config.backend != "win32_com") {
                throw std::runtime_error("unsupported serial backend for " + config.guest + ": " + config.backend);
            }
            if (config.enabled && config.backend == "win32_com" && config.host.empty()) {
                throw std::runtime_error("enabled win32_com serial device requires host for " + config.guest);
            }
        } else if (config.backend != "stub" && config.backend != "nanduuid_return") {
            throw std::runtime_error("unsupported ioctl_device backend for " + config.guest + ": " + config.backend);
        }

        const std::string key = normalizeGuestDeviceName(config.guest);
        serialDevicesByGuest_[key] = std::move(config);
    }
    spdlog::info("serial device map: {} devices={} defaults={} {}",
                 pathToUtf8(serialDeviceMapPath_), serialDevicesByGuest_.size(),
                 defaultSerialBaud_, defaultSerialMode_);
}

void SyntheticDllRuntime::setFramebuffer(uint32_t* bgra, int width, int height) {
    framebuffer_ = bgra;
    framebufferWidth_ = width;
    framebufferHeight_ = height;
}

void SyntheticDllRuntime::setHostPresenterTargetSize(int width, int height) {
    if (width < 0 || height < 0) {
        hostPresenterTargetWidth_ = -1;
        hostPresenterTargetHeight_ = -1;
        return;
    }
    hostPresenterTargetWidth_ = width;
    hostPresenterTargetHeight_ = height;
}

void SyntheticDllRuntime::registerLoadedModule(const std::string& moduleName,
                                               const std::filesystem::path& path,
                                               uint32_t base,
                                               uint32_t imageSize,
                                               const std::map<std::string, uint32_t>& exportsByName,
                                               const std::map<uint16_t, uint32_t>& exportsByOrdinal) {
    if (!base) return;
    std::string nameKey = lowerAscii(pathToUtf8(std::filesystem::path(moduleName).filename()));
    if (nameKey.empty() && !path.empty()) nameKey = lowerAscii(pathToUtf8(path.filename()));
    LoadedModuleInfo info{nameKey, path, base, imageSize, exportsByName, exportsByOrdinal};
    if (!nameKey.empty()) loadedModulesByName_[nameKey] = info;
    if (!path.empty()) loadedModulesByPath_[lowerAscii(pathToUtf8(path))] = info;
    loadedModulesByBase_[base] = info;
}

void SyntheticDllRuntime::setGuestProcessLauncher(GuestProcessLauncher launcher) {
    guestProcessLauncher_ = std::move(launcher);
}

uint32_t SyntheticDllRuntime::threadExitStubAddress() const {
    return threadExitStub_;
}

void SyntheticDllRuntime::hookCode(uc_engine* uc, uint64_t address, uint32_t, void* user) {
    auto* runtime = static_cast<SyntheticDllRuntime*>(user);
    auto it = runtime->exportsByAddress_.find(uint32_t(address));
    if (it == runtime->exportsByAddress_.end()) return;
    runtime->dispatch(it->second);
}

void SyntheticDllRuntime::beginInteractiveSlice(std::chrono::milliseconds wallBudget,
                                                const char* reason,
                                                uint64_t instructionBudget) {
    interactiveSliceActive_ = true;
    interactiveSliceStopRequested_ = false;
    interactiveSliceBlockCounter_ = 0;
    interactiveSliceInstructionBudget_ = instructionBudget;
    interactiveSliceReason_ = reason ? reason : "";
    interactiveSliceDeadline_ = std::chrono::steady_clock::now() + wallBudget;
}

void SyntheticDllRuntime::endInteractiveSlice() {
    interactiveSliceActive_ = false;
}

void SyntheticDllRuntime::hookBasicBlock(uc_engine* uc, uint64_t address, uint32_t, void* user) {
    auto* runtime = static_cast<SyntheticDllRuntime*>(user);
    if (!runtime || !runtime->interactiveSliceActive_) return;
    const uint32_t blockCount = ++runtime->interactiveSliceBlockCounter_;
    if ((blockCount & 0x000fu) != 0) return;

    bool hostQueuePending = false;
#if defined(_WIN32)
    hostQueuePending = HIWORD(GetQueueStatus(QS_ALLINPUT)) != 0;
#endif
    if (!hostQueuePending && std::chrono::steady_clock::now() < runtime->interactiveSliceDeadline_) return;

    if (!runtime->interactiveSliceStopRequested_) {
        uint32_t pc = 0;
        uint32_t ra = 0;
        uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
        uc_reg_read(uc, UC_MIPS_REG_RA, &ra);
        if (hostQueuePending) {
            spdlog::debug("guest slice watchdog stop reason={} stopCause=host-queue activeThread=0x{:08x} budget={} block=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={}",
                          runtime->interactiveSliceReason_, runtime->ceKernel_.activeGuestThread(),
                          runtime->interactiveSliceInstructionBudget_, uint32_t(address), pc, ra,
                          runtime->ceGwe_.messageCount());
        } else {
            const bool messageTransfer =
                runtime->interactiveSliceReason_ == "message-transfer" ||
                runtime->interactiveSliceReason_ == "blocked-main-message-transfer";
            if (messageTransfer) runtime->diagnostics_.incrementMessageTransferWatchdogStops();
            const uint64_t nowMs = hostTickMilliseconds();
            const bool rateLimitedMessageTransfer =
                messageTransfer &&
                !runtime->diagnostics_.shouldLog("message-transfer-latency", nowMs, 1000);
            if (rateLimitedMessageTransfer) {
                spdlog::debug("guest slice watchdog stop reason={} stopCause=deadline activeThread=0x{:08x} budget={} block=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={}",
                              runtime->interactiveSliceReason_, runtime->ceKernel_.activeGuestThread(),
                              runtime->interactiveSliceInstructionBudget_, uint32_t(address), pc, ra,
                              runtime->ceGwe_.messageCount());
            } else {
                const uint32_t owner = !runtime->ceGwe_.pendingMessageTransfers().empty()
                    ? runtime->ceGwe_.pendingMessageTransfers().back().ownerThread
                    : runtime->ceGwe_.oldestPendingOwner().value_or(0);
                const auto ownerQueue = runtime->ceGwe_.ownerQueueSnapshot(owner);
                spdlog::info("guest slice watchdog stop reason={} stopCause=deadline activeThread=0x{:08x} budget={} block=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={} owner=0x{:08x} ownerPosted={} ownerSent={} ownerInput={} ownerTimer={} transferStops={}",
                             runtime->interactiveSliceReason_, runtime->ceKernel_.activeGuestThread(),
                             runtime->interactiveSliceInstructionBudget_, uint32_t(address), pc, ra,
                             runtime->ceGwe_.messageCount(), owner, ownerQueue.posted, ownerQueue.sent,
                             ownerQueue.input, ownerQueue.timers,
                             runtime->diagnostics_.messageTransferWatchdogStops());
            }
        }
        runtime->interactiveSliceStopRequested_ = true;
    }
    uc_emu_stop(uc);
}

uint32_t SyntheticDllRuntime::makeGuestHandle(GuestHandle handle) {
    return ceKernel_.makeHandle(handle);
}

SyntheticDllRuntime::GuestHandle* SyntheticDllRuntime::lookupGuestHandle(uint32_t guestHandle) {
    return ceKernel_.lookupHandle(guestHandle);
}

uint32_t SyntheticDllRuntime::closeGuestHandle(uint32_t guestHandle) {
    auto it = ceKernel_.handles().find(guestHandle);
    if (it == ceKernel_.handles().end()) {
        lastError_ = 6; // ERROR_INVALID_HANDLE
        return 0;
    }
    if (it->second.kind == GuestHandle::Kind::GuestWindow) {
        auto window = ceGwe_.windows().find(guestHandle);
        if (window != ceGwe_.windows().end()) destroyHostWindow(window->second);
        ceGwe_.windows().erase(guestHandle);
        crossProcessBroker_.forgetImportedWindow(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestDc) {
        ceMgdi_.dcs().erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestBrush) {
        ceMgdi_.brushes().erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestFont) {
        ceMgdi_.fonts().erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestRegistryKey) {
        registryHandles_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostFile ||
               it->second.kind == GuestHandle::Kind::HostSerialDevice ||
               it->second.kind == GuestHandle::Kind::GuestSerialDevice) {
        fileHandleDebugNames_.erase(guestHandle);
        ceDevice_.unregisterSerial(guestHandle);
        fileReadCounts_.erase(guestHandle);
        fileSeekCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostCrtFile) {
        fileHandleDebugNames_.erase(guestHandle);
        fileReadCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostFind ||
               it->second.kind == GuestHandle::Kind::GuestFind) {
        fileHandleDebugNames_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostWaveOut) {
        clearHostAudioBackend(guestHandle);
        ceAudio_.closeStream(guestHandle);
        ceAudio_.waveOutStates().erase(guestHandle);
        ceAudio_.hostWaveBuffers().clear();
    } else if (it->second.kind == GuestHandle::Kind::GuestFileMapping) {
        const bool hasMappedView = std::any_of(ceIpc_.mappedViews().begin(), ceIpc_.mappedViews().end(),
                                               [&](const auto& entry) {
                                                   return entry.second.mappingHandle == guestHandle;
                                               });
        if (!hasMappedView) ceIpc_.fileMappings().erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostBitmap) {
        ceMgdi_.bitmaps().erase(guestHandle);
        if (it->second.filePointer) releaseAllocation(it->second.filePointer);
    } else if (it->second.kind == GuestHandle::Kind::GuestThread) {
        auto thread = ceKernel_.threads().find(guestHandle);
        if (thread != ceKernel_.threads().end()) {
            if (thread->second.state == GuestThreadRunState::Terminated) {
                releaseAllocation(thread->second.stackBase);
                releaseAllocation(thread->second.tlsBase);
                ceKernel_.threads().erase(thread);
                if (ceKernel_.lastScheduledGuestThread() == guestHandle) ceKernel_.lastScheduledGuestThread() = 0;
            } else {
                spdlog::info("CloseHandle released live guest thread handle=0x{:08x} state={} without terminating scheduler thread",
                             guestHandle, static_cast<int>(thread->second.state));
            }
        }
    }
    if (it->second.kind == GuestHandle::Kind::GuestHeap) {
        lastError_ = 6; // ERROR_INVALID_HANDLE; heaps are destroyed via HeapDestroy.
        return 0;
    }
#if defined(_WIN32)
    if (it->second.hostValue) {
        if (it->second.kind == GuestHandle::Kind::HostSocket) {
            closesocket(SOCKET(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostFind) {
            FindClose(reinterpret_cast<HANDLE>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostCrtFile) {
            std::fclose(reinterpret_cast<FILE*>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostWaveIn) {
            closeHostWaveInHandle(it->second.hostValue);
        } else if (it->second.kind == GuestHandle::Kind::HostWaveOut) {
            closeHostWaveOutHandle(it->second.hostValue);
        } else if (it->second.kind == GuestHandle::Kind::HostMenu) {
            DestroyMenu(reinterpret_cast<HMENU>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostAccelerator) {
            DestroyAcceleratorTable(reinterpret_cast<HACCEL>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostIcon) {
            DestroyIcon(reinterpret_cast<HICON>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostCursor) {
            // Shared cursors returned by LoadCursorW are owned by the host.
        } else if (it->second.kind == GuestHandle::Kind::HostBitmap) {
            DeleteObject(reinterpret_cast<HGDIOBJ>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostRegion) {
            DeleteObject(reinterpret_cast<HRGN>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostComInterface) {
            reinterpret_cast<IUnknown*>(it->second.hostValue)->Release();
        } else {
            CloseHandle(reinterpret_cast<HANDLE>(it->second.hostValue));
        }
    }
#endif
    ceKernel_.handles().erase(it);
    lastError_ = 0;
    return 1;
}

uint32_t SyntheticDllRuntime::openGuestSerialDevice(const std::string& guestPath, uint32_t access, uint32_t share) {
    const uint32_t ra = reg(UC_MIPS_REG_RA);
    const std::string deviceKey = normalizeGuestDeviceName(guestPath);
    auto registerVirtualSerial = [&](uint32_t guest,
                                     const SerialDeviceConfig* config,
                                     std::string debugName,
                                     bool virtualNoDataBackend) {
        CeDevice::SerialState state{};
        state.handle = guest;
        state.guestName = guestPath;
        state.deviceType = config ? config->type : std::string{"serial"};
        state.hostName = config ? config->host : std::string{};
        state.backend = config ? config->backend : std::string{"virtual"};
        state.mode.baud = config && config->baud ? config->baud : defaultSerialBaud_;
        state.mode.byteSize = 8;
        state.mode.parity = 0;
        state.mode.stopBits = 0;
        state.timeouts.readIntervalTimeout = 0xffffffffu;
        state.virtualNoDataBackend = virtualNoDataBackend;
        ceDevice_.registerSerial(std::move(state));
        fileHandleDebugNames_[guest] = std::move(debugName);
    };
    const auto mapped = serialDevicesByGuest_.find(deviceKey);
    if (mapped != serialDevicesByGuest_.end()) {
        const SerialDeviceConfig& config = mapped->second;
        const std::string note = config.note.empty() ? std::string{} : " (" + config.note + ")";
        if (!config.enabled || config.backend == "stub" || config.type == "ioctl_device") {
            const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
            const std::string state = config.enabled ? config.backend : std::string("disabled");
            registerVirtualSerial(guest, &config, guestPath + " -> " + config.type + " " + state + note, true);
            lastError_ = 0;
            spdlog::info("CreateFileW guest device=\"{}\" mapped type={} backend={} enabled={} guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} ra=0x{:08x}{}",
                         guestPath, config.type, config.backend, config.enabled ? 1 : 0,
                         guest, access, share, ra, note);
            return guest;
        }

#if defined(_WIN32)
        const std::wstring hostPort = normalizeHostCommPort(config.host);
        const DWORD desiredAccess = access ? access : (GENERIC_READ | GENERIC_WRITE);
        const std::string displayName = narrowAsciiLossy(hostPort);
        HANDLE host = INVALID_HANDLE_VALUE;
        DWORD openError = 0;
        constexpr int kHostSerialOpenAttempts = 1;
        constexpr DWORD kHostSerialRetryDelayMs = 250;
        for (int attempt = 1; attempt <= kHostSerialOpenAttempts; ++attempt) {
            host = CreateFileW(hostPort.c_str(), desiredAccess, share, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
            if (host != INVALID_HANDLE_VALUE) {
                if (attempt > 1) {
                    spdlog::info("CreateFileW guest device=\"{}\" host=\"{}\" opened after retry attempt={}",
                                 guestPath, displayName, attempt);
                }
                break;
            }

            openError = GetLastError();
            const bool retryable = openError == ERROR_ACCESS_DENIED ||
                                   openError == ERROR_FILE_NOT_FOUND ||
                                   openError == ERROR_PATH_NOT_FOUND;
            if (!retryable || attempt == kHostSerialOpenAttempts) break;
            spdlog::debug("CreateFileW guest device=\"{}\" host=\"{}\" retrying after lastError={} attempt={}/{}",
                          guestPath, displayName, openError, attempt, kHostSerialOpenAttempts);
            Sleep(kHostSerialRetryDelayMs);
        }
        if (host != INVALID_HANDLE_VALUE) {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (GetCommState(host, &dcb) && applySerialModeToDcb(dcb, config.baud, config.mode)) {
                if (!SetCommState(host, &dcb)) {
                    spdlog::warn("CreateFileW guest device=\"{}\" host=\"{}\" SetCommState failed lastError={} requested={} {}",
                                 guestPath, displayName, GetLastError(), config.baud, config.mode);
                }
            }
            COMMTIMEOUTS timeouts{};
            timeouts.ReadIntervalTimeout = MAXDWORD;
            SetCommTimeouts(host, &timeouts);
            const uint32_t guest = makeGuestHandle({GuestHandle::Kind::HostSerialDevice,
                                                    reinterpret_cast<uintptr_t>(host), 0});
            fileHandleDebugNames_[guest] = guestPath + " -> " + displayName + note;
            lastError_ = 0;
            spdlog::info("CreateFileW guest device=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} ra=0x{:08x} serial={} {}{}",
                         guestPath, displayName, guest, desiredAccess, share,
                         ra, config.baud, config.mode, note);
            return guest;
        }
        spdlog::warn("CreateFileW guest device=\"{}\" host=\"{}\" unavailable lastError={} ra=0x{:08x}; using virtual serial no-data backend{}",
                     guestPath, displayName, openError, ra, note);
        const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
        registerVirtualSerial(guest, &config, guestPath + " -> " + displayName + " virtual no-data" + note, true);
        lastError_ = 0;
        spdlog::info("CreateFileW guest device=\"{}\" guestHandle=0x{:08x} virtual serial no-data backend access=0x{:08x} share=0x{:08x} ra=0x{:08x}",
                     guestPath, guest, access, share, ra);
        return guest;
#else
        spdlog::warn("CreateFileW guest device=\"{}\" backend=win32_com unavailable on this host ra=0x{:08x}; using virtual serial no-data backend{}",
                     guestPath, ra, note);
        const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
        registerVirtualSerial(guest, &config, guestPath + " -> virtual no-data" + note, true);
        lastError_ = 0;
        spdlog::info("CreateFileW guest device=\"{}\" guestHandle=0x{:08x} virtual serial no-data backend access=0x{:08x} share=0x{:08x} ra=0x{:08x}",
                     guestPath, guest, access, share, ra);
        return guest;
#endif
    }
    const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
    registerVirtualSerial(guest, nullptr, guestPath + " -> virtual no-data", true);
    lastError_ = 0;
    spdlog::info("CreateFileW guest device=\"{}\" guestHandle=0x{:08x} virtual serial no-data backend access=0x{:08x} share=0x{:08x} ra=0x{:08x}",
                 guestPath, guest, access, share, ra);
    return guest;
}

uint32_t SyntheticDllRuntime::dispatchDeviceIoControl(uint32_t handleValue, uint32_t controlCode,
                                                      uint32_t inPtr, uint32_t inSize) {
    auto* handle = lookupGuestHandle(handleValue);
    const uint32_t outPtr = stackArg(4);
    const uint32_t outSize = stackArg(5);
    const uint32_t bytesReturnedPtr = stackArg(6);
    const uint32_t overlappedPtr = stackArg(7);
    if (bytesReturnedPtr) writeU32(bytesReturnedPtr, 0);
    if (!handle) {
        lastError_ = 6;
        return 0;
    }
    if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
        auto debugName = fileHandleDebugNames_.find(handleValue);
        const std::string name = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
        const CeDevice::SerialState* serial = ceDevice_.serialState(handleValue);
        if (serial &&
            serial->open &&
            serial->deviceType == "ioctl_device" &&
            serial->backend == "nanduuid_return" &&
            (controlCode == 0xa00100ccu || controlCode == 0xa00100d0u) &&
            outPtr && outSize) {
            std::array<uint8_t, 16> uid{};
            std::ifstream file(sdmmcHostRoot_ / "Device.uid", std::ios::binary);
            file.read(reinterpret_cast<char*>(uid.data()), uid.size());
            const std::streamsize available = file.gcount();
            if (available > 0) {
                if (controlCode == 0xa00100ccu) {
                    uint32_t compactId = 0;
                    uint32_t digits = 0;
                    for (uint8_t ch : uid) {
                        if (!std::isdigit(ch) || digits >= 8) break;
                        compactId = compactId * 10u + uint32_t(ch - '0');
                        ++digits;
                    }
                    if (outSize < sizeof(compactId)) {
                        lastError_ = 122;
                        return 0;
                    }
                    writeU32(outPtr, compactId);
                    if (bytesReturnedPtr) writeU32(bytesReturnedPtr, sizeof(compactId));
                    lastError_ = 0;
                    spdlog::info("DeviceIoControl ioctl backend=NANDUUID_RETURN guest=\"{}\" code=0x{:08x} outSize={} transferred=4 source=\"{}\" compactId={:08}",
                                 serial->guestName, controlCode, outSize,
                                 pathToUtf8(sdmmcHostRoot_ / "Device.uid"), compactId);
                    return 1;
                }
                const uint32_t transferred = std::min<uint32_t>(outSize, uint32_t(uid.size()));
                uc_mem_write(uc_, outPtr, uid.data(), transferred);
                if (bytesReturnedPtr) writeU32(bytesReturnedPtr, transferred);
                lastError_ = 0;
                spdlog::info("DeviceIoControl ioctl backend=NANDUUID_RETURN guest=\"{}\" code=0x{:08x} outSize={} transferred={} source=\"{}\" value=\"{}\"",
                             serial->guestName, controlCode, outSize, transferred,
                             pathToUtf8(sdmmcHostRoot_ / "Device.uid"),
                             std::string(reinterpret_cast<const char*>(uid.data()),
                                         reinterpret_cast<const char*>(uid.data()) + std::min<std::streamsize>(available, uid.size())));
                return 1;
            }
            lastError_ = 2;
            spdlog::warn("DeviceIoControl ioctl backend=NANDUUID_RETURN guest=\"{}\" code=0x{:08x} missing source=\"{}\"",
                         serial->guestName, controlCode, pathToUtf8(sdmmcHostRoot_ / "Device.uid"));
            return 0;
        }
        lastError_ = 120;
        spdlog::info("DeviceIoControl guest device handle=0x{:08x} name=\"{}\" code=0x{:08x} inSize={} outSize={} -> 0 lastError={}",
                     handleValue, name,
                     controlCode, inSize, outSize, lastError_);
        return 0;
    }
    if ((handle->kind != GuestHandle::Kind::HostFile &&
         handle->kind != GuestHandle::Kind::HostSerialDevice) ||
        !handle->hostValue) {
        lastError_ = 6;
        return 0;
    }
    if (overlappedPtr) {
        lastError_ = 120;
        return 0;
    }
    if ((inSize && !inPtr) || (outSize && !outPtr)) {
        lastError_ = 87;
        return 0;
    }
#if defined(_WIN32)
    std::vector<uint8_t> inBytes(inSize);
    std::vector<uint8_t> outBytes(outSize);
    if (inSize) uc_mem_read(uc_, inPtr, inBytes.data(), inBytes.size());
    DWORD transferred = 0;
    const BOOL ok = DeviceIoControl(reinterpret_cast<HANDLE>(handle->hostValue),
                                    controlCode,
                                    inSize ? inBytes.data() : nullptr,
                                    inSize,
                                    outSize ? outBytes.data() : nullptr,
                                    outSize,
                                    &transferred,
                                    nullptr);
    if (ok && transferred && outPtr) {
        uc_mem_write(uc_, outPtr, outBytes.data(), std::min<uint32_t>(transferred, outSize));
    }
    if (bytesReturnedPtr) writeU32(bytesReturnedPtr, transferred);
    lastError_ = ok ? 0 : GetLastError();
    auto debugName = fileHandleDebugNames_.find(handleValue);
    spdlog::info("DeviceIoControl host handle=0x{:08x} path=\"{}\" code=0x{:08x} inSize={} outSize={} transferred={} -> {} lastError={}",
                 handleValue, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
                 controlCode, inSize, outSize, transferred, ok ? 1 : 0, lastError_);
    return ok ? 1 : 0;
#else
    lastError_ = 120;
    return 0;
#endif
}

uint32_t SyntheticDllRuntime::makeGuestComProxy(uintptr_t hostInterface) {
    if (!hostInterface || !comQueryInterfaceStub_ || !comAddRefStub_ || !comReleaseStub_) return 0;
    if (!comProxyVtable_) {
        comProxyVtable_ = allocate(12, true);
        writeU32(comProxyVtable_, comQueryInterfaceStub_);
        writeU32(comProxyVtable_ + 4, comAddRefStub_);
        writeU32(comProxyVtable_ + 8, comReleaseStub_);
    }
    const uint32_t guestHandle = makeGuestHandle({GuestHandle::Kind::HostComInterface, hostInterface, 0});
    const uint32_t object = allocate(8, true);
    writeU32(object, comProxyVtable_);
    writeU32(object + 4, guestHandle);
    return object;
}

uint32_t SyntheticDllRuntime::handleWNetGetUserW(uint32_t, uint32_t userName, uint32_t lengthPtr) {
    if (!userName || !lengthPtr) {
        lastError_ = 87; // ERROR_INVALID_PARAMETER
        return 87;
    }
    uint32_t capacity = 0;
    if (uc_mem_read(uc_, lengthPtr, &capacity, sizeof(capacity)) != UC_ERR_OK || !capacity) {
        lastError_ = 87;
        return 87;
    }
    std::string user;
#if defined(_WIN32)
    std::vector<wchar_t> hostName(std::max<uint32_t>(capacity, 256));
    DWORD hostLength = DWORD(hostName.size());
    if (!GetUserNameW(hostName.data(), &hostLength)) {
        const DWORD error = GetLastError();
        writeU32(lengthPtr, hostLength);
        lastError_ = error;
        return error;
    }
    for (DWORD i = 0; i + 1 < hostLength && hostName[i]; ++i) {
        user.push_back(hostName[i] < 0x80 ? char(hostName[i]) : '?');
    }
#else
    const char* envUser = std::getenv("USER");
    user = envUser && *envUser ? envUser : "user";
#endif
    const uint32_t required = uint32_t(user.size() + 1);
    writeU32(lengthPtr, required);
    if (capacity < required) {
        lastError_ = 122; // ERROR_INSUFFICIENT_BUFFER
        return 122;
    }
    writeUtf16(userName, user, capacity);
    lastError_ = 0;
    return 0;
}

uint32_t SyntheticDllRuntime::handleSystemParametersInfoW(uint32_t action,
                                                          uint32_t uiParam,
                                                          uint32_t pvParam,
                                                          uint32_t flags) {
    char actionHex[9]{};
    std::snprintf(actionHex, sizeof(actionHex), "%08x", action);
    const nlohmann::json* configured =
        registryValue("hklm\\system\\emulator\\systemparametersinfo", actionHex);
    if (!configured && action == 0x00000101u) {
        configured = registryValue("hklm\\system\\emulator\\systemparametersinfo", "platformtype");
    }
    if (!configured && action == 0x00000102u) {
        configured = registryValue("hklm\\system\\emulator\\systemparametersinfo", "oeminfo");
    }

    std::string text;
    if (configured) {
        const auto data = configured->find("data");
        if (data != configured->end() && data->is_string()) text = data->get<std::string>();
    }

    uint32_t result = 0;
    if (!text.empty() && pvParam && uiParam) {
        writeUtf16(pvParam, text, uiParam);
        lastError_ = 0;
        result = 1;
    } else {
        lastError_ = configured ? 87 : 120;
    }

    spdlog::info("SystemParametersInfoW action=0x{:08x} uiParam={} pvParam=0x{:08x} flags=0x{:08x} -> {} lastError={} data=\"{}\"",
                 action, uiParam, pvParam, flags, result, lastError_, text);
    return result;
}

uint32_t SyntheticDllRuntime::handleGetDeviceCaps(uint32_t, uint32_t index) {
    switch (index) {
    case 8: return 32; // BITSPIXEL
    case 10: return 0xffffffffu; // NUMCOLORS
    case 12:
    case 14:
        return 1;
    case 88: return framebufferWidth_ > 0 ? uint32_t(framebufferWidth_) : 800;
    case 90: return framebufferHeight_ > 0 ? uint32_t(framebufferHeight_) : 480;
    default:
#if defined(_WIN32)
        HDC dc = ::GetDC(nullptr);
        const uint32_t result = dc ? uint32_t(::GetDeviceCaps(dc, int(index))) : 0;
        if (dc) ::ReleaseDC(nullptr, dc);
        return result;
#else
        return 0;
#endif
    }
}

bool SyntheticDllRuntime::writeGuestMessage(uint32_t address, const GuestMessage& message) const {
    if (!address) return false;
    writeU32(address, message.hwnd);
    writeU32(address + 4, message.message);
    writeU32(address + 8, message.wParam);
    writeU32(address + 12, message.lParam);
    writeU32(address + 16, message.time);
    writeU32(address + 20, message.x);
    writeU32(address + 24, message.y);
    return true;
}

void SyntheticDllRuntime::refreshGuestMainModulePath() {
    if (hostMainModulePath_.empty()) return;

    const std::string hostKey = normalizedPathKey(hostMainModulePath_);
    if (!sdmmcHostRoot_.empty()) {
        const std::string rootKey = normalizedPathKey(sdmmcHostRoot_);
        if (!rootKey.empty() && startsWithPathKey(hostKey, rootKey)) {
            std::filesystem::path relative = hostMainModulePath_.lexically_relative(sdmmcHostRoot_);
            std::string relativeText = pathWithBackslashes(relative);
            if (!relativeText.empty() && relativeText != ".") {
                std::filesystem::path relativePath = pathFromUtf8(relativeText);
                auto first = relativePath.begin();
                const bool alreadyMounted = first != relativePath.end() && isStorageMountName(pathToUtf8(*first));
                mainModulePath_ = alreadyMounted ? "\\" + relativeText : sdmmcGuestRoot_ + "\\" + relativeText;
                spdlog::info("guest module path: {}", mainModulePath_);
                return;
            }
        }
    }

    std::string fileName = pathToUtf8(hostMainModulePath_.filename());
    if (!fileName.empty()) mainModulePath_ = "\\" + fileName;
    spdlog::info("guest module path: {}", mainModulePath_);
}

std::filesystem::path SyntheticDllRuntime::resolveGuestPath(const std::string& guestPath) const {
    if (guestPath.empty()) return {};

    std::string normalized = guestPath;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    if (normalized.size() > 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) &&
        normalized[1] == ':') {
        return pathFromUtf8(normalized);
    }
    const bool rootRelative = !normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/');

    const size_t firstRelativeChar = normalized.find_first_not_of("\\/");
    if (firstRelativeChar == std::string::npos) {
        normalized.clear();
    } else if (firstRelativeChar != 0) {
        normalized.erase(0, firstRelativeChar);
    }

    const std::filesystem::path relative = pathFromUtf8(normalized);
    auto resolveFromSdmmcRoot = [&](const std::filesystem::path& guestRelative) -> std::filesystem::path {
        if (sdmmcHostRoot_.empty()) return {};
        const std::filesystem::path candidate =
            guestRelative.empty() ? sdmmcHostRoot_ : sdmmcHostRoot_ / guestRelative;
        if (pathExistsForLookup(candidate) || parentExistsForLookup(candidate)) return candidate;
        return candidate;
    };
    auto resolveFromHostBase = [&](const std::filesystem::path& guestRelative) -> std::filesystem::path {
        if (hostBaseDir_.empty()) return guestRelative;
        auto first = guestRelative.begin();
        if (first != guestRelative.end() &&
            lowerAscii(pathToUtf8(*first)) == lowerAscii(pathToUtf8(hostBaseDir_.filename()))) {
            return hostBaseDir_.parent_path() / guestRelative;
        }
        return hostBaseDir_ / guestRelative;
    };
    auto stripMountedStoragePrefix = [](const std::filesystem::path& guestRelative) -> std::filesystem::path {
        auto it = guestRelative.begin();
        if (it == guestRelative.end() || !isStorageMountName(pathToUtf8(*it))) return guestRelative;

        std::filesystem::path withoutMount;
        for (++it; it != guestRelative.end(); ++it) withoutMount /= *it;
        return withoutMount;
    };
    if (rootRelative && !sdmmcHostRoot_.empty()) {
        const std::string relativeText = pathWithBackslashes(relative);
        std::string sdmmcRelative = sdmmcGuestRoot_;
        const size_t firstSdmmcRelativeChar = sdmmcRelative.find_first_not_of('\\');
        if (firstSdmmcRelativeChar == std::string::npos) {
            sdmmcRelative.clear();
        } else if (firstSdmmcRelativeChar != 0) {
            sdmmcRelative.erase(0, firstSdmmcRelativeChar);
        }
        if (!sdmmcRelative.empty() &&
            startsWithPathKey(lowerAscii(relativeText), lowerAscii(sdmmcRelative))) {
            std::string withoutRoot = relativeText.size() == sdmmcRelative.size()
                ? std::string{}
                : relativeText.substr(sdmmcRelative.size() + 1);
            return resolveFromSdmmcRoot(pathFromUtf8(withoutRoot));
        }

        const std::filesystem::path direct = resolveFromSdmmcRoot(relative);
        if (pathExistsForLookup(direct) || parentExistsForLookup(direct)) return direct;

        const std::filesystem::path withoutMount = stripMountedStoragePrefix(relative);
        if (!withoutMount.empty() && withoutMount != relative) return resolveFromSdmmcRoot(withoutMount);

        return direct;
    }
    if (!sdmmcHostRoot_.empty()) {
        const std::filesystem::path withoutMount = stripMountedStoragePrefix(relative);
        if (!withoutMount.empty() && withoutMount != relative) {
            return resolveFromSdmmcRoot(withoutMount);
        }
        return resolveFromSdmmcRoot(relative);
    }
    return resolveFromHostBase(relative);
}

bool SyntheticDllRuntime::isUnderFileSystemRoot(const std::filesystem::path& path) const {
    if (sdmmcHostRoot_.empty()) return false;
    const std::string pathKey = normalizedPathKey(path);
    const std::string rootKey = normalizedPathKey(sdmmcHostRoot_);
    if (rootKey.empty()) return false;
    if (pathKey == rootKey) return true;
    if (pathKey.size() > rootKey.size() && pathKey.compare(0, rootKey.size(), rootKey) == 0 &&
        pathKey[rootKey.size()] == '\\') {
        return true;
    }
    return false;
}

uint32_t SyntheticDllRuntime::normalizeVirtualFileMiss(const std::filesystem::path& hostPath, uint32_t error) const {
    if (error == kErrorPathNotFound && !hostPath.empty() && isUnderFileSystemRoot(hostPath)) {
        return kErrorFileNotFound;
    }
    return error;
}

void SyntheticDllRuntime::dispatch(ExportEntry& entry) {
    auto& mutableEntry = entry;
    entry.calls++;
    if (!mutableEntry.ordinalHandler && mutableEntry.calls == 1) {
        if (const auto* ordinalHandler = findOrdinalHandler(mutableEntry);
            ordinalHandler && ordinalHandler->handler) {
            mutableEntry.ordinalHandler = ordinalHandler->handler;
            mutableEntry.code = ordinalHandler->code;
            if (mutableEntry.name.empty() && ordinalHandler->name) {
                mutableEntry.name = ordinalHandler->name;
            }
        }
    }
    std::string generatedName;
    const std::string& name = mutableEntry.name.empty()
        ? (generatedName = "#" + std::to_string(mutableEntry.ordinal))
        : mutableEntry.name;
    const uint32_t a0 = reg(UC_MIPS_REG_A0);
    const uint32_t a1 = reg(UC_MIPS_REG_A1);
    const uint32_t a2 = reg(UC_MIPS_REG_A2);
    const uint32_t a3 = reg(UC_MIPS_REG_A3);
    const uint32_t ra = reg(UC_MIPS_REG_RA);
    const GuestCallArgs args{a0, a1, a2, a3, ra};
    const bool isCoredll = mutableEntry.moduleKind == SyntheticModuleKind::Coredll;
    const uint16_t ordinal = mutableEntry.ordinal;
    const uint32_t pc = reg(UC_MIPS_REG_PC);
    const bool syncSharedMappings = isCoredll && coredllOrdinalTouchesSharedMappingBoundary(ordinal);
    if (syncSharedMappings) {
        syncNamedMappedViews();
    }
    if (mutableEntry.calls <= 128) {
        spdlog::debug("synthetic {}!{} call {} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} ra=0x{:08x}",
                      mutableEntry.moduleName, name, mutableEntry.calls, a0, a1, a2, a3, ra);
    }

    const bool continuationPc =
        isCoredll &&
        ((destroyWindowContinuationStub_ && pc == destroyWindowContinuationStub_) ||
         (createWindowContinuationStub_ && pc == createWindowContinuationStub_) ||
         (blockingApiContinuationStub_ && pc == blockingApiContinuationStub_) ||
         (updateWindowContinuationStub_ && pc == updateWindowContinuationStub_) ||
         (messageTransferContinuationStub_ && pc == messageTransferContinuationStub_) ||
         (threadExitStub_ && pc == threadExitStub_));
    if (mutableEntry.ordinalHandler &&
        !continuationPc &&
        (!isCoredll || !coredllOrdinalUsesInlineDispatch(ordinal))) {
        uint32_t ret = 0;
        const bool handled = (this->*mutableEntry.ordinalHandler)(mutableEntry.code, args, ret);
        if (handled) {
            if (mutableEntry.calls <= 128) {
                spdlog::debug("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, ret);
            }
            const uint32_t returnPc = normalizeGuestCodeAddress(ra, name.c_str());
            setReg(UC_MIPS_REG_V0, ret);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(returnPc));
            setReg(UC_MIPS_REG_PC, returnPc);
            if (syncSharedMappings) syncNamedMappedViews();
            pumpHostMessages();
            cooperateGuestThreadsAfterCall(name);
            return;
        }
    }

    auto finishImmediateReturn = [&](uint32_t value, bool cooperateThreads = false) {
        if (mutableEntry.calls <= 128) {
            spdlog::debug("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, value);
        }
        const uint32_t returnPc = normalizeGuestCodeAddress(ra, name.c_str());
        setReg(UC_MIPS_REG_V0, value);
        setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(returnPc));
        setReg(UC_MIPS_REG_PC, returnPc);
        if (syncSharedMappings) syncNamedMappedViews();
        pumpHostMessages();
        if (cooperateThreads) cooperateGuestThreadsAfterCall(name);
    };

    auto translatedWndProc = [&](uint32_t wndProc, const char* why) {
        constexpr uint32_t kCeProcessSlotSize = 0x02000000u;
        if (!isGuestRangeReadable(wndProc, 4)) {
            const uint32_t activeSlotProc = wndProc & (kCeProcessSlotSize - 1);
            if (activeSlotProc != wndProc && isGuestRangeReadable(activeSlotProc, 4)) {
                spdlog::info("synthetic coredll.dll!{} translated slot WNDPROC 0x{:08x} -> 0x{:08x}",
                             why, wndProc, activeSlotProc);
                wndProc = activeSlotProc;
            }
        }
        return normalizeGuestCodeAddress(wndProc, why);
    };
    auto finalizeDestroyedWindow = [&](uint32_t hwnd,
                                       std::optional<bool> visibleOverride = std::nullopt,
                                       std::optional<uint32_t> parentOverride = std::nullopt) {
        auto it = ceGwe_.windows().find(hwnd);
        if (it == ceGwe_.windows().end()) return;
        const bool wasVisible = visibleOverride.value_or(it->second.visible);
        const uint32_t parent = parentOverride.value_or(it->second.parent);
        const bool exposesCoveredWindows =
            wasVisible && isOwnedPopupWindow(hwnd) && guestWindowCoversFramebuffer(hwnd);
        for (auto timer = ceGwe_.timers().begin(); timer != ceGwe_.timers().end();) {
            if (timer->second.hwnd == hwnd) timer = ceGwe_.timers().erase(timer);
            else ++timer;
        }
        if (focusedWindow_ == hwnd) focusedWindow_ = 0;
        if (capturedWindow_ == hwnd) capturedWindow_ = 0;
        if (hostPointerCaptureWindow_ == hwnd) hostPointerCaptureWindow_ = 0;
        if (pendingSyntheticChildButtonUpWindow_ == hwnd) pendingSyntheticChildButtonUpWindow_ = 0;
        if (wasVisible) eraseGuestWindowArea(hwnd, it->second);
        it->second.visible = false;
        it->second.destroyed = true;
        it->second.paintBoundsValid = false;
        it->second.backingValid = false;
        it->second.backingPixels.clear();
        ceGwe_.unregisterWindow(hwnd);
        ceMgdi_.destroyWindowBitmap(hwnd);
        publishGuestWindowState(hwnd);
        destroyHostWindow(it->second);
        if (wasVisible && parent) {
            spdlog::info("DestroyWindow invalidating parent=0x{:08x} after child=0x{:08x}", parent, hwnd);
            queueGuestPaint(parent, true);
        }
        if (exposesCoveredWindows) {
            size_t exposed = 0;
            for (const auto& [otherHwnd, window] : ceGwe_.windows()) {
                if (otherHwnd == hwnd || window.destroyed || !window.visible) continue;
                queueGuestPaint(otherHwnd, true);
                ++exposed;
            }
            spdlog::info("DestroyWindow exposed full-screen popup hwnd=0x{:08x}; queued repaint for {} visible windows",
                         hwnd, exposed);
        }
    };
    auto dispatchQueuedPaintForBlockingApi = [&](PendingBlockingApi&, const char*) {
        // CE/MFC Sleep and wait APIs do not pump GWE messages by themselves.
        // Keep queued paint/timer/input on the owner queue until the guest
        // returns to GetMessage/DispatchMessage or enters a real message wait.
        return false;
    };

    if (isCoredll && pc == destroyWindowContinuationStub_) {
        if (ceGwe_.pendingDestroyWindows().empty()) {
            spdlog::warn("DestroyWindow continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(ra, name.c_str()));
            return;
        }
        auto& pending = ceGwe_.pendingDestroyWindows().back();
        if (pending.stage == 0) {
            pending.stage = 1;
            uint32_t wndProc = pending.wndProc;
            auto window = ceGwe_.windows().find(pending.hwnd);
            if (window != ceGwe_.windows().end() && window->second.wndProc) wndProc = window->second.wndProc;
            wndProc = translatedWndProc(wndProc, "DestroyWindow");
            spdlog::info("DestroyWindow synchronous WM_NCDESTROY hwnd=0x{:08x} wndproc=0x{:08x}",
                         pending.hwnd, wndProc);
            setReg(UC_MIPS_REG_A0, pending.hwnd);
            setReg(UC_MIPS_REG_A1, 0x0082); // WM_NCDESTROY
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, 0);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, destroyWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t hwnd = pending.hwnd;
        const uint32_t originalRa = pending.originalRa;
        const uint32_t originalGp = pending.originalGp;
        const uint32_t parent = pending.parent;
        const bool wasVisible = pending.wasVisible;
        ceGwe_.pendingDestroyWindows().pop_back();
        finalizeDestroyedWindow(hwnd, wasVisible, parent);
        lastError_ = 0;
        setReg(UC_MIPS_REG_V0, 1);
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_GP, originalGp);
        setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(originalRa, name.c_str()));
        spdlog::info("DestroyWindow synchronous destroy complete hwnd=0x{:08x} return=0x{:08x}", hwnd, originalRa);
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == createWindowContinuationStub_) {
        if (ceGwe_.pendingCreateWindows().empty()) {
            spdlog::warn("CreateWindowExW continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 0);
            setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(ra, name.c_str()));
            return;
        }
        auto& pending = ceGwe_.pendingCreateWindows().back();
        const uint32_t wndProcResult = reg(UC_MIPS_REG_V0);
        if (pending.stage == 0) {
            if (wndProcResult == 0) {
                const uint32_t originalRa = pending.originalRa;
                const uint32_t originalGp = pending.originalGp;
                const uint32_t hwnd = pending.hwnd;
                ceGwe_.pendingCreateWindows().pop_back();
                finalizeDestroyedWindow(hwnd);
                lastError_ = 0;
                setReg(UC_MIPS_REG_V0, 0);
                setReg(UC_MIPS_REG_RA, originalRa);
                setReg(UC_MIPS_REG_GP, originalGp);
                setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(originalRa, name.c_str()));
                spdlog::info("CreateWindowExW synchronous WM_NCCREATE rejected hwnd=0x{:08x}", hwnd);
                pumpHostMessages();
                return;
            }
            pending.stage = 1;
            auto window = ceGwe_.windows().find(pending.hwnd);
            uint32_t wndProc = pending.wndProc;
            if (window != ceGwe_.windows().end() && window->second.wndProc) wndProc = window->second.wndProc;
            wndProc = translatedWndProc(wndProc, "CreateWindowExW");
            spdlog::info("CreateWindowExW synchronous WM_CREATE hwnd=0x{:08x} wndproc=0x{:08x}",
                         pending.hwnd, wndProc);
            setReg(UC_MIPS_REG_A0, pending.hwnd);
            setReg(UC_MIPS_REG_A1, 0x0001); // WM_CREATE
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, pending.createStruct);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, createWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t hwnd = pending.hwnd;
        const uint32_t originalRa = pending.originalRa;
        const uint32_t originalGp = pending.originalGp;
        ceGwe_.pendingCreateWindows().pop_back();
        auto completedWindow = ceGwe_.windows().find(hwnd);
        if (wndProcResult == 0xffffffffu ||
            completedWindow == ceGwe_.windows().end() ||
            completedWindow->second.destroyed) {
            finalizeDestroyedWindow(hwnd);
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, 0);
            spdlog::info("CreateWindowExW synchronous WM_CREATE failed/destroyed hwnd=0x{:08x}", hwnd);
        } else {
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, hwnd);
            spdlog::info("CreateWindowExW synchronous create complete hwnd=0x{:08x}", hwnd);
            queueVisibleFullScreenPopupPaint(hwnd);
            queueVisiblePopupPaint(hwnd);
        }
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_GP, originalGp);
        setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(originalRa, name.c_str()));
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == blockingApiContinuationStub_) {
        if (pendingBlockingApis_.empty()) {
            spdlog::warn("blocking API continuation reached with no pending call");
            setReg(UC_MIPS_REG_V0, 0);
            setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(ra, name.c_str()));
            return;
        }
        auto& pending = pendingBlockingApis_.back();
        if (pending.releaseHostPresentAfterPaintHwnd) {
            const uint32_t paintedHwnd = pending.releaseHostPresentAfterPaintHwnd;
            pending.releaseHostPresentAfterPaintHwnd = 0;
            releaseHostErasePresentDeferral(paintedHwnd);
            presentHostWindows(true);
        }
        PendingBlockingApi pendingCall = pendingBlockingApis_.back();
        uint32_t ret = 0;
        bool waitStillBlocked = false;
        if (pendingCall.ordinal == 0x01F0) {
            pendingBlockingApis_.pop_back();
            handleSleep(SyntheticExportCode::CoreDllSleep, pendingCall.args, ret);
        } else if (pendingCall.ordinal == 0x01F1) {
            constexpr uint32_t kWaitTimeout = CeKernel::kWaitTimeout;
            constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
#if defined(_WIN32)
            auto hostWaitProbe = [](const GuestHandle& handle) {
                const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
                if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
                if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
                return CeKernel::HostWaitResult{false, true, GetLastError()};
            };
#else
            CeKernel::HostWaitProbe hostWaitProbe;
#endif
            refreshCompletedHostWaveBuffers();
	            const CeKernel::WaitQueryResult wait =
	                ceKernel_.queryWaitObject(pendingCall.args.a0, hostWaitProbe, true);
	            ret = wait.result;
	            lastError_ = wait.error;
	            if (ret == CeKernel::kWaitObject0) {
	                ceKernel_.consumeAutoResetEvent(pendingCall.args.a0);
	            }
            const bool finiteExpired =
                pendingCall.deadlineMs && hostTickMilliseconds() >= pendingCall.deadlineMs;
            waitStillBlocked = ret == kWaitTimeout &&
                               pendingCall.args.a1 != 0 &&
                               pendingCall.args.a1 != kInfiniteTimeout &&
                               !finiteExpired;
            if (ret == kWaitTimeout &&
                pendingCall.args.a1 == kInfiniteTimeout) {
                waitStillBlocked = true;
            }
            if (waitStillBlocked) {
                setReg(UC_MIPS_REG_PC, blockingApiContinuationStub_);
                ceKernel_.mainThreadContext() = captureGuestCpuContext();
                updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
                spdlog::debug("{} remains parked wait=0x{:08x} timeout=0x{:08x} queued={} activeThread=0x{:08x}",
                              pendingCall.name,
                              pendingCall.args.a0,
                              pendingCall.args.a1,
                              ceGwe_.messageCount(),
                              ceKernel_.activeGuestThread());
                pumpHostMessages();
                if (!switchToRunnableGuestThread(pendingCall.name.c_str())) {
                    uc_emu_stop(uc_);
                }
                return;
            }
            pendingBlockingApis_.pop_back();
        } else if (!dispatchHostWin32(pendingCall.ordinal, pendingCall.args, ret)) {
            pendingBlockingApis_.pop_back();
            spdlog::warn("blocking API continuation could not resume {}", pendingCall.name);
            lastError_ = 120;
            ret = 0;
        } else {
            pendingBlockingApis_.pop_back();
        }
        setReg(UC_MIPS_REG_V0, ret);
        setReg(UC_MIPS_REG_RA, pendingCall.args.ra);
        setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(pendingCall.args.ra));
        setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(pendingCall.args.ra, name.c_str()));
        spdlog::info("{} resumed after blocking wait handle=0x{:08x} timeout=0x{:08x} return=0x{:08x} ra=0x{:08x} queued={} activeThread=0x{:08x}",
                     pendingCall.name, pendingCall.args.a0, pendingCall.args.a1, ret,
                     pendingCall.args.ra, ceGwe_.messageCount(), ceKernel_.activeGuestThread());
        pumpHostMessages();
        cooperateGuestThreadsAfterCall(pendingCall.name);
        return;
    }

    if (isCoredll && pc == updateWindowContinuationStub_) {
        if (ceGwe_.pendingUpdateWindows().empty()) {
            spdlog::warn("UpdateWindow continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(ra, name.c_str()));
            return;
        }
        auto& pending = ceGwe_.pendingUpdateWindows().back();
        auto window = ceGwe_.windows().find(pending.hwnd);
        const std::string sourceName = pending.sourceName.empty() ? "UpdateWindow" : pending.sourceName;
        uint32_t wndProc = pending.wndProc;
        if (window == ceGwe_.windows().end() || window->second.destroyed || !window->second.visible) {
            if (pending.deferredHostPresent) {
                releaseHostErasePresentDeferral(pending.hwnd);
                pending.deferredHostPresent = false;
            }
            const uint32_t originalRa = pending.originalRa;
            const uint32_t originalGp = pending.originalGp;
            const uint32_t hwnd = pending.hwnd;
            ceGwe_.pendingUpdateWindows().pop_back();
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_RA, originalRa);
            setReg(UC_MIPS_REG_GP, originalGp);
            setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(originalRa, name.c_str()));
            spdlog::info("{} skipped invisible/destroyed hwnd=0x{:08x}", sourceName, hwnd);
            pumpHostMessages();
            return;
        }
        if (window->second.wndProc) wndProc = window->second.wndProc;
        wndProc = translatedWndProc(wndProc, sourceName.c_str());
        if (pending.stage == 0) {
            pending.stage = 1;
            spdlog::info("{} synchronous WM_PAINT hwnd=0x{:08x} wndproc=0x{:08x}",
                         sourceName, pending.hwnd, wndProc);
            setReg(UC_MIPS_REG_A0, pending.hwnd);
            setReg(UC_MIPS_REG_A1, 0x000f); // WM_PAINT
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, 0);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, updateWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t originalRa = pending.originalRa;
        const uint32_t originalGp = pending.originalGp;
        const uint32_t hwnd = pending.hwnd;
        if (pending.deferredHostPresent) {
            releaseHostErasePresentDeferral(hwnd);
            pending.deferredHostPresent = false;
        }
        ceGwe_.pendingUpdateWindows().pop_back();
        lastError_ = 0;
        setReg(UC_MIPS_REG_V0, 1);
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_GP, originalGp);
        setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(originalRa, name.c_str()));
        spdlog::info("{} synchronous paint complete hwnd=0x{:08x} return=0x{:08x}",
                     sourceName, hwnd, originalRa);
        queueVisiblePopupPaintsAbove(hwnd);
        invalidateHostWindows();
        presentHostWindows(false);
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == messageTransferContinuationStub_) {
        if (ceGwe_.pendingMessageTransfers().empty()) {
            spdlog::error("message transfer continuation reached with no pending message pc=0x{:08x} ra=0x{:08x} "
                          "v0=0x{:08x} queued={} activeThread=0x{:08x}",
                          pc, ra, reg(UC_MIPS_REG_V0), ceGwe_.messageCount(), ceKernel_.activeGuestThread());
            const uint32_t normalizedRa = normalizeGuestCodeAddress(ra, name.c_str());
            if (normalizedRa && normalizedRa != messageTransferContinuationStub_ &&
                isGuestRangeReadable(normalizedRa, 4)) {
                setReg(UC_MIPS_REG_PC, normalizedRa);
            } else if (restoreMainThreadContextIfRunnable("empty-message-transfer-continuation")) {
                return;
            } else if (switchToRunnableGuestThread("empty-message-transfer-continuation")) {
                return;
            } else {
                setReg(UC_MIPS_REG_PC, 0);
                uc_emu_stop(uc_);
            }
            return;
        }
        PendingMessageTransfer pending = ceGwe_.pendingMessageTransfers().back();
        ceGwe_.pendingMessageTransfers().pop_back();
        const uint32_t wndProcResult = reg(UC_MIPS_REG_V0);
        const uint64_t completedMs = hostTickMilliseconds();
        const uint64_t transferElapsedMs =
            pending.startedMs && completedMs >= pending.startedMs ? completedMs - pending.startedMs : 0;
        if (pending.releaseHostPresentAfterPaint) {
            releaseHostErasePresentDeferral(pending.hwnd);
        }
        if (pending.synchronousSender) {
            auto sender = ceKernel_.threads().find(pending.synchronousSender);
            if (sender != ceKernel_.threads().end() &&
                sender->second.state == GuestThreadRunState::WaitingForSendMessage) {
                const uint32_t senderPc = sender->second.context.registers.count(UC_MIPS_REG_PC)
                    ? sender->second.context.registers[UC_MIPS_REG_PC]
                    : 0;
                const uint32_t senderRa = sender->second.context.registers.count(UC_MIPS_REG_RA)
                    ? sender->second.context.registers[UC_MIPS_REG_RA]
                    : 0;
                const uint32_t senderSp = sender->second.context.registers.count(UC_MIPS_REG_SP)
                    ? sender->second.context.registers[UC_MIPS_REG_SP]
                    : 0;
                sender->second.context.registers[UC_MIPS_REG_V0] = wndProcResult;
                sender->second.state = GuestThreadRunState::Runnable;
                if (traceGuestWindowMessage(pending.message)) {
                    spdlog::info("{} completed synchronous sender=0x{:08x} hwnd=0x{:08x} msg=0x{:08x} result=0x{:08x} senderPc=0x{:08x} senderRa=0x{:08x} senderSp=0x{:08x} return=0x{:08x}",
                                 pending.sourceName,
                                 pending.synchronousSender,
                                 pending.hwnd,
                                 pending.message,
                                 wndProcResult,
                                 senderPc,
                                 senderRa,
                                 senderSp,
                                 pending.originalRa);
                }
            } else {
                spdlog::warn("{} synchronous sender missing/not waiting sender=0x{:08x} hwnd=0x{:08x} msg=0x{:08x}",
                             pending.sourceName,
                             pending.synchronousSender,
                             pending.hwnd,
                             pending.message);
            }
        }
        if (!pending.synchronousSender && traceGuestWindowMessage(pending.message)) {
            spdlog::info("{} message transfer complete hwnd=0x{:08x} msg=0x{:08x} result=0x{:08x} return=0x{:08x} elapsedMs={} owner=0x{:08x}",
                         pending.sourceName,
                         pending.hwnd,
                         pending.message,
                         wndProcResult,
                         pending.originalRa,
                         transferElapsedMs,
                         pending.ownerThread);
        } else if (transferElapsedMs >= 250) {
            spdlog::info("{} long message transfer complete hwnd=0x{:08x} msg=0x{:08x} elapsedMs={} owner=0x{:08x} startPc=0x{:08x} queuedStart={} queuedNow={}",
                         pending.sourceName,
                         pending.hwnd,
                         pending.message,
                         transferElapsedMs,
                         pending.ownerThread,
                         pending.startPc,
                         pending.queuedAtStart,
                         ceGwe_.messageCount());
        }
        if (pending.releaseHostPresentAfterPaint) {
            invalidateHostWindows();
            presentHostWindows(false);
        }
        auto readableReturn = [&](uint32_t address) {
            const uint32_t normalized = normalizeGuestCodeAddress(address, pending.sourceName.c_str());
            return normalized && normalized != messageTransferContinuationStub_ &&
                   isGuestRangeReadable(normalized, 4);
        };
        uint32_t returnRa = normalizeGuestCodeAddress(pending.originalRa, pending.sourceName.c_str());
        if (returnRa == messageTransferContinuationStub_ && ceGwe_.pendingMessageTransfers().empty()) {
            if (readableReturn(pending.outerReturnRa)) {
                spdlog::warn("{} collapsed orphaned message-transfer continuation hwnd=0x{:08x} msg=0x{:08x} "
                             "result=0x{:08x} return=0x{:08x}",
                             pending.sourceName, pending.hwnd, pending.message, wndProcResult, pending.outerReturnRa);
                returnRa = normalizeGuestCodeAddress(pending.outerReturnRa, pending.sourceName.c_str());
            } else {
                spdlog::error("{} cannot resolve orphaned message-transfer continuation hwnd=0x{:08x} msg=0x{:08x} "
                              "result=0x{:08x} queued={} activeThread=0x{:08x}",
                              pending.sourceName, pending.hwnd, pending.message, wndProcResult,
                              ceGwe_.messageCount(), ceKernel_.activeGuestThread());
                setReg(UC_MIPS_REG_PC, 0);
                uc_emu_stop(uc_);
                return;
            }
        }
        if (returnRa != messageTransferContinuationStub_ && !readableReturn(returnRa)) {
            if (readableReturn(pending.outerReturnRa)) {
                spdlog::warn("{} replaced unreadable message-transfer return hwnd=0x{:08x} msg=0x{:08x} "
                             "result=0x{:08x} return=0x{:08x} outer=0x{:08x}",
                             pending.sourceName, pending.hwnd, pending.message, wndProcResult,
                             returnRa, pending.outerReturnRa);
                returnRa = normalizeGuestCodeAddress(pending.outerReturnRa, pending.sourceName.c_str());
            } else if (restoreMainThreadContextIfRunnable("invalid-message-transfer-return")) {
                spdlog::warn("{} restored parked main context after unreadable message-transfer return hwnd=0x{:08x} "
                             "msg=0x{:08x} result=0x{:08x} return=0x{:08x} outer=0x{:08x}",
                             pending.sourceName, pending.hwnd, pending.message, wndProcResult,
                             returnRa, pending.outerReturnRa);
                return;
            } else if (switchToRunnableGuestThread("invalid-message-transfer-return")) {
                spdlog::warn("{} switched to runnable thread after unreadable message-transfer return hwnd=0x{:08x} "
                             "msg=0x{:08x} result=0x{:08x} return=0x{:08x} outer=0x{:08x}",
                             pending.sourceName, pending.hwnd, pending.message, wndProcResult,
                             returnRa, pending.outerReturnRa);
                return;
            } else {
                spdlog::error("{} cannot resolve unreadable message-transfer return hwnd=0x{:08x} msg=0x{:08x} "
                              "result=0x{:08x} return=0x{:08x} outer=0x{:08x} queued={} activeThread=0x{:08x}",
                              pending.sourceName, pending.hwnd, pending.message, wndProcResult,
                              returnRa, pending.outerReturnRa, ceGwe_.messageCount(), ceKernel_.activeGuestThread());
                setReg(UC_MIPS_REG_PC, 0);
                uc_emu_stop(uc_);
                return;
            }
        }
        setReg(UC_MIPS_REG_RA, returnRa);
        setReg(UC_MIPS_REG_GP, pending.originalGp);
        setReg(UC_MIPS_REG_PC, normalizeGuestCodeAddress(returnRa, pending.sourceName.c_str()));
        return;
    }

    if (isCoredll && pc == threadExitStub_) {
        if (!finishActiveGuestThread(reg(UC_MIPS_REG_V0))) {
            spdlog::info("main guest context returned through thread-exit stub exitCode=0x{:08x}", reg(UC_MIPS_REG_V0));
            quitPosted_ = true;
            uc_emu_stop(uc_);
        }
        pumpHostMessages();
        return;
    }

    uint32_t ret = 1;
    if (isCoredll) {
        switch (ordinal) {
        case 0x0006: {
            if (!finishActiveGuestThread(a0)) {
                spdlog::warn("ExitThread called without active guest thread exitCode=0x{:08x}", a0);
                setReg(UC_MIPS_REG_V0, 0);
                setReg(UC_MIPS_REG_PC, ra);
            }
            pumpHostMessages();
            return;

        }
        case 0x00F6: {
            uint32_t ret = 0;
            if (!dispatchHostWin32(ordinal, args, ret)) {
                lastError_ = 120;
                finishImmediateReturn(0);
                return;
            }
            auto window = ceGwe_.windows().find(ret);
            if (!ret || window == ceGwe_.windows().end() || !window->second.wndProc ||
                !window->second.createStruct || !createWindowContinuationStub_) {
                finishImmediateReturn(ret);
                return;
            }
            const uint32_t wndProc = translatedWndProc(window->second.wndProc, "CreateWindowExW");
            ceGwe_.pendingCreateWindows().push_back(PendingCreateWindow{
                ret, wndProc, ra, reg(UC_MIPS_REG_GP), window->second.createStruct, 0,
            });
            spdlog::info("CreateWindowExW synchronous WM_NCCREATE hwnd=0x{:08x} wndproc=0x{:08x} return=0x{:08x}",
                         ret, wndProc, ra);
            setReg(UC_MIPS_REG_A0, ret);
            setReg(UC_MIPS_REG_A1, 0x0081); // WM_NCCREATE
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, window->second.createStruct);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, createWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;

        }
        case 0x010B: {
            auto it = ceGwe_.windows().find(a0);
            if (it == ceGwe_.windows().end() || it->second.destroyed) {
                lastError_ = 1400;
                finishImmediateReturn(0);
                return;
            }
            if (!it->second.visible) {
                lastError_ = 0;
                finishImmediateReturn(1);
                spdlog::info("UpdateWindow ignored invisible hwnd=0x{:08x}", a0);
                return;
            }
            uint32_t wndProc = translatedWndProc(it->second.wndProc, "UpdateWindow");
            if (!wndProc || !updateWindowContinuationStub_) {
                lastError_ = 0;
                finishImmediateReturn(1);
                return;
            }
            ensureHostWindow(a0, it->second);
            const uint32_t eraseDc = makeGuestDc(a0);
            applyPaintUpdateClip(a0, eraseDc);
            const bool deferredHostPresent = beginHostErasePresentDeferral(a0);
            ceGwe_.pendingUpdateWindows().push_back(PendingUpdateWindow{
                a0, wndProc, ra, reg(UC_MIPS_REG_GP), eraseDc, 0, deferredHostPresent, "UpdateWindow"});
            spdlog::info("UpdateWindow synchronous WM_ERASEBKGND hwnd=0x{:08x} wndproc=0x{:08x}",
                         a0, wndProc);
            setReg(UC_MIPS_REG_A0, a0);
            setReg(UC_MIPS_REG_A1, 0x0014); // WM_ERASEBKGND
            setReg(UC_MIPS_REG_A2, eraseDc);
            setReg(UC_MIPS_REG_A3, 0);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, updateWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;

        }
        case 0x0109: {
            auto it = ceGwe_.windows().find(a0);
            if (it == ceGwe_.windows().end() || it->second.destroyed) {
                lastError_ = 1400;
                finishImmediateReturn(0);
                return;
            }
            const size_t discardedMessages =
                ceGwe_.eraseIf([&](const GuestMessage& message) { return message.hwnd == a0; });
            if (discardedMessages) {
                spdlog::info("DestroyWindow discarded {} pending posted messages for hwnd=0x{:08x}",
                             discardedMessages, a0);
            }
            const bool wasVisible = it->second.visible;
            const uint32_t parent = it->second.parent;
            it->second.visible = false;
            const uint32_t wndProc = translatedWndProc(it->second.wndProc, "DestroyWindow");
            if (!wndProc || !destroyWindowContinuationStub_) {
                finalizeDestroyedWindow(a0, wasVisible, parent);
                lastError_ = 0;
                finishImmediateReturn(1);
                return;
            }
            ceGwe_.pendingDestroyWindows().push_back(PendingDestroyWindow{
                a0, wndProc, ra, reg(UC_MIPS_REG_GP), 0, parent, wasVisible});
            spdlog::info("DestroyWindow synchronous WM_DESTROY hwnd=0x{:08x} wndproc=0x{:08x} return=0x{:08x}",
                         a0, wndProc, ra);
            setReg(UC_MIPS_REG_A0, a0);
            setReg(UC_MIPS_REG_A1, 0x0002); // WM_DESTROY
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, 0);
            setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
            setReg(UC_MIPS_REG_T9, wndProc);
            setReg(UC_MIPS_REG_RA, destroyWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;

        }
        case 0x01F0: {
            if (ceKernel_.activeGuestThread()) {
                auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
                if (active != ceKernel_.threads().end()) {
                    active->second.context = captureGuestCpuContext();
                    active->second.context.registers[UC_MIPS_REG_PC] = ra;
                    active->second.context.registers[UC_MIPS_REG_GP] = guestGpForCodeAddress(ra);
                    active->second.context.registers[UC_MIPS_REG_V0] = 0;
                    active->second.waitTimeoutResult = 0;
                    if (a0 == 0) {
                        active->second.state = GuestThreadRunState::Runnable;
                        active->second.sleepUntilMs = 0;
                    } else {
                        active->second.state = GuestThreadRunState::Waiting;
                        active->second.waitHandle = 0;
                        active->second.waitHandles.clear();
                        active->second.waitAll = false;
                        active->second.waitForMessages = false;
                        active->second.waitWakeMask = 0;
                        active->second.sleepUntilMs = hostTickMilliseconds() + uint64_t(a0);
                    }
                    const uint32_t savedRa = active->second.context.registers.count(UC_MIPS_REG_RA)
                        ? active->second.context.registers[UC_MIPS_REG_RA]
                        : 0;
                    const uint32_t savedSp = active->second.context.registers.count(UC_MIPS_REG_SP)
                        ? active->second.context.registers[UC_MIPS_REG_SP]
                        : 0;
                    spdlog::debug("guest thread sleep handle=0x{:08x} timeout={} return=0x{:08x} savedRa=0x{:08x} savedSp=0x{:08x}",
                                  ceKernel_.activeGuestThread(), a0, ra, savedRa, savedSp);
	                }
	                ceKernel_.activeGuestThread() = 0;
	                if (!pendingBlockingApis_.empty() ||
	                    !restoreMainThreadContextIfRunnable(name.c_str())) {
	                    switchToRunnableGuestThread(name.c_str());
	                }
	                pumpHostMessages();
	                return;

            }
            if (blockingApiContinuationStub_) {
                constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
                PendingBlockingApi pending{name, ordinal, args};
                if (a0 != kInfiniteTimeout) {
                    pending.deadlineMs = hostTickMilliseconds() + uint64_t(a0);
                }
                pendingBlockingApis_.push_back(std::move(pending));
                if (dispatchQueuedPaintForBlockingApi(pendingBlockingApis_.back(), "before block")) {
                    return;
                }
                pendingBlockingApis_.pop_back();
            }

            break;
        }
        case 0x01F2: {
            constexpr uint32_t kWaitTimeout = 0x00000102u;
            constexpr uint32_t kWaitFailed = 0xffffffffu;
            constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
            if (ceKernel_.activeGuestThread()) {
                uint32_t ret = waitForMultipleGuestObjects(a0, a1, a2 != 0);
                const bool wouldBlock = ret == kWaitTimeout && a3 != 0;
                if (wouldBlock) {
                    std::vector<uint32_t> handles;
                    if (!readGuestWaitHandles(a0, a1, handles)) {
                        setReg(UC_MIPS_REG_V0, kWaitFailed);
                        setReg(UC_MIPS_REG_PC, ra);
                        pumpHostMessages();
                        return;
                    }
                    auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
                    if (active != ceKernel_.threads().end()) {
                        active->second.context = captureGuestCpuContext();
                        active->second.context.registers[UC_MIPS_REG_PC] = ra;
                        active->second.context.registers[UC_MIPS_REG_GP] = guestGpForCodeAddress(ra);
                        active->second.context.registers[UC_MIPS_REG_V0] = 0;
                        active->second.state = GuestThreadRunState::Waiting;
                        active->second.waitHandle = handles.size() == 1 ? handles.front() : 0;
                        active->second.waitHandles = std::move(handles);
                        active->second.waitAll = a2 != 0;
                        active->second.waitForMessages = false;
                        active->second.waitWakeMask = 0;
                        active->second.waitTimeoutResult = kWaitTimeout;
                        active->second.sleepUntilMs = a3 == kInfiniteTimeout
                            ? 0
                            : hostTickMilliseconds() + uint64_t(a3);
                        spdlog::info("guest thread wait-multiple handle=0x{:08x} count={} waitAll={} timeout=0x{:08x} return=0x{:08x}",
                                     ceKernel_.activeGuestThread(), a0, a2 != 0, a3, ra);
	                    }
	                    ceKernel_.activeGuestThread() = 0;
	                    if (!pendingBlockingApis_.empty() ||
	                        !restoreMainThreadContextIfRunnable(name.c_str())) {
	                        switchToRunnableGuestThread(name.c_str());
	                    }
	                    pumpHostMessages();
	                    return;
                }
                setReg(UC_MIPS_REG_V0, ret);
                setReg(UC_MIPS_REG_PC, ra);
                pumpHostMessages();
                return;
            }
            if (!ceGwe_.hasMessages() && hasRunnableGuestThread()) {
                uint32_t preferredThread = 0;
                std::vector<uint32_t> handles;
                if (readGuestWaitHandles(a0, a1, handles)) {
                    for (uint32_t handleValue : handles) {
                        auto* handle = lookupGuestHandle(handleValue);
                        if (handle && handle->kind == GuestHandle::Kind::GuestThread) {
                            preferredThread = handleValue;
                            break;
                        }
                    }
                }
                const uint32_t immediate = waitForMultipleGuestObjects(a0, a1, a2 != 0);
                if (immediate != kWaitTimeout || a3 == 0) {
                    setReg(UC_MIPS_REG_V0, immediate);
                    setReg(UC_MIPS_REG_PC, ra);
                    pumpHostMessages();
                    return;
                }
                spdlog::info("WaitForMultipleObjects cooperative guest-thread slice count={} handles=0x{:08x} waitAll={} timeout=0x{:08x} retry=1",
                             a0, a1, a2 != 0, a3);
                switchToRunnableGuestThread(name.c_str(), 0, preferredThread);
                pumpHostMessages();
                return;
            }

            break;
        }
        case 0x01F1: {
            constexpr uint32_t kWaitTimeout = CeKernel::kWaitTimeout;
            constexpr uint32_t kWaitFailed = CeKernel::kWaitFailed;
            constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
            auto pollSingleWait = [&](uint32_t& waitResult, uint32_t& preferredThread) {
#if defined(_WIN32)
                auto hostWaitProbe = [](const GuestHandle& handle) {
                    const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
                    if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
                    if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
                    return CeKernel::HostWaitResult{false, true, GetLastError()};
                };
#else
                CeKernel::HostWaitProbe hostWaitProbe;
#endif
                const CeKernel::WaitQueryResult wait =
                    ceKernel_.queryWaitObject(a0, hostWaitProbe, true);
                lastError_ = wait.error;
                waitResult = wait.result;
                preferredThread = wait.preferredThread;
            };

            if (ceKernel_.activeGuestThread()) {
                uint32_t ret = kWaitFailed;
                uint32_t ignoredPreferredThread = 0;
	                pollSingleWait(ret, ignoredPreferredThread);
	                if (ret == CeKernel::kWaitObject0) {
	                    ceKernel_.consumeAutoResetEvent(a0);
	                }
	                const bool wouldBlock = ret == kWaitTimeout && a1 != 0;
                if (wouldBlock) {
                    auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
                    if (active != ceKernel_.threads().end()) {
                        active->second.context = captureGuestCpuContext();
                        active->second.context.registers[UC_MIPS_REG_PC] = ra;
                        active->second.context.registers[UC_MIPS_REG_GP] = guestGpForCodeAddress(ra);
                        active->second.context.registers[UC_MIPS_REG_V0] = 0; // completed wait result after wake
                        active->second.state = GuestThreadRunState::Waiting;
                        active->second.waitHandle = a0;
                        active->second.waitHandles.clear();
                        active->second.waitAll = false;
                        active->second.waitForMessages = false;
                        active->second.waitWakeMask = 0;
                        active->second.waitTimeoutResult = kWaitTimeout;
                        active->second.sleepUntilMs = a1 == kInfiniteTimeout
                            ? 0
                            : hostTickMilliseconds() + uint64_t(a1);
                        spdlog::info("guest thread wait handle=0x{:08x} wait=0x{:08x} return=0x{:08x}",
                                     ceKernel_.activeGuestThread(), a0, ra);
	                    }
	                    ceKernel_.activeGuestThread() = 0;
	                    if (!pendingBlockingApis_.empty() ||
	                        !restoreMainThreadContextIfRunnable(name.c_str())) {
	                        switchToRunnableGuestThread(name.c_str());
	                    }
	                    pumpHostMessages();
	                    return;
                }
                setReg(UC_MIPS_REG_V0, ret);
                setReg(UC_MIPS_REG_PC, ra);
                pumpHostMessages();
                return;
            }
            uint32_t immediate = kWaitFailed;
            uint32_t preferredThread = 0;
            auto parkMainWaitAndRunWorker = [&](const char* reason) {
                if (!blockingApiContinuationStub_) return false;
                PendingBlockingApi pending{name, ordinal, args};
                if (a1 != kInfiniteTimeout) {
                    pending.deadlineMs = hostTickMilliseconds() + uint64_t(a1);
                }
                pendingBlockingApis_.push_back(std::move(pending));
                setReg(UC_MIPS_REG_PC, blockingApiContinuationStub_);
                ceKernel_.mainThreadContext() = captureGuestCpuContext();
                updateCurrentThreadKData(ceKernel_.mainThreadPseudoHandle(), ceKernel_.mainThreadTls());
                spdlog::info("WaitForSingleObject parked main wait wait=0x{:08x} timeout=0x{:08x} reason={} queued={} preferred=0x{:08x}",
                             a0,
                             a1,
                             reason ? reason : "worker",
                             ceGwe_.messageCount(),
                             preferredThread);
                if (!switchToRunnableGuestThread(name.c_str(), 0, preferredThread)) {
                    uc_emu_stop(uc_);
                }
                pumpHostMessages();
                return true;
            };
	            pollSingleWait(immediate, preferredThread);
	            if (immediate != kWaitTimeout || a1 == 0) {
	                if (immediate == CeKernel::kWaitObject0) {
	                    ceKernel_.consumeAutoResetEvent(a0);
	                }
	                setReg(UC_MIPS_REG_V0, a1 == 0 && immediate == kWaitTimeout ? kWaitTimeout : immediate);
	                setReg(UC_MIPS_REG_PC, ra);
	                pumpHostMessages();
	                return;
	            }
	            if (blockingApiContinuationStub_) {
	                PendingBlockingApi pending{name, ordinal, args};
	                if (a1 != kInfiniteTimeout) {
	                    pending.deadlineMs = hostTickMilliseconds() + uint64_t(a1);
	                }
                pendingBlockingApis_.push_back(std::move(pending));
                if (dispatchQueuedPaintForBlockingApi(pendingBlockingApis_.back(), "before block")) {
                    return;
                }
                pendingBlockingApis_.pop_back();

	            }

	            if (parkMainWaitAndRunWorker("initial")) return;

	            pollSingleWait(immediate, preferredThread);
	            if (immediate != kWaitTimeout) {
	                if (immediate == CeKernel::kWaitObject0) {
	                    ceKernel_.consumeAutoResetEvent(a0);
	                }
	                setReg(UC_MIPS_REG_V0, immediate);
                setReg(UC_MIPS_REG_PC, ra);
                pumpHostMessages();
                return;
            }
            if (!ceGwe_.hasMessages() && hasRunnableGuestThread()) {
                if (parkMainWaitAndRunWorker("after-paint")) return;
            }
            if (a1 == kInfiniteTimeout || ceGwe_.hasMessages() || !ceGwe_.timers().empty()) {
                spdlog::debug("WaitForSingleObject parking main context wait=0x{:08x} timeout=0x{:08x} pc=0x{:08x} queued={} timers={}",
                              a0, a1, pc, ceGwe_.messageCount(), ceGwe_.timers().size());
                setReg(UC_MIPS_REG_PC, pc);
                pumpHostMessages();
                uc_emu_stop(uc_);
                return;
            }

            break;
        }
        case kCoredllMsgWaitForMultipleObjectsExOrdinal: {
            constexpr uint32_t kWaitObject0 = CeKernel::kWaitObject0;
            constexpr uint32_t kWaitTimeout = CeKernel::kWaitTimeout;
            constexpr uint32_t kWaitFailed = CeKernel::kWaitFailed;
            constexpr uint32_t kInfiniteTimeout = 0xffffffffu;
            constexpr uint32_t kMwmoWaitAll = 0x00000001u;

            const uint32_t waitCount = a0;
            const uint32_t handlesPtr = a1;
            const uint32_t timeoutMs = a2;
            const uint32_t wakeMask = a3;
            const uint32_t flags = stackArg(4);
            const bool waitAll = (flags & kMwmoWaitAll) != 0;

            std::vector<uint32_t> handles;
            if (waitCount) {
                if (!readGuestWaitHandles(waitCount, handlesPtr, handles)) {
                    setReg(UC_MIPS_REG_V0, kWaitFailed);
                    setReg(UC_MIPS_REG_PC, ra);
                    pumpHostMessages();
                    return;
                }
            } else if (!wakeMask) {
                setReg(UC_MIPS_REG_V0, kWaitTimeout);
                setReg(UC_MIPS_REG_PC, ra);
                pumpHostMessages();
                return;
            }

            auto pollHandles = [&]() {
#if defined(_WIN32)
                auto hostWaitProbe = [](const GuestHandle& handle) {
                    const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle.hostValue), 0);
                    if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
                    if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
                    return CeKernel::HostWaitResult{false, true, GetLastError()};
                };
#else
                CeKernel::HostWaitProbe hostWaitProbe;
#endif
                return handles.empty()
                    ? CeKernel::WaitQueryResult{kWaitTimeout, 0, 0}
                    : ceKernel_.queryWaitObjects(handles, waitAll, hostWaitProbe, false);
            };

            const uint32_t queueOwner = ceKernel_.activeGuestThread()
                ? ceKernel_.activeGuestThread()
                : ceKernel_.mainThreadPseudoHandle();
            const bool messagesReady = wakeMask && ceGwe_.hasMessagesForOwner(queueOwner);
            CeKernel::WaitQueryResult wait = pollHandles();
            if (messagesReady && (!waitAll || handles.empty() || wait.result == kWaitObject0)) {
                setReg(UC_MIPS_REG_V0, kWaitObject0 + waitCount);
                setReg(UC_MIPS_REG_PC, ra);
                lastError_ = 0;
                pumpHostMessages();
                return;
            }
            if (wait.result != kWaitTimeout) {
                setReg(UC_MIPS_REG_V0, wait.result);
                setReg(UC_MIPS_REG_PC, ra);
                lastError_ = wait.error;
                pumpHostMessages();
                return;
            }
            if (timeoutMs == 0) {
                setReg(UC_MIPS_REG_V0, kWaitTimeout);
                setReg(UC_MIPS_REG_PC, ra);
                lastError_ = 0;
                pumpHostMessages();
                return;
            }

            if (ceKernel_.activeGuestThread()) {
                auto active = ceKernel_.threads().find(ceKernel_.activeGuestThread());
                if (active != ceKernel_.threads().end()) {
                    active->second.context = captureGuestCpuContext();
                    active->second.context.registers[UC_MIPS_REG_PC] = ra;
                    active->second.context.registers[UC_MIPS_REG_GP] = guestGpForCodeAddress(ra);
                    active->second.context.registers[UC_MIPS_REG_V0] = 0;
                    active->second.state = GuestThreadRunState::Waiting;
                    active->second.waitHandle = handles.size() == 1 ? handles.front() : 0;
                    active->second.waitHandles = std::move(handles);
                    active->second.waitAll = waitAll;
                    active->second.waitForMessages = wakeMask != 0;
                    active->second.waitWakeMask = wakeMask;
                    active->second.waitTimeoutResult = kWaitTimeout;
                    active->second.sleepUntilMs = timeoutMs == kInfiniteTimeout
                        ? 0
                        : hostTickMilliseconds() + uint64_t(timeoutMs);
                    spdlog::info("guest thread msg-wait handle=0x{:08x} count={} timeout=0x{:08x} wakeMask=0x{:08x} flags=0x{:08x} return=0x{:08x}",
                                 ceKernel_.activeGuestThread(), waitCount, timeoutMs, wakeMask, flags, ra);
                }
                ceKernel_.activeGuestThread() = 0;
                if (!restoreMainThreadContextIfRunnable(name.c_str())) {
                    switchToRunnableGuestThread(name.c_str());
                }
                pumpHostMessages();
                return;
            }

            if (!ceGwe_.hasMessagesForOwner(ceKernel_.mainThreadPseudoHandle()) && hasRunnableGuestThread()) {
                switchToRunnableGuestThread(name.c_str(), 0, wait.preferredThread);
                pumpHostMessages();
                return;
            }
            if (timeoutMs == kInfiniteTimeout || wakeMask || !handles.empty()) {
                spdlog::debug("MsgWaitForMultipleObjectsEx parking main context count={} timeout=0x{:08x} wakeMask=0x{:08x} flags=0x{:08x} queued={}",
                              waitCount, timeoutMs, wakeMask, flags, ceGwe_.messageCount());
                setReg(UC_MIPS_REG_PC, pc);
                pumpHostMessages();
                uc_emu_stop(uc_);
                return;
            }
            break;
        }
        case 0x039D:
        case 0x03AF:
        case 0x03CB: {
            uint32_t ret = 0;
            switch (ordinal) {
            case 0x039D:
                ret = createPatternBrushFromBitmap(a0);
                lastError_ = 0;
                break;
            case 0x03AF: {
                GuestDc* dc = lookupGuestDc(a0);
                if (!dc) {
                    lastError_ = 6;
                    ret = 0;
                } else {
                    if (a3) {
                        writeU32(a3, 0);
                        writeU32(a3 + 4, 0);
                    }
                    lastError_ = 0;
                    ret = 1;
                }
                break;
            }
            case 0x03CB: {
                GuestDc* dc = lookupGuestDc(a0);
                if (!dc || !a1) {
                    lastError_ = dc ? 87 : 6;
                    ret = 0;
                } else {
                    int32_t width = framebufferWidth_;
                    int32_t height = framebufferHeight_;
                    auto bitmap = ceMgdi_.bitmaps().find(dc->selectedBitmap);
                    if (bitmap != ceMgdi_.bitmaps().end()) {
                        width = bitmap->second.width;
                        height = std::abs(bitmap->second.heightRaw);
                    }
                    writeGuestRect(a1, 0, 0, width, height);
                    lastError_ = 0;
                    ret = 2; // SIMPLEREGION
                }
                break;
            }
            default:
                break;
            }
            finishImmediateReturn(ret, true);
            return;
        }
        case 0x00AA:
            if (tryParkGuestSerialRead(args, pc, ra)) {
                return;
            }
            if (handleReadFile(SyntheticExportCode::CoreDllReadFile, args, ret)) {
                finishImmediateReturn(ret, true);
                return;
            }
            break;
        case 0x035D: {
            const uint32_t activeThread = ceKernel_.activeGuestThread();
            if (activeThread && !ceGwe_.hasMessagesForOwner(activeThread) && !quitPosted_) {
                auto active = ceKernel_.threads().find(activeThread);
                if (active != ceKernel_.threads().end()) {
                    active->second.context = captureGuestCpuContext();
                    active->second.context.registers[UC_MIPS_REG_PC] = pc;
                    active->second.state = GuestThreadRunState::WaitingForMessage;
                    spdlog::info("guest thread message wait handle=0x{:08x} return=0x{:08x} msgPtr=0x{:08x}",
                                 activeThread, ra, a0);
                }
                ceKernel_.activeGuestThread() = 0;
                if (!restoreMainThreadContextIfRunnable(name.c_str())) {
                    switchToRunnableGuestThread(name.c_str());
                }
                pumpHostMessages();
                return;

            }
            break;
        }
        case 0x011D:
        case 0x035B:
        case 0x0364: {
            uint32_t wndProc = a0;
            uint32_t hwnd = a1;
            uint32_t msg = a2;
            uint32_t wParam = a3;
            uint32_t lParam = stackArg(4);
            uint32_t synchronousSender = 0;
            bool isDispatchMessageW = false;
            bool isSendMessageW = false;
            switch (ordinal) {
            case 0x035B:
                isDispatchMessageW = true;
                break;
            case 0x0364:
                isSendMessageW = true;
                break;
            default:
                break;
            }
            if (isDispatchMessageW) {
                if (a0) {
                    uc_mem_read(uc_, a0, &hwnd, sizeof(hwnd));
                    uc_mem_read(uc_, a0 + 4, &msg, sizeof(msg));
                    uc_mem_read(uc_, a0 + 8, &wParam, sizeof(wParam));
                    uc_mem_read(uc_, a0 + 12, &lParam, sizeof(lParam));
                    auto syncSender = ceGwe_.retrievedSyncSendersByMsgPtr().find(a0);
                    if (syncSender != ceGwe_.retrievedSyncSendersByMsgPtr().end()) {
                        synchronousSender = syncSender->second;
                        ceGwe_.retrievedSyncSendersByMsgPtr().erase(syncSender);
                    }
                }
                if (msg == 0x0113 && lParam) {
                    wndProc = lParam;
                    if (a0) uc_mem_read(uc_, a0 + 16, &lParam, sizeof(lParam));
                } else {
                    auto it = ceGwe_.windows().find(hwnd);
                    wndProc = it == ceGwe_.windows().end() ? 0 : it->second.wndProc;
                }
            } else if (isSendMessageW) {
                auto it = ceGwe_.windows().find(a0);
                wndProc = it == ceGwe_.windows().end() ? 0 : it->second.wndProc;
                hwnd = a0;
                msg = a1;
                wParam = a2;
                lParam = a3;
            }
            auto targetWindow = ceGwe_.windows().find(hwnd);
            if (targetWindow != ceGwe_.windows().end() && targetWindow->second.externalProcess) {
                const bool delivered = postCrossProcessGuestMessage(targetWindow->second.externalProcessId,
                                                                    targetWindow->second.externalHwnd,
                                                                    msg,
                                                                    wParam,
                                                                    lParam);
                ret = delivered ? 1 : 0;
                spdlog::info("synthetic coredll.dll!{} delivered external hwnd=0x{:08x} remotePid={} remoteHwnd=0x{:08x} msg=0x{:08x} ok={}",
                             name,
                             hwnd,
                             targetWindow->second.externalProcessId,
                             targetWindow->second.externalHwnd,
                             msg,
                             delivered);
                finishImmediateReturn(ret);
                return;
            }
            if (!wndProc) {
                ret = 0;
                finishImmediateReturn(ret);
                return;
            }
            const uint32_t senderHandle = ceKernel_.activeGuestThread();
            const uint32_t targetOwner = targetWindow == ceGwe_.windows().end()
                ? CeGwe::kNoOwnerThread
                : targetWindow->second.ownerThread;
            if (isSendMessageW && senderHandle &&
                targetOwner != CeGwe::kNoOwnerThread &&
                targetOwner != senderHandle) {
                GuestMessage message{};
                message.hwnd = hwnd;
                message.message = msg;
                message.wParam = wParam;
                message.lParam = lParam;
                message.time = uint32_t(++tick_ * 16);
                message.synchronousSender = senderHandle;
                ceGwe_.postSentBeforeFirstMatch(message, [](const GuestMessage& queued) {
                    return queued.synchronousSender == 0;
                });
                wakeGuestThreadsWaitingForMessage();
                lastError_ = 0;
                auto sender = ceKernel_.threads().find(senderHandle);
                if (sender != ceKernel_.threads().end() &&
                    sender->second.state == GuestThreadRunState::Running) {
                    sender->second.context = captureGuestCpuContext();
                    sender->second.context.registers[UC_MIPS_REG_PC] = ra;
                    sender->second.state = GuestThreadRunState::WaitingForSendMessage;
                    const uint32_t senderRa = sender->second.context.registers.count(UC_MIPS_REG_RA)
                        ? sender->second.context.registers[UC_MIPS_REG_RA]
                        : 0;
                    const uint32_t senderSp = sender->second.context.registers.count(UC_MIPS_REG_SP)
                        ? sender->second.context.registers[UC_MIPS_REG_SP]
                        : 0;
                    spdlog::debug("SendMessageW cross-thread saved sender context sender=0x{:08x} return=0x{:08x} savedRa=0x{:08x} savedSp=0x{:08x}",
                                  senderHandle,
                                  ra,
                                  senderRa,
                                  senderSp);
                }
                const bool traceCrossThread = traceGuestWindowMessage(msg);
                if (traceCrossThread) {
                    spdlog::info("SendMessageW cross-thread queued hwnd=0x{:08x} msg=0x{:08x} sender=0x{:08x} owner=0x{:08x} queued={} waiting={}",
                                 hwnd,
                                 msg,
                                 senderHandle,
                                 targetOwner,
                                 ceGwe_.messageCount(),
                                 sender != ceKernel_.threads().end());
                } else {
                    spdlog::debug("SendMessageW cross-thread queued hwnd=0x{:08x} msg=0x{:08x} sender=0x{:08x} owner=0x{:08x} queued={} waiting={}",
                                  hwnd,
                                  msg,
                                  senderHandle,
                                  targetOwner,
                                  ceGwe_.messageCount(),
                                  sender != ceKernel_.threads().end());
                }
                ceKernel_.activeGuestThread() = 0;
                bool receiverRunnable = false;
                if (targetOwner == ceKernel_.mainThreadPseudoHandle()) {
                    receiverRunnable = restoreMainThreadContextIfRunnable("SendMessageW-cross-thread");
                } else {
                    receiverRunnable = switchToRunnableGuestThread("SendMessageW-cross-thread", 0, targetOwner);
                }
                if (!receiverRunnable && !restoreMainThreadContextIfRunnable("SendMessageW-cross-thread")) {
                    switchToRunnableGuestThread("SendMessageW-cross-thread");
                }
                pumpHostMessages();
                return;
            }
            wndProc = translatedWndProc(wndProc, name.c_str());
            const bool tracePrivatePayload =
                msg == 0x006ee ||
                msg == 0x057c9;
            const bool traceWindowMessage = traceGuestWindowMessage(msg);
            if (mutableEntry.calls <= 128 || traceWindowMessage) {
                spdlog::info("synthetic coredll.dll!{} transfer wndproc=0x{:08x} hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x}",
                             name, wndProc, hwnd, msg, wParam, lParam);
            }
            if (traceWindowMessage && isGuestRangeReadable(wndProc, 12)) {
                const uint32_t first = readU32(wndProc);
                const uint32_t second = readU32(wndProc + 4);
                const uint32_t third = readU32(wndProc + 8);
                if (msg == 0x006ee) {
                    spdlog::info("window proc words msg=0x{:08x} wndproc=0x{:08x} words=0x{:08x},0x{:08x},0x{:08x}",
                                 msg, wndProc, first, second, third);
                }
                if (first == 0x3c080006u && third == 0x01000008u) {
                    const uint32_t slot = 0x00060000u + uint32_t(int16_t(second & 0xffffu));
                    const uint32_t target = isGuestRangeReadable(slot, 4) ? readU32(slot) : 0;
                    spdlog::info("window proc thunk msg=0x{:08x} thunk=0x{:08x} slot=0x{:08x} target=0x{:08x}",
                                 msg, wndProc, slot, target);
                }
            }
            if (tracePrivatePayload) {
                auto describePointer = [&](const char* label, uint32_t ptr) {
                    if (!ptr || !isGuestRangeReadable(ptr, 4)) return;
                    std::array<uint8_t, 64> bytes{};
                    size_t byteCount = 0;
                    if (uc_mem_read(uc_, ptr, bytes.data(), bytes.size()) == UC_ERR_OK) {
                        byteCount = bytes.size();
                    }
                    std::string hex;
                    hex.reserve(byteCount * 3);
                    for (size_t i = 0; i < byteCount; ++i) {
                        char tmp[4]{};
                        std::snprintf(tmp, sizeof(tmp), "%02x", bytes[i]);
                        if (!hex.empty()) hex.push_back(' ');
                        hex.append(tmp);
                    }
                    const std::string ascii = readAscii(ptr, 128);
                    const std::string utf16 = readUtf16(ptr, 128);
                    spdlog::info("private-msg ptr {}=0x{:08x} msg=0x{:08x} ascii=\"{}\" utf16=\"{}\" bytes={}",
                                 label, ptr, msg, ascii, utf16, hex);
                };
                describePointer("wparam", wParam);
                describePointer("lparam", lParam);
            }
            if (isSendMessageW && msg == 0x0201 && wParam == 0 && lParam == 0) {
                auto target = ceGwe_.windows().find(hwnd);
                if (target != ceGwe_.windows().end() && !target->second.destroyed &&
                    (target->second.style & kWindowStyleChild) && target->second.parent) {
                    constexpr uint32_t kWmLButtonUp = 0x0202;
                    pendingSyntheticChildButtonUpWindow_ = hwnd;
                    auto matchingQueuedRelease = [&](const GuestMessage& queued) {
                        return queued.message == kWmLButtonUp &&
                               queued.hwnd &&
                               isWindowOrDescendant(hwnd, queued.hwnd);
                    };
                    const bool alreadyMirroredRelease = ceGwe_.anyMessage([&](const GuestMessage& queued) {
                        return queued.hwnd == hwnd && queued.message == kWmLButtonUp;
                    });
                    if (!alreadyMirroredRelease) {
                        auto parentRelease = ceGwe_.firstMatching(matchingQueuedRelease, false);
                        if (parentRelease) {
                            GuestMessage childUp{};
                            childUp.hwnd = hwnd;
                            childUp.message = kWmLButtonUp;
                            childUp.wParam = 0;
                            childUp.lParam = 0;
                            childUp.time = uint32_t(++tick_ * 16);
                            childUp.x = parentRelease->x;
                            childUp.y = parentRelease->y;
                            childUp.queueKind = CeGwe::MessageQueueKind::Input;
                            ceGwe_.postBeforeFirstMatch(childUp, matchingQueuedRelease);
                            pendingSyntheticChildButtonUpWindow_ = 0;
                            spdlog::info("queued mirrored child button-up hwnd=0x{:08x} before pending parent release hwnd=0x{:08x}",
                                         hwnd, parentRelease->hwnd);
                        }
                    }
                    spdlog::info("remembered synthetic child button-down hwnd=0x{:08x} parent=0x{:08x}",
                                 hwnd, target->second.parent);
                }
            }
            const bool hasQueuedPaintForHwnd =
                msg == 0x0014 &&
                ceGwe_.anyMessage([&](const GuestMessage& queued) {
                    return queued.hwnd == hwnd && queued.message == 0x000f;
                });
            const bool deferredHostPresentForErase =
                msg == 0x0014 && hasQueuedPaintForHwnd && beginHostErasePresentDeferral(hwnd);
            const bool releaseHostPresentAfterPaint = msg == 0x000f && hasHostErasePresentDeferral(hwnd);
            setReg(UC_MIPS_REG_A0, hwnd);
            setReg(UC_MIPS_REG_A1, msg);
            setReg(UC_MIPS_REG_A2, wParam);
            setReg(UC_MIPS_REG_A3, lParam);
            if (messageTransferContinuationStub_) {
                auto readableReturn = [&](uint32_t address) {
                    return address && address != messageTransferContinuationStub_ && isGuestRangeReadable(address, 4);
                };
                const uint32_t parkedMainPc = guestContextReg(ceKernel_.mainThreadContext(), UC_MIPS_REG_PC);
                const uint32_t outerReturnRa =
                    (ra == messageTransferContinuationStub_ && !ceGwe_.pendingMessageTransfers().empty())
                        ? ceGwe_.pendingMessageTransfers().back().outerReturnRa
                        : (readableReturn(ra) ? ra : parkedMainPc);
                if (ra != messageTransferContinuationStub_ && !readableReturn(ra)) {
                    spdlog::warn("{} transfer captured unreadable return hwnd=0x{:08x} msg=0x{:08x} "
                                 "ra=0x{:08x} outer=0x{:08x} parkedMain=0x{:08x} pendingTransfers={}",
                                 name, hwnd, msg, ra, outerReturnRa, parkedMainPc, ceGwe_.pendingMessageTransfers().size());
                }
                ceGwe_.pendingMessageTransfers().push_back(PendingMessageTransfer{
                    hwnd,
                    msg,
                    ra,
                    reg(UC_MIPS_REG_GP),
                    outerReturnRa,
                    synchronousSender,
                    ceGwe_.ownerForWindow(hwnd),
                    wndProc,
                    hostTickMilliseconds(),
                    ceGwe_.messageCount(),
                    releaseHostPresentAfterPaint,
                    name,
                });
                setReg(UC_MIPS_REG_GP, guestGpForCodeAddress(wndProc));
                setReg(UC_MIPS_REG_T9, wndProc);
                setReg(UC_MIPS_REG_RA, messageTransferContinuationStub_);
            } else if (deferredHostPresentForErase) {
                releaseHostErasePresentDeferral(hwnd);
            }
            setReg(UC_MIPS_REG_PC, wndProc);
            return;

        }
        default:
            break;
        }
    }
    if (mutableEntry.ordinalHandler) {
        bool handled = (this->*mutableEntry.ordinalHandler)(mutableEntry.code, args, ret);
        if (handled) {
            finishImmediateReturn(ret, true);
            return;
        }
    }
    if (isCoredll &&
        (dispatchHostWin32(ordinal, args, ret) || dispatchGuestMemoryApi(ordinal, args, ret))) {
        switch (ordinal) {
        case 0x035D:
            if (ret != 0 || quitPosted_) break;
            spdlog::debug("GetMessageW empty queue blocks main context pc=0x{:08x} ra=0x{:08x} queued={}",
                          pc, ra, ceGwe_.messageCount());
            setReg(UC_MIPS_REG_PC, pc);
            pumpHostMessages();
            if (!switchToRunnableGuestThread("GetMessageW-blocking", pc)) {
                uc_emu_stop(uc_);
            }
            return;
        default:
            break;
        }
        finishImmediateReturn(ret, true);
        return;
    }
    if (isCoredll) {
        lastError_ = 120; // ERROR_CALL_NOT_IMPLEMENTED
        ret = 0;
        switch (ordinal) {
        case 1167: {
            const uint32_t sp = reg(UC_MIPS_REG_SP);
            const uint32_t s4 = stackArg(4);
            const uint32_t s5 = stackArg(5);
            const uint32_t s6 = stackArg(6);
            const uint32_t s7 = stackArg(7);
            auto previewPointer = [&](const char* label, uint32_t ptr) {
                if (!ptr || !isGuestRangeReadable(ptr, 2)) return;
                std::string ascii = readAscii(ptr, 96);
                std::string utf16 = readUtf16(ptr, 96);
                if (ascii.empty() && utf16.empty()) return;
                spdlog::warn("synthetic coredll.dll!{} unsupported ptr {}=0x{:08x} ascii=\"{}\" utf16=\"{}\"",
                             name, label, ptr, ascii, utf16);
            };
            spdlog::warn("synthetic coredll.dll!{} unsupported by translate layer -> 0 "
                         "call={} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x} "
                         "a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} "
                         "s4=0x{:08x} s5=0x{:08x} s6=0x{:08x} s7=0x{:08x}",
                         name, mutableEntry.calls, pc, ra, sp, a0, a1, a2, a3, s4, s5, s6, s7);
            previewPointer("a0", a0);
            previewPointer("a1", a1);
            previewPointer("a2", a2);
            previewPointer("a3", a3);
            previewPointer("s4", s4);
            previewPointer("s5", s5);
            break;
        }
        default:
            if (mutableEntry.calls == 1) {
                spdlog::warn("synthetic coredll.dll!{} unsupported by translate layer -> 0", name);
            }
            break;
        }
        finishImmediateReturn(ret);
        return;
    }

    bool handled = false;
    if (mutableEntry.moduleKind == SyntheticModuleKind::Ole32) {
        handled = dispatchOle32(name, args, ret);
    } else if (mutableEntry.moduleKind == SyntheticModuleKind::OleAut32) {
        handled = dispatchOleAut32(name, args, ret);
    }

    if (!handled) {
        lastError_ = 120; // ERROR_CALL_NOT_IMPLEMENTED
        ret = 0;
        if (mutableEntry.calls <= 128) {
            spdlog::warn("synthetic {}!{} unsupported by module translate layer -> 0",
                         mutableEntry.moduleName, name);
        }
    }

    finishImmediateReturn(ret);
}
