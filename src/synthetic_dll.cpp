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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace {
std::string lowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string registryRootName(uint32_t hkey) {
    switch (hkey) {
    case 0x80000000u: return "hkcr";
    case 0x80000001u: return "hkcu";
    case 0x80000002u: return "hklm";
    case 0x80000003u: return "hku";
    default: return {};
    }
}

std::string normalizeRegistrySubKey(std::string key) {
    std::replace(key.begin(), key.end(), '/', '\\');
    while (!key.empty() && key.front() == '\\') key.erase(key.begin());
    while (!key.empty() && key.back() == '\\') key.pop_back();
    return lowerAscii(std::move(key));
}

std::string normalizeRegistryValueName(std::string name) {
    return lowerAscii(std::move(name));
}

std::string registryTypeName(uint32_t type) {
    switch (type) {
    case 0: return "REG_NONE";
    case 1: return "REG_SZ";
    case 2: return "REG_EXPAND_SZ";
    case 3: return "REG_BINARY";
    case 4: return "REG_DWORD";
    case 7: return "REG_MULTI_SZ";
    default: return "REG_" + std::to_string(type);
    }
}

uint32_t registryTypeId(const nlohmann::json& value) {
    if (value.contains("type")) {
        if (value["type"].is_number_unsigned()) return value["type"].get<uint32_t>();
        if (value["type"].is_string()) {
            const std::string type = lowerAscii(value["type"].get<std::string>());
            if (type == "reg_none") return 0;
            if (type == "reg_sz") return 1;
            if (type == "reg_expand_sz") return 2;
            if (type == "reg_binary") return 3;
            if (type == "reg_dword") return 4;
            if (type == "reg_multi_sz") return 7;
            if (type.rfind("reg_", 0) == 0) return uint32_t(std::strtoul(type.c_str() + 4, nullptr, 10));
        }
    }
    return 0;
}

bool sameModule(std::string name, std::string_view wanted) {
    name = lowerAscii(std::move(name));
    return name == wanted;
}

uint16_t readLe16(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 2 > bytes.size()) return 0;
    return uint16_t(bytes[offset] | (bytes[offset + 1] << 8));
}

uint32_t readLe32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset + 4 > bytes.size()) return 0;
    return uint32_t(bytes[offset] | (bytes[offset + 1] << 8) |
                    (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24));
}

std::string utf16FromBytes(const std::vector<uint8_t>& bytes, size_t offset, size_t chars) {
    std::string result;
    for (size_t i = 0; i < chars && offset + i * 2 + 1 < bytes.size(); ++i) {
        uint16_t ch = readLe16(bytes, offset + i * 2);
        if (ch < 0x80) result.push_back(char(ch));
        else result.push_back('?');
    }
    return result;
}

#if defined(_WIN32)
struct WinmmBridge {
    HMODULE module{};
    decltype(&waveInOpen) waveInOpen{};
    decltype(&waveInAddBuffer) waveInAddBuffer{};
    decltype(&waveInUnprepareHeader) waveInUnprepareHeader{};
    decltype(&waveInReset) waveInReset{};
    decltype(&waveInClose) waveInClose{};
    decltype(&waveInMessage) waveInMessage{};
    decltype(&mixerGetControlDetailsW) mixerGetControlDetailsW{};
    bool attempted{};
};

WinmmBridge& winmmBridge() {
    static WinmmBridge bridge;
    if (bridge.attempted) return bridge;
    bridge.attempted = true;
    bridge.module = LoadLibraryW(L"winmm.dll");
    if (!bridge.module) return bridge;
    bridge.waveInOpen = reinterpret_cast<decltype(bridge.waveInOpen)>(
        GetProcAddress(bridge.module, "waveInOpen"));
    bridge.waveInAddBuffer = reinterpret_cast<decltype(bridge.waveInAddBuffer)>(
        GetProcAddress(bridge.module, "waveInAddBuffer"));
    bridge.waveInUnprepareHeader = reinterpret_cast<decltype(bridge.waveInUnprepareHeader)>(
        GetProcAddress(bridge.module, "waveInUnprepareHeader"));
    bridge.waveInReset = reinterpret_cast<decltype(bridge.waveInReset)>(
        GetProcAddress(bridge.module, "waveInReset"));
    bridge.waveInClose = reinterpret_cast<decltype(bridge.waveInClose)>(
        GetProcAddress(bridge.module, "waveInClose"));
    bridge.waveInMessage = reinterpret_cast<decltype(bridge.waveInMessage)>(
        GetProcAddress(bridge.module, "waveInMessage"));
    bridge.mixerGetControlDetailsW = reinterpret_cast<decltype(bridge.mixerGetControlDetailsW)>(
        GetProcAddress(bridge.module, "mixerGetControlDetailsW"));
    return bridge;
}
#endif
}

SyntheticDllRuntime::SyntheticDllRuntime(uc_engine* uc) : uc_(uc) {
    if (!uc_) throw std::runtime_error("SyntheticDllRuntime requires a Unicorn engine");
    uc_mem_map(uc_, heapBase_, heapLimit_ - heapBase_, UC_PROT_ALL);
    uc_mem_map(uc_, 0x00005000, 0x00001000, UC_PROT_ALL);
}

void SyntheticDllRuntime::setMainModulePath(std::string path) {
    if (path.empty()) return;
    mainModulePath_ = path;
    hostBaseDir_ = std::filesystem::path(std::move(path)).parent_path();
    loadMainResources(mainModulePath_);
}

void SyntheticDllRuntime::setFramebuffer(uint32_t* bgra, int width, int height) {
    framebuffer_ = bgra;
    framebufferWidth_ = width;
    framebufferHeight_ = height;
}

void SyntheticDllRuntime::setRegistryPath(const std::filesystem::path& path) {
    registryPath_ = path;
    registry_ = nlohmann::json::object();
    if (!registryPath_.empty()) {
        std::ifstream input(registryPath_);
        if (input) {
            try {
                input >> registry_;
            } catch (const std::exception& e) {
                spdlog::warn("failed to parse registry database {}: {}", registryPath_.string(), e.what());
                registry_ = nlohmann::json::object();
            }
        }
    }
    if (!registry_.is_object()) registry_ = nlohmann::json::object();
    if (!registry_.contains("version")) registry_["version"] = 1;
    if (!registry_.contains("keys") || !registry_["keys"].is_object()) registry_["keys"] = nlohmann::json::object();
    registryEnsureKey("hkcr");
    registryEnsureKey("hkcu");
    registryEnsureKey("hklm");
    registryEnsureKey("hku");
    registryDirty_ = false;
    if (!registryPath_.empty()) spdlog::info("registry database: {}", registryPath_.string());
}

void SyntheticDllRuntime::flushRegistry() {
    if (registryPath_.empty() || !registryDirty_) return;
    if (!registryPath_.parent_path().empty()) std::filesystem::create_directories(registryPath_.parent_path());
    std::ofstream output(registryPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        spdlog::warn("failed to write registry database {}", registryPath_.string());
        return;
    }
    output << registry_.dump(2);
    registryDirty_ = false;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createModule(const std::string& dllName) {
    if (sameModule(dllName, "coredll.dll")) return createCoredll();
    if (sameModule(dllName, "commctrl.dll")) return createGenericOrdinalDll("commctrl.dll", 512);
    if (sameModule(dllName, "winsock.dll")) {
        auto module = createGenericOrdinalDll("WINSOCK.dll", 128);
        if (module) {
            registerExport(*module, 0x0001, "WSACleanup");
            registerExport(*module, 0x0003, "WSAStartup");
            registerExport(*module, 0x0005, "__WSAFDIsSet");
            registerExport(*module, 0x0006, "accept");
            registerExport(*module, 0x0007, "bind");
            registerExport(*module, 0x0008, "closesocket");
            registerExport(*module, 0x0009, "connect");
            registerExport(*module, 0x000B, "gethostbyname");
            registerExport(*module, 0x000C, "gethostname");
            registerExport(*module, 0x0010, "htonl");
            registerExport(*module, 0x0011, "htons");
            registerExport(*module, 0x0012, "inet_addr");
            registerExport(*module, 0x0013, "inet_ntoa");
            registerExport(*module, 0x0014, "ioctlsocket");
            registerExport(*module, 0x0015, "listen");
            registerExport(*module, 0x0016, "ntohl");
            registerExport(*module, 0x0017, "ntohs");
            registerExport(*module, 0x0018, "recv");
            registerExport(*module, 0x001A, "select");
            registerExport(*module, 0x001B, "send");
            registerExport(*module, 0x001E, "setsockopt");
            registerExport(*module, 0x0020, "socket");
        }
        return module;
    }
    if (sameModule(dllName, "ole32.dll")) {
        auto module = createGenericOrdinalDll("ole32.dll", 512);
        if (module) {
            registerExport(*module, 0x0000, "CLSIDFromProgID");
            registerExport(*module, 0x0001, "CLSIDFromString");
            registerExport(*module, 0x0002, "CoCreateInstance");
            registerExport(*module, 0x0006, "CoInitializeEx");
            registerExport(*module, 0x0009, "CoTaskMemAlloc");
            registerExport(*module, 0x000A, "CoTaskMemFree");
            registerExport(*module, 0x000B, "CoTaskMemRealloc");
            registerExport(*module, 0x000D, "CoUninitialize");
            registerExport(*module, 0x000E, "CoTaskMemSize");
            registerExport(*module, 0x0011, "ProgIDFromCLSID");
            registerExport(*module, 0x0013, "StringFromGUID2");
            registerExport(*module, 0x0014, "StringFromIID");
            registerExport(*module, 0x001B, "CoCreateGuid");
            registerExport(*module, 0x001C, "ReadClassStm");
            registerExport(*module, 0x001D, "OleSave");
            registerExport(*module, 0x001E, "OleRun");
            registerExport(*module, 0x001F, "OleIsRunning");
            registerExport(*module, 0x0022, "StringFromCLSID");
            registerExport(*module, 0x0025, "CreateOleAdviseHolder");
            registerExport(*module, 0x0026, "WriteClassStm");
            registerExport(*module, 0x0027, "OleDraw");
            registerExport(*module, 0x0028, "OleSetContainedObject");
        }
        return module;
    }
    if (sameModule(dllName, "oleaut32.dll")) {
        auto module = createGenericOrdinalDll("OLEAUT32.dll", 512);
        if (module) {
            registerExport(*module, 0x0001, "CreateErrorInfo");
            registerExport(*module, 0x000C, "LoadRegTypeLib");
            registerExport(*module, 0x000D, "LoadTypeLib");
            registerExport(*module, 0x000F, "RegisterTypeLib");
            registerExport(*module, 0x0025, "SetErrorInfo");
            registerExport(*module, 0x0026, "SysAllocString");
            registerExport(*module, 0x0027, "SysAllocStringByteLen");
            registerExport(*module, 0x0028, "SysAllocStringLen");
            registerExport(*module, 0x0029, "SysFreeString");
            registerExport(*module, 0x002C, "SysStringByteLen");
            registerExport(*module, 0x002D, "SysStringLen");
            registerExport(*module, 0x00D8, "VarUI4FromStr");
            registerExport(*module, 0x00DC, "VariantChangeType");
            registerExport(*module, 0x00DE, "VariantClear");
        }
        return module;
    }
    return std::nullopt;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createCoredll() {
    SyntheticModule module;
    module.moduleName = "coredll.dll";
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00020000;
    nextModuleBase_ += 0x00020000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic COREDLL.dll");
    }

    for (uint16_t ordinal = 1; ordinal <= 2500; ++ordinal) {
        registerExport(module, ordinal, {});
    }

    // Ordinals and names are from the Windows CE 4.2 Standard SDK MIPSII
    // coredll.lib COFF import-object headers. Keep these SDK ordinals
    // authoritative; runtime surprises belong in TODO.md until verified.
    registerExport(module, 0x0002, "InitializeCriticalSection");
    registerExport(module, 0x0003, "DeleteCriticalSection");
    registerExport(module, 0x0004, "EnterCriticalSection");
    registerExport(module, 0x0005, "LeaveCriticalSection");
    registerExport(module, 0x0006, "ExitThread");
    registerExport(module, 0x0009, "InterlockedTestExchange");
    registerExport(module, 0x000A, "InterlockedIncrement");
    registerExport(module, 0x000B, "InterlockedDecrement");
    registerExport(module, 0x000C, "InterlockedExchange");
    registerExport(module, 0x000F, "TlsGetValue");
    registerExport(module, 0x0010, "TlsSetValue");
    registerExport(module, 0x0013, "SystemTimeToFileTime");
    registerExport(module, 0x0017, "GetLocalTime");
    registerExport(module, 0x0019, "GetSystemTime");
    registerExport(module, 0x001B, "GetTimeZoneInformation");
    registerExport(module, 0x0020, "GetAPIAddress");
    registerExport(module, 0x0021, "LocalAlloc");
    registerExport(module, 0x0022, "LocalReAlloc");
    registerExport(module, 0x0023, "LocalSize");
    registerExport(module, 0x0024, "LocalFree");
    registerExport(module, 0x0026, "RemoteLocalReAlloc");
    registerExport(module, 0x002C, "HeapCreate");
    registerExport(module, 0x002D, "HeapDestroy");
    registerExport(module, 0x002E, "HeapAlloc");
    registerExport(module, 0x002F, "HeapReAlloc");
    registerExport(module, 0x0030, "HeapSize");
    registerExport(module, 0x0031, "HeapFree");
    registerExport(module, 0x0032, "GetProcessHeap");
    registerExport(module, 0x0038, "wsprintfW");
    registerExport(module, 0x003B, "wcschr");
    registerExport(module, 0x003D, "wcscpy");
    registerExport(module, 0x003E, "wcscspn");
    registerExport(module, 0x003F, "wcslen");
    registerExport(module, 0x0041, "wcsncmp");
    registerExport(module, 0x0045, "wcsrchr");
    registerExport(module, 0x004A, "_wcsdup");
    registerExport(module, 0x004E, "_wtol");
    registerExport(module, 0x0058, "GlobalMemoryStatus");
    registerExport(module, 0x005E, "LoadAcceleratorsW");
    registerExport(module, 0x005F, "RegisterClassW");
    registerExport(module, 0x0060, "CopyRect");
    registerExport(module, 0x0062, "InflateRect");
    registerExport(module, 0x0064, "IsRectEmpty");
    registerExport(module, 0x0066, "PtInRect");
    registerExport(module, 0x0068, "SetRectEmpty");
    registerExport(module, 0x00A5, "DeleteFileW");
    registerExport(module, 0x00A7, "FindFirstFileW");
    registerExport(module, 0x00A8, "CreateFileW");
    registerExport(module, 0x00AA, "ReadFile");
    registerExport(module, 0x00AB, "WriteFile");
    registerExport(module, 0x00AC, "GetFileSize");
    registerExport(module, 0x00AD, "SetFilePointer");
    registerExport(module, 0x00B1, "SetFileTime");
    registerExport(module, 0x00B4, "FindClose");
    registerExport(module, 0x00C0, "IsDBCSLeadByteEx");
    registerExport(module, 0x00C1, "iswctype");
    registerExport(module, 0x00C4, "MultiByteToWideChar");
    registerExport(module, 0x00E5, "_wcsnicmp");
    registerExport(module, 0x00F6, "CreateWindowExW");
    registerExport(module, 0x00F7, "SetWindowPos");
    registerExport(module, 0x00F9, "GetClientRect");
    registerExport(module, 0x00FA, "InvalidateRect");
    registerExport(module, 0x00FB, "GetWindow");
    registerExport(module, 0x0102, "SetWindowLongW");
    registerExport(module, 0x0103, "GetWindowLongW");
    registerExport(module, 0x0104, "BeginPaint");
    registerExport(module, 0x0105, "EndPaint");
    registerExport(module, 0x0106, "GetDC");
    registerExport(module, 0x0107, "ReleaseDC");
    registerExport(module, 0x0108, "DefWindowProcW");
    registerExport(module, 0x0109, "DestroyWindow");
    registerExport(module, 0x010A, "ShowWindow");
    registerExport(module, 0x010B, "UpdateWindow");
    registerExport(module, 0x010D, "GetParent");
    registerExport(module, 0x0116, "ValidateRect");
    registerExport(module, 0x011D, "CallWindowProcW");
    registerExport(module, 0x011E, "FindWindowW");
    registerExport(module, 0x0143, "GetStoreInformation");
    registerExport(module, 0x0195, "waveInUnprepareHeader");
    registerExport(module, 0x0196, "waveInAddBuffer");
    registerExport(module, 0x0199, "waveInReset");
    registerExport(module, 0x019C, "waveInMessage");
    registerExport(module, 0x019D, "waveInOpen");
    registerExport(module, 0x01BE, "WNetConnectionDialog1W");
    registerExport(module, 0x01C2, "WNetGetUniversalNameW");
    registerExport(module, 0x01C3, "WNetGetUserW");
    registerExport(module, 0x01C8, "RegCreateKeyExW");
    registerExport(module, 0x01EF, "CreateEventW");
    registerExport(module, 0x01EE, "EventModify");
    registerExport(module, 0x01F0, "Sleep");
    registerExport(module, 0x01F1, "WaitForSingleObject");
    registerExport(module, 0x0204, "GetLastError");
    registerExport(module, 0x0205, "SetLastError");
    registerExport(module, 0x0208, "TlsCall");
    registerExport(module, 0x020C, "VirtualAlloc");
    registerExport(module, 0x020D, "VirtualFree");
    registerExport(module, 0x0210, "LoadLibraryW");
    registerExport(module, 0x0212, "GetProcAddressW");
    registerExport(module, 0x0213, "FindResource");
    registerExport(module, 0x0214, "FindResourceW");
    registerExport(module, 0x0215, "LoadResource");
    registerExport(module, 0x0216, "SizeofResource");
    registerExport(module, 0x021E, "GetSystemInfo");
    registerExport(module, 0x0217, "GetTickCount");
    registerExport(module, 0x0219, "GetModuleFileNameW");
    registerExport(module, 0x021A, "QueryPerformanceCounter");
    registerExport(module, 0x021B, "QueryPerformanceFrequency");
    registerExport(module, 0x021D, "OutputDebugStringW");
    registerExport(module, 0x0229, "CloseHandle");
    registerExport(module, 0x022B, "CreateMutexW");
    registerExport(module, 0x022C, "ReleaseMutex");
    registerExport(module, 0x02B4, "GetDlgItem");
    registerExport(module, 0x02CD, "GetVersionExW");
    registerExport(module, 0x02DD, "LocalAllocTrace");
    registerExport(module, 0x02DE, "GetCursorPos");
    registerExport(module, 0x02D8, "LoadIconW");
    registerExport(module, 0x02DA, "LoadImageW");
    registerExport(module, 0x034B, "RemoveMenu");
    registerExport(module, 0x034E, "LoadMenuW");
    registerExport(module, 0x0350, "CheckMenuItem");
    registerExport(module, 0x0351, "CheckMenuRadioItem");
    registerExport(module, 0x036A, "LoadStringW");
    registerExport(module, 0x036C, "KillTimer");
    registerExport(module, 0x036E, "GetClassInfoW");
    registerExport(module, 0x035B, "DispatchMessageW");
    registerExport(module, 0x035D, "GetMessageW");
    registerExport(module, 0x035E, "GetMessagePos");
    registerExport(module, 0x035F, "GetMessageWNoWait");
    registerExport(module, 0x0360, "PeekMessageW");
    registerExport(module, 0x0361, "PostMessageW");
    registerExport(module, 0x0362, "PostQuitMessage");
    registerExport(module, 0x0364, "SendMessageW");
    registerExport(module, 0x0366, "TranslateMessage");
    registerExport(module, 0x0375, "GetSystemMetrics");
    registerExport(module, 0x0376, "IsWindowVisible");
    registerExport(module, 0x0379, "GetSysColor");
    registerExport(module, 0x037F, "CreateFontIndirectW");
    registerExport(module, 0x0380, "ExtTextOutW");
    registerExport(module, 0x0387, "BitBlt");
    registerExport(module, 0x038E, "CreateCompatibleDC");
    registerExport(module, 0x0390, "DeleteObject");
    registerExport(module, 0x0397, "GetStockObject");
    registerExport(module, 0x0399, "SelectObject");
    registerExport(module, 0x039A, "SetBkColor");
    registerExport(module, 0x039B, "SetBkMode");
    registerExport(module, 0x039C, "SetTextColor");
    registerExport(module, 0x03A3, "CreateSolidBrush");
    registerExport(module, 0x03A7, "FillRect");
    registerExport(module, 0x03AA, "PatBlt");
    registerExport(module, 0x03AD, "Rectangle");
    registerExport(module, 0x03B1, "DrawTextW");
    registerExport(module, 0x037B, "RegisterWindowMessageW");
    registerExport(module, 0x037C, "RegisterTaskBar");
    registerExport(module, 0x04A1, "GetDCEx");
    registerExport(module, 0x0449, "swprintf");
    registerExport(module, 0x044B, "vswprintf");
    registerExport(module, 0x01C7, "RegCloseKey");
    registerExport(module, 0x01CD, "RegOpenKeyExW");
    registerExport(module, 0x01CF, "RegQueryValueExW");
    registerExport(module, 0x01C9, "RegDeleteKeyW");
    registerExport(module, 0x01CA, "RegDeleteValueW");
    registerExport(module, 0x01CB, "RegEnumValueW");
    registerExport(module, 0x01CC, "RegEnumKeyExW");
    registerExport(module, 0x01CE, "RegQueryInfoKeyW");
    registerExport(module, 0x01D0, "RegSetValueExW");
    registerExport(module, 0x0480, "RegFlushKey");
    registerExport(module, 0x03FA, "free");
    registerExport(module, 0x040C, "longjmp");
    registerExport(module, 0x0411, "malloc");
    registerExport(module, 0x0414, "memcpy");
    registerExport(module, 0x0416, "memmove");
    registerExport(module, 0x0417, "memset");
    registerExport(module, 0x041E, "realloc");
    registerExport(module, 0x0429, "strcmp");
    registerExport(module, 0x042A, "strcpy");
    registerExport(module, 0x042B, "strcspn");
    registerExport(module, 0x042C, "strlen");
    registerExport(module, 0x0446, "operator_delete");
    registerExport(module, 0x0447, "operator_new");
    registerExport(module, 0x0499, "GetModuleHandleW");
    registerExport(module, 0x04BD, "IsProcessDying");
    registerExport(module, 0x04CB, "GetCRTStorageEx");
    registerExport(module, 0x04CC, "GetCRTFlags");
    registerExport(module, 0x04CE, "GetProcAddressA");
    registerExport(module, 0x04D1, "TryEnterCriticalSection");
    registerExport(module, 0x057C, "strtol");
    registerExport(module, 0x0582, "_stricmp");
    registerExport(module, 0x0583, "_strnicmp");
    registerExport(module, 0x05B0, "operator_vector_new");
    registerExport(module, 0x05D3, "InterlockedExchangeAdd");
    registerExport(module, 0x05D4, "InterlockedCompareExchange");
    registerExport(module, 0x05E3, "RegisterDesktop");
    registerExport(module, 0x05EF, "GlobalAddAtomW");
    registerExport(module, 0x05F0, "GlobalDeleteAtom");
    registerExport(module, 0x05F1, "GlobalFindAtomW");
    registerExport(module, 0x0635, "mixerGetControlDetails");
    registerExport(module, 0x0646, "RemoteHeapFree");
    registerExport(module, 0x0673, "MoveToEx");
    registerExport(module, 0x0674, "LineTo");
    registerExport(module, 0x0676, "SetTextAlign");
    registerExport(module, 0x0683, "StretchDIBits");
    registerExport(module, 0x07D0, "_setjmp");
    registerExport(module, 0x07D5, "__ll_div");
    registerExport(module, 0x0532, "operator_new");
    registerExport(module, 0x0533, "operator_vector_new");
    registerExport(module, 0x0535, "operator_new_nothrow");
    registerExport(module, 0x0536, "operator_vector_new_nothrow");
    registerExport(module, 0x0531, "operator_delete");
    registerExport(module, 0x0534, "operator_vector_delete");
    registerExport(module, 0x0537, "operator_delete_nothrow");
    registerExport(module, 0x0538, "operator_vector_delete_nothrow");

    spdlog::info("mapped synthetic COREDLL.dll base=0x{:08x} ordinals={}",
                 module.imageBase, module.exportsByOrdinal.size());
    return module;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createGenericOrdinalDll(
    const std::string& moduleName, uint16_t maxOrdinal) {
    SyntheticModule module;
    module.moduleName = moduleName;
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00010000;
    nextModuleBase_ += 0x00010000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic " + moduleName);
    }
    for (uint16_t ordinal = 1; ordinal <= maxOrdinal; ++ordinal) {
        registerExport(module, ordinal, {});
    }
    spdlog::info("mapped synthetic {} base=0x{:08x} ordinals={}",
                 moduleName, module.imageBase, module.exportsByOrdinal.size());
    return module;
}

void SyntheticDllRuntime::registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name) {
    const uint32_t rva = 0x1000 + uint32_t(ordinal) * 8;
    module.exportsByOrdinal[ordinal] = rva;
    if (!name.empty()) {
        module.exportNamesByOrdinal[ordinal] = name;
        module.exportsByName[lowerAscii(name)] = rva;
    }
    const uint32_t address = module.imageBase + rva;
    auto& entry = exportsByAddress_[address];
    entry.moduleName = module.moduleName;
    entry.ordinal = ordinal;
    if (!name.empty()) entry.name = name;
    writeStub(address);
}

void SyntheticDllRuntime::writeStub(uint32_t address) {
    const std::array<uint8_t, 8> stub = {
        0x08, 0x00, 0xe0, 0x03, // jr ra
        0x00, 0x00, 0x00, 0x00, // nop
    };
    uc_mem_write(uc_, address, stub.data(), stub.size());
}

void SyntheticDllRuntime::hookCode(uc_engine* uc, uint64_t address, uint32_t, void* user) {
    auto* runtime = static_cast<SyntheticDllRuntime*>(user);
    auto it = runtime->exportsByAddress_.find(uint32_t(address));
    if (it == runtime->exportsByAddress_.end()) return;
    runtime->dispatch(it->second);
}

uint32_t SyntheticDllRuntime::makeGuestHandle(GuestHandle handle) {
    const uint32_t guest = nextHandle_++;
    guestHandles_[guest] = handle;
    return guest;
}

SyntheticDllRuntime::GuestHandle* SyntheticDllRuntime::lookupGuestHandle(uint32_t guestHandle) {
    auto it = guestHandles_.find(guestHandle);
    return it == guestHandles_.end() ? nullptr : &it->second;
}

uint32_t SyntheticDllRuntime::closeGuestHandle(uint32_t guestHandle) {
    auto it = guestHandles_.find(guestHandle);
    if (it == guestHandles_.end()) {
        lastError_ = 6; // ERROR_INVALID_HANDLE
        return 0;
    }
    if (it->second.kind == GuestHandle::Kind::GuestWindow) {
        windows_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestDc) {
        dcs_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestBrush) {
        brushes_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestFont) {
        fonts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestRegistryKey) {
        registryHandles_.erase(guestHandle);
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
        } else if (it->second.kind == GuestHandle::Kind::HostWaveIn) {
            auto& winmm = winmmBridge();
            if (winmm.waveInClose) winmm.waveInClose(reinterpret_cast<HWAVEIN>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostMenu) {
            DestroyMenu(reinterpret_cast<HMENU>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostAccelerator) {
            DestroyAcceleratorTable(reinterpret_cast<HACCEL>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostIcon) {
            DestroyIcon(reinterpret_cast<HICON>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostBitmap) {
            DeleteObject(reinterpret_cast<HGDIOBJ>(it->second.hostValue));
        } else {
            CloseHandle(reinterpret_cast<HANDLE>(it->second.hostValue));
        }
    }
#endif
    guestHandles_.erase(it);
    lastError_ = 0;
    return 1;
}

std::optional<std::string> SyntheticDllRuntime::registryPathFromHandle(uint32_t hkey, const std::string& subKey) const {
    std::string base = registryRootName(hkey);
    if (base.empty()) {
        auto handle = guestHandles_.find(hkey);
        auto path = registryHandles_.find(hkey);
        if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestRegistryKey ||
            path == registryHandles_.end()) {
            return std::nullopt;
        }
        base = path->second;
    }
    const std::string suffix = normalizeRegistrySubKey(subKey);
    return suffix.empty() ? base : base + "\\" + suffix;
}

bool SyntheticDllRuntime::registryKeyExists(const std::string& path) const {
    const auto keys = registry_.find("keys");
    return keys != registry_.end() && keys->is_object() && keys->contains(path);
}

void SyntheticDllRuntime::registryEnsureKey(const std::string& path) {
    if (!registry_.contains("keys") || !registry_["keys"].is_object()) registry_["keys"] = nlohmann::json::object();
    auto& keys = registry_["keys"];
    if (!keys.contains(path) || !keys[path].is_object()) {
        keys[path] = nlohmann::json{{"values", nlohmann::json::object()}};
        registryDirty_ = true;
    } else if (!keys[path].contains("values") || !keys[path]["values"].is_object()) {
        keys[path]["values"] = nlohmann::json::object();
        registryDirty_ = true;
    }
}

uint32_t SyntheticDllRuntime::makeRegistryHandle(const std::string& path) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestRegistryKey, 0, 0});
    registryHandles_[handle] = path;
    return handle;
}

std::vector<std::string> SyntheticDllRuntime::registryChildNames(const std::string& path) const {
    std::set<std::string> names;
    const auto keys = registry_.find("keys");
    if (keys == registry_.end() || !keys->is_object()) return {};
    const std::string prefix = path + "\\";
    for (auto it = keys->begin(); it != keys->end(); ++it) {
        const std::string key = it.key();
        if (key.rfind(prefix, 0) != 0) continue;
        std::string rest = key.substr(prefix.size());
        const size_t slash = rest.find('\\');
        if (slash != std::string::npos) rest.resize(slash);
        if (!rest.empty()) names.insert(rest);
    }
    return {names.begin(), names.end()};
}

nlohmann::json* SyntheticDllRuntime::registryValue(const std::string& path, const std::string& valueName) {
    if (!registryKeyExists(path)) return nullptr;
    auto& values = registry_["keys"][path]["values"];
    const std::string key = normalizeRegistryValueName(valueName);
    return values.contains(key) ? &values[key] : nullptr;
}

const nlohmann::json* SyntheticDllRuntime::registryValue(const std::string& path, const std::string& valueName) const {
    const auto keys = registry_.find("keys");
    if (keys == registry_.end() || !keys->is_object() || !keys->contains(path)) return nullptr;
    const auto values = (*keys)[path].find("values");
    if (values == (*keys)[path].end() || !values->is_object()) return nullptr;
    const std::string key = normalizeRegistryValueName(valueName);
    return values->contains(key) ? &(*values)[key] : nullptr;
}

std::vector<uint8_t> SyntheticDllRuntime::registryValueBytes(const nlohmann::json& value) const {
    const uint32_t type = registryTypeId(value);
    std::vector<uint8_t> bytes;
    const auto data = value.find("data");
    if (type == 1 || type == 2 || type == 7) {
        std::string text;
        if (data != value.end() && data->is_string()) text = data->get<std::string>();
        bytes.resize((text.size() + 1) * 2);
        for (size_t i = 0; i < text.size(); ++i) bytes[i * 2] = uint8_t(text[i]);
    } else if (type == 4) {
        uint32_t dword = 0;
        if (data != value.end() && data->is_number_unsigned()) dword = data->get<uint32_t>();
        bytes = {uint8_t(dword), uint8_t(dword >> 8), uint8_t(dword >> 16), uint8_t(dword >> 24)};
    } else if (data != value.end() && data->is_array()) {
        for (const auto& item : *data) bytes.push_back(uint8_t(item.get<uint32_t>() & 0xffu));
    } else if (data != value.end() && data->is_string()) {
        const std::string text = data->get<std::string>();
        bytes.assign(text.begin(), text.end());
    }
    return bytes;
}

nlohmann::json SyntheticDllRuntime::registryJsonFromBytes(uint32_t type, uint32_t dataPtr, uint32_t dataSize) const {
    nlohmann::json value = nlohmann::json::object();
    value["type"] = registryTypeName(type);
    if (type == 1 || type == 2 || type == 7) {
        std::string text;
        const uint32_t chars = dataSize / 2;
        for (uint32_t i = 0; dataPtr && i < chars; ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, dataPtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
            text.push_back(ch < 0x80 ? char(ch) : '?');
        }
        value["data"] = text;
    } else if (type == 4 && dataPtr && dataSize >= 4) {
        value["data"] = readU32(dataPtr);
    } else {
        std::vector<uint8_t> bytes(dataSize);
        if (dataPtr && dataSize && uc_mem_read(uc_, dataPtr, bytes.data(), bytes.size()) == UC_ERR_OK) {
            value["data"] = bytes;
        } else {
            value["data"] = nlohmann::json::array();
        }
    }
    return value;
}

uint32_t SyntheticDllRuntime::makeGuestDc(uint32_t hwnd) {
    GuestDc dc{};
    dc.hwnd = hwnd;
    dc.selectedBrush = makeStockObject(4); // BLACK_BRUSH
    dc.selectedFont = makeStockObject(17); // DEFAULT_GUI_FONT
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestDc, 0, 0});
    dcs_[handle] = dc;
    return handle;
}

SyntheticDllRuntime::GuestDc* SyntheticDllRuntime::lookupGuestDc(uint32_t hdc) {
    auto handle = guestHandles_.find(hdc);
    if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestDc) return nullptr;
    auto dc = dcs_.find(hdc);
    return dc == dcs_.end() ? nullptr : &dc->second;
}

uint32_t SyntheticDllRuntime::makeGuestBrush(uint32_t colorRef, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestBrush, 0, stock ? 1u : 0u});
    brushes_[handle] = GuestBrush{colorRef, stock};
    return handle;
}

uint32_t SyntheticDllRuntime::makeGuestFont(const std::array<uint8_t, 92>& logFont, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestFont, 0, stock ? 1u : 0u});
    fonts_[handle] = GuestFont{logFont, stock};
    return handle;
}

uint32_t SyntheticDllRuntime::makeStockObject(int32_t index) {
    auto existing = stockObjects_.find(index);
    if (existing != stockObjects_.end()) return existing->second;

    uint32_t handle = 0;
    switch (index) {
    case 0: handle = makeGuestBrush(0x00ffffff, true); break; // WHITE_BRUSH
    case 1: handle = makeGuestBrush(0x00c0c0c0, true); break; // LTGRAY_BRUSH
    case 2: handle = makeGuestBrush(0x00808080, true); break; // GRAY_BRUSH
    case 3: handle = makeGuestBrush(0x00404040, true); break; // DKGRAY_BRUSH
    case 4: handle = makeGuestBrush(0x00000000, true); break; // BLACK_BRUSH
    case 5: handle = makeGuestBrush(0xffffffffu, true); break; // NULL_BRUSH
    case 6: handle = makeGuestBrush(0x00ffffff, true); break; // WHITE_PEN
    case 7: handle = makeGuestBrush(0x00000000, true); break; // BLACK_PEN
    case 8: handle = makeGuestBrush(0xffffffffu, true); break; // NULL_PEN
    case 13:
    case 17: {
        std::array<uint8_t, 92> font{};
        handle = makeGuestFont(font, true);
        break;
    }
    default:
        return 0;
    }
    stockObjects_[index] = handle;
    return handle;
}

uint32_t SyntheticDllRuntime::colorRefToPixel(uint32_t colorRef) const {
    if (colorRef == 0xffffffffu) return 0;
    return 0xff000000u | ((colorRef & 0x000000ffu) << 16) |
           (colorRef & 0x0000ff00u) | ((colorRef >> 16) & 0x000000ffu);
}

bool SyntheticDllRuntime::readGuestRect(uint32_t address,
                                        int32_t& left,
                                        int32_t& top,
                                        int32_t& right,
                                        int32_t& bottom) const {
    if (!address) return false;
    std::array<uint32_t, 4> rect{};
    if (uc_mem_read(uc_, address, rect.data(), rect.size() * sizeof(uint32_t)) != UC_ERR_OK) return false;
    left = int32_t(rect[0]);
    top = int32_t(rect[1]);
    right = int32_t(rect[2]);
    bottom = int32_t(rect[3]);
    return true;
}

void SyntheticDllRuntime::writeGuestRect(uint32_t address,
                                         int32_t left,
                                         int32_t top,
                                         int32_t right,
                                         int32_t bottom) const {
    if (!address) return;
    writeU32(address, uint32_t(left));
    writeU32(address + 4, uint32_t(top));
    writeU32(address + 8, uint32_t(right));
    writeU32(address + 12, uint32_t(bottom));
}

void SyntheticDllRuntime::fillFramebufferRect(const GuestDc& dc,
                                              int32_t left,
                                              int32_t top,
                                              int32_t right,
                                              int32_t bottom,
                                              uint32_t pixel) {
    if (!framebuffer_ || pixel == 0 || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    int32_t originX = 0;
    int32_t originY = 0;
    auto window = windows_.find(dc.hwnd);
    if (window != windows_.end()) {
        originX = window->second.x;
        originY = window->second.y;
    }
    left += originX;
    right += originX;
    top += originY;
    bottom += originY;
    if (left > right) std::swap(left, right);
    if (top > bottom) std::swap(top, bottom);
    left = std::clamp<int32_t>(left, 0, framebufferWidth_);
    right = std::clamp<int32_t>(right, 0, framebufferWidth_);
    top = std::clamp<int32_t>(top, 0, framebufferHeight_);
    bottom = std::clamp<int32_t>(bottom, 0, framebufferHeight_);
    for (int32_t y = top; y < bottom; ++y) {
        uint32_t* row = framebuffer_ + size_t(y) * size_t(framebufferWidth_);
        for (int32_t x = left; x < right; ++x) row[x] = pixel;
    }
}

void SyntheticDllRuntime::drawFramebufferLine(const GuestDc& dc,
                                              int32_t x0,
                                              int32_t y0,
                                              int32_t x1,
                                              int32_t y1,
                                              uint32_t pixel) {
    if (!framebuffer_ || pixel == 0 || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    int32_t originX = 0;
    int32_t originY = 0;
    auto window = windows_.find(dc.hwnd);
    if (window != windows_.end()) {
        originX = window->second.x;
        originY = window->second.y;
    }
    x0 += originX;
    x1 += originX;
    y0 += originY;
    y1 += originY;
    const int32_t dx = std::abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < framebufferWidth_ && y0 >= 0 && y0 < framebufferHeight_) {
            framebuffer_[size_t(y0) * size_t(framebufferWidth_) + size_t(x0)] = pixel;
        }
        if (x0 == x1 && y0 == y1) break;
        const int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

bool SyntheticDllRuntime::stretchDibToFramebuffer(const GuestDc& dc,
                                                  int32_t dstX,
                                                  int32_t dstY,
                                                  int32_t dstW,
                                                  int32_t dstH,
                                                  int32_t srcX,
                                                  int32_t srcY,
                                                  int32_t srcW,
                                                  int32_t srcH,
                                                  uint32_t bitsPtr,
                                                  uint32_t infoPtr) {
    if (!framebuffer_ || !bitsPtr || !infoPtr || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) return false;
    std::array<uint8_t, 40> header{};
    if (uc_mem_read(uc_, infoPtr, header.data(), header.size()) != UC_ERR_OK) return false;
    const uint32_t headerSize = uint32_t(header[0]) | (uint32_t(header[1]) << 8) |
                                (uint32_t(header[2]) << 16) | (uint32_t(header[3]) << 24);
    const int32_t dibWidth = int32_t(uint32_t(header[4]) | (uint32_t(header[5]) << 8) |
                                    (uint32_t(header[6]) << 16) | (uint32_t(header[7]) << 24));
    const int32_t dibHeightRaw = int32_t(uint32_t(header[8]) | (uint32_t(header[9]) << 8) |
                                        (uint32_t(header[10]) << 16) | (uint32_t(header[11]) << 24));
    const uint16_t planes = uint16_t(header[12] | (header[13] << 8));
    const uint16_t bpp = uint16_t(header[14] | (header[15] << 8));
    const uint32_t compression = uint32_t(header[16]) | (uint32_t(header[17]) << 8) |
                                 (uint32_t(header[18]) << 16) | (uint32_t(header[19]) << 24);
    const uint32_t clrUsed = uint32_t(header[32]) | (uint32_t(header[33]) << 8) |
                             (uint32_t(header[34]) << 16) | (uint32_t(header[35]) << 24);
    if (headerSize < 40 || planes != 1 || compression != 0 || dibWidth <= 0 || dibHeightRaw == 0) return false;
    const int32_t dibHeight = std::abs(dibHeightRaw);
    const bool topDown = dibHeightRaw < 0;
    const uint32_t paletteEntries = bpp <= 8 ? (clrUsed ? clrUsed : (1u << bpp)) : 0;
    std::vector<uint32_t> palette(paletteEntries);
    if (paletteEntries) {
        std::vector<uint8_t> rawPalette(size_t(paletteEntries) * 4);
        if (uc_mem_read(uc_, infoPtr + headerSize, rawPalette.data(), rawPalette.size()) != UC_ERR_OK) return false;
        for (uint32_t i = 0; i < paletteEntries; ++i) {
            const uint8_t b = rawPalette[size_t(i) * 4 + 0];
            const uint8_t g = rawPalette[size_t(i) * 4 + 1];
            const uint8_t r = rawPalette[size_t(i) * 4 + 2];
            palette[i] = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        }
    }
    const uint32_t rowStride = ((uint32_t(dibWidth) * uint32_t(bpp) + 31u) / 32u) * 4u;
    if (!rowStride || rowStride > 0x100000u || uint64_t(rowStride) * uint64_t(dibHeight) > 0x2000000ull) return false;
    std::vector<uint8_t> bits(size_t(rowStride) * size_t(dibHeight));
    if (uc_mem_read(uc_, bitsPtr, bits.data(), bits.size()) != UC_ERR_OK) return false;

    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    int32_t originX = 0;
    int32_t originY = 0;
    auto window = windows_.find(dc.hwnd);
    if (window != windows_.end()) {
        originX = window->second.x;
        originY = window->second.y;
    }
    outLeft += originX;
    outTop += originY;
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < 0 || dstPy >= framebufferHeight_) continue;
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        if (sy < 0 || sy >= dibHeight) continue;
        const int32_t row = topDown ? sy : (dibHeight - 1 - sy);
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < 0 || dstPx >= framebufferWidth_) continue;
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            if (sx < 0 || sx >= dibWidth) continue;
            const uint8_t* p = bits.data() + size_t(row) * rowStride;
            uint32_t pixel = 0;
            if (bpp == 32) {
                const size_t o = size_t(sx) * 4;
                pixel = 0xff000000u | (uint32_t(p[o + 2]) << 16) | (uint32_t(p[o + 1]) << 8) | p[o];
            } else if (bpp == 24) {
                const size_t o = size_t(sx) * 3;
                pixel = 0xff000000u | (uint32_t(p[o + 2]) << 16) | (uint32_t(p[o + 1]) << 8) | p[o];
            } else if (bpp == 16) {
                const uint16_t v = uint16_t(p[size_t(sx) * 2] | (p[size_t(sx) * 2 + 1] << 8));
                const uint8_t r = uint8_t(((v >> 11) & 0x1f) * 255 / 31);
                const uint8_t g = uint8_t(((v >> 5) & 0x3f) * 255 / 63);
                const uint8_t b = uint8_t((v & 0x1f) * 255 / 31);
                pixel = 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
            } else if (bpp == 8 && !palette.empty()) {
                pixel = palette[p[sx]];
            } else {
                return false;
            }
            framebuffer_[size_t(dstPy) * size_t(framebufferWidth_) + size_t(dstPx)] = pixel;
        }
    }
    return true;
}

void SyntheticDllRuntime::loadMainResources(const std::filesystem::path& path) {
    mainResources_.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        spdlog::warn("resource parse skipped; cannot open {}", path.string());
        return;
    }
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.size() < 0x100 || readLe16(bytes, 0) != 0x5a4d) return;
    const uint32_t nt = readLe32(bytes, 0x3c);
    if (nt + 0x18 >= bytes.size() || readLe32(bytes, nt) != 0x4550) return;
    const uint16_t sectionCount = readLe16(bytes, nt + 6);
    const uint16_t optSize = readLe16(bytes, nt + 20);
    const uint32_t opt = nt + 24;
    if (opt + 112 > bytes.size() || readLe16(bytes, opt) != 0x10b) return;
    const uint32_t sizeOfHeaders = readLe32(bytes, opt + 60);
    const uint32_t resourceRva = readLe32(bytes, opt + 96 + 2 * 8);
    const uint32_t resourceSize = readLe32(bytes, opt + 100 + 2 * 8);
    if (!resourceRva || !resourceSize) return;

    struct SectionView { uint32_t va{}, vsize{}, raw{}, rawSize{}; };
    std::vector<SectionView> sections;
    const uint32_t sh = opt + optSize;
    for (uint16_t i = 0; i < sectionCount && sh + i * 40 + 40 <= bytes.size(); ++i) {
        SectionView s;
        s.vsize = readLe32(bytes, sh + i * 40 + 8);
        s.va = readLe32(bytes, sh + i * 40 + 12);
        s.rawSize = readLe32(bytes, sh + i * 40 + 16);
        s.raw = readLe32(bytes, sh + i * 40 + 20);
        sections.push_back(s);
    }
    auto rvaToFile = [&](uint32_t rva) -> std::optional<uint32_t> {
        if (rva < sizeOfHeaders) return rva;
        for (const auto& s : sections) {
            const uint32_t span = std::max(s.vsize, s.rawSize);
            if (rva >= s.va && rva < s.va + span) return s.raw + (rva - s.va);
        }
        return std::nullopt;
    };
    const auto resourceBase = rvaToFile(resourceRva);
    if (!resourceBase) return;
    const uint32_t resourceEnd = *resourceBase + resourceSize;
    if (*resourceBase >= bytes.size()) return;

    auto readResourceName = [&](uint32_t raw) -> ResourceName {
        ResourceName result{};
        if (raw & 0x80000000u) {
            const uint32_t offset = *resourceBase + (raw & 0x7fffffffu);
            const uint16_t length = readLe16(bytes, offset);
            result.ordinal = false;
            result.name = lowerAscii(utf16FromBytes(bytes, offset + 2, length));
        } else {
            result.ordinal = true;
            result.id = raw & 0xffffu;
        }
        return result;
    };
    std::function<void(uint32_t, int, ResourceName, ResourceName)> walk =
        [&](uint32_t dirOffset, int depth, ResourceName type, ResourceName name) {
            const uint32_t dir = *resourceBase + dirOffset;
            if (dir + 16 > bytes.size() || dir >= resourceEnd) return;
            const uint16_t named = readLe16(bytes, dir + 12);
            const uint16_t ids = readLe16(bytes, dir + 14);
            const uint32_t count = uint32_t(named) + ids;
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t entry = dir + 16 + i * 8;
                if (entry + 8 > bytes.size() || entry >= resourceEnd) return;
                ResourceName current = readResourceName(readLe32(bytes, entry));
                const uint32_t data = readLe32(bytes, entry + 4);
                if (data & 0x80000000u) {
                    if (depth == 0) walk(data & 0x7fffffffu, depth + 1, current, {});
                    else if (depth == 1) walk(data & 0x7fffffffu, depth + 1, type, current);
                } else if (depth >= 2) {
                    const uint32_t dataEntry = *resourceBase + data;
                    if (dataEntry + 16 > bytes.size() || dataEntry >= resourceEnd) continue;
                    const uint32_t dataRva = readLe32(bytes, dataEntry);
                    const uint32_t dataSize = readLe32(bytes, dataEntry + 4);
                    const auto dataOff = rvaToFile(dataRva);
                    if (!dataOff || *dataOff + dataSize > bytes.size()) continue;
                    ResourceEntry resource{};
                    resource.type = type;
                    resource.name = name;
                    resource.language = uint16_t(current.id);
                    resource.data.assign(bytes.begin() + *dataOff, bytes.begin() + *dataOff + dataSize);
                    mainResources_.push_back(std::move(resource));
                }
            }
        };
    walk(0, 0, {}, {});
    spdlog::info("parsed {} resources from {}", mainResources_.size(), path.filename().string());
}

bool SyntheticDllRuntime::resourceNameMatches(const ResourceName& resourceName, uint32_t guestArg) const {
    if (guestArg < 0x10000) {
        return resourceName.ordinal && resourceName.id == guestArg;
    }
    return !resourceName.ordinal && resourceName.name == lowerAscii(readUtf16(guestArg));
}

const SyntheticDllRuntime::ResourceEntry* SyntheticDllRuntime::findResource(uint32_t typeArg,
                                                                             uint32_t nameArg) const {
    for (const auto& resource : mainResources_) {
        if (resourceNameMatches(resource.type, typeArg) && resourceNameMatches(resource.name, nameArg)) {
            return &resource;
        }
    }
    if (typeArg == 6 && nameArg < 0x10000) {
        const uint32_t blockId = (nameArg >> 4) + 1;
        for (const auto& resource : mainResources_) {
            if (resource.type.ordinal && resource.type.id == 6 &&
                resource.name.ordinal && resource.name.id == blockId) {
                return &resource;
            }
        }
    }
    return nullptr;
}

const SyntheticDllRuntime::ResourceEntry* SyntheticDllRuntime::resourceFromHandle(uint32_t guestHandle) const {
    auto handle = guestHandles_.find(guestHandle);
    if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestResource ||
        !handle->second.hostValue) {
        return nullptr;
    }
    const size_t index = size_t(handle->second.hostValue - 1);
    return index < mainResources_.size() ? &mainResources_[index] : nullptr;
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

uint32_t SyntheticDllRuntime::reg(int regId) const {
    uint32_t value = 0;
    uc_reg_read(uc_, regId, &value);
    return value;
}

void SyntheticDllRuntime::setReg(int regId, uint32_t value) const {
    uc_reg_write(uc_, regId, &value);
}

uint32_t SyntheticDllRuntime::stackArg(uint32_t index) const {
    uint32_t value = 0;
    const uint32_t sp = reg(UC_MIPS_REG_SP);
    uc_mem_read(uc_, sp + index * 4, &value, sizeof(value));
    return value;
}

uint32_t SyntheticDllRuntime::allocate(uint32_t size, bool zeroFill) {
    size = std::max<uint32_t>(size, 1);
    const uint32_t aligned = (size + 0x0fff) & ~0x0fffu;
    if (nextHeap_ + aligned >= heapLimit_) {
        lastError_ = 14; // ERROR_OUTOFMEMORY
        return 0;
    }
    const uint32_t address = nextHeap_;
    nextHeap_ += aligned;
    allocationSizes_[address] = size;
    if (zeroFill) {
        std::vector<uint8_t> zeros(aligned);
        uc_mem_write(uc_, address, zeros.data(), zeros.size());
    }
    lastError_ = 0;
    return address;
}

uint32_t SyntheticDllRuntime::readU32(uint32_t address) const {
    uint32_t value = 0;
    if (address) uc_mem_read(uc_, address, &value, sizeof(value));
    return value;
}

void SyntheticDllRuntime::writeU32(uint32_t address, uint32_t value) const {
    if (address) uc_mem_write(uc_, address, &value, sizeof(value));
}

bool SyntheticDllRuntime::copyGuest(uint32_t dst, uint32_t src, uint32_t size) const {
    if (!dst || !src || !size || size > 0x100000) return false;
    std::vector<uint8_t> bytes(size);
    if (uc_mem_read(uc_, src, bytes.data(), bytes.size()) != UC_ERR_OK) return false;
    return uc_mem_write(uc_, dst, bytes.data(), bytes.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::fillGuest(uint32_t dst, uint8_t value, uint32_t size) const {
    if (!dst || !size || size > 0x100000) return false;
    std::vector<uint8_t> bytes(size, value);
    return uc_mem_write(uc_, dst, bytes.data(), bytes.size()) == UC_ERR_OK;
}

std::string SyntheticDllRuntime::readAscii(uint32_t address, size_t maxChars) const {
    std::string out;
    for (size_t i = 0; address && i < maxChars; ++i) {
        char ch = 0;
        if (uc_mem_read(uc_, address + uint32_t(i), &ch, sizeof(ch)) != UC_ERR_OK) break;
        if (!ch) break;
        out.push_back(ch);
    }
    return out;
}

void SyntheticDllRuntime::writeAscii(uint32_t address, const std::string& value) const {
    if (!address) return;
    uc_mem_write(uc_, address, value.data(), value.size());
    const char nul = 0;
    uc_mem_write(uc_, address + uint32_t(value.size()), &nul, sizeof(nul));
}

std::string SyntheticDllRuntime::readUtf16(uint32_t address, size_t maxChars) const {
    std::string out;
    for (size_t i = 0; address && i < maxChars; ++i) {
        uint16_t ch = 0;
        if (uc_mem_read(uc_, address + uint32_t(i * 2), &ch, sizeof(ch)) != UC_ERR_OK) break;
        if (!ch) break;
        out.push_back(ch < 0x80 ? char(ch) : '?');
    }
    return out;
}

uint32_t SyntheticDllRuntime::writeUtf16(uint32_t address, const std::string& value, uint32_t maxChars) const {
    if (!address || !maxChars) return 0;
    const uint32_t charsToWrite = std::min<uint32_t>(uint32_t(value.size()), maxChars - 1);
    for (uint32_t i = 0; i < charsToWrite; ++i) {
        const uint16_t ch = uint16_t(static_cast<unsigned char>(value[i]));
        uc_mem_write(uc_, address + i * 2, &ch, sizeof(ch));
    }
    const uint16_t nul = 0;
    uc_mem_write(uc_, address + charsToWrite * 2, &nul, sizeof(nul));
    return charsToWrite;
}

std::filesystem::path SyntheticDllRuntime::resolveGuestPath(const std::string& guestPath) const {
    if (guestPath.empty()) return {};

    std::string normalized = guestPath;
    std::replace(normalized.begin(), normalized.end(), '/', '\\');
    if (normalized.size() > 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) &&
        normalized[1] == ':') {
        return std::filesystem::path(normalized);
    }

    while (!normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/')) {
        normalized.erase(normalized.begin());
    }

    std::filesystem::path relative(normalized);
    if (!hostBaseDir_.empty()) {
        auto first = relative.begin();
        if (first != relative.end() &&
            lowerAscii(first->string()) == lowerAscii(hostBaseDir_.filename().string())) {
            return hostBaseDir_.parent_path() / relative;
        }
        return hostBaseDir_ / relative;
    }
    return relative;
}

bool SyntheticDllRuntime::dispatchGuestMemoryApi(const std::string& name,
                                                 const GuestCallArgs& args,
                                                 uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;

    if (name == "memcpy" || name == "memmove") {
        copyGuest(a0, a1, a2);
        ret = a0;
    } else if (name == "memset") {
        fillGuest(a0, uint8_t(a1 & 0xffu), a2);
        ret = a0;
    } else if (name == "swprintf" || name == "wsprintfW" || name == "vswprintf") {
        std::vector<uint32_t> values;
        if (name == "vswprintf") {
            values.reserve(16);
            for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(a2 + i * 4));
        } else {
            values = {a2, a3};
            for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
        }
        size_t argIndex = 0;
        auto nextArg = [&]() -> uint32_t {
            return argIndex < values.size() ? values[argIndex++] : 0;
        };
        const std::string format = readUtf16(a1, 2048);
        std::string out;
        for (size_t i = 0; i < format.size(); ++i) {
            if (format[i] != '%' || i + 1 >= format.size()) {
                out.push_back(format[i]);
                continue;
            }
            if (format[i + 1] == '%') {
                out.push_back('%');
                ++i;
                continue;
            }
            ++i;
            bool zeroPad = false;
            if (format[i] == '0') {
                zeroPad = true;
                ++i;
            }
            int width = 0;
            while (i < format.size() && std::isdigit(static_cast<unsigned char>(format[i]))) {
                width = width * 10 + (format[i++] - '0');
            }
            if (i < format.size() && (format[i] == 'l' || format[i] == 'h')) ++i;
            if (i >= format.size()) break;
            const char spec = format[i];
            const uint32_t value = spec == 'c' || spec == 'C' || spec == 'd' ||
                                   spec == 'i' || spec == 'u' || spec == 'x' ||
                                   spec == 'X' || spec == 's' || spec == 'S'
                ? nextArg()
                : 0;
            char buffer[64]{};
            switch (spec) {
            case 's':
            case 'S':
                out += readUtf16(value, 2048);
                break;
            case 'c':
            case 'C':
                out.push_back(char(value & 0xffu));
                break;
            case 'd':
            case 'i':
                if (width) std::snprintf(buffer, sizeof(buffer), zeroPad ? "%0*d" : "%*d", width, int32_t(value));
                else std::snprintf(buffer, sizeof(buffer), "%d", int32_t(value));
                out += buffer;
                break;
            case 'u':
                if (width) std::snprintf(buffer, sizeof(buffer), zeroPad ? "%0*u" : "%*u", width, value);
                else std::snprintf(buffer, sizeof(buffer), "%u", value);
                out += buffer;
                break;
            case 'x':
            case 'X':
                if (width) std::snprintf(buffer, sizeof(buffer), zeroPad ? "%0*x" : "%*x", width, value);
                else std::snprintf(buffer, sizeof(buffer), "%x", value);
                out += buffer;
                break;
            default:
                out.push_back('%');
                out.push_back(spec);
                break;
            }
        }
        ret = writeUtf16(a0, out, uint32_t(out.size() + 1));
    } else if (name == "_wtol") {
        const std::string value = readUtf16(a0, 128);
        char* end = nullptr;
        ret = uint32_t(std::strtol(value.c_str(), &end, 10));
    } else if (name == "InitializeCriticalSection") {
        if (a0) {
            criticalSectionDepth_[a0] = 0;
            lastError_ = 0;
        } else {
            lastError_ = 87;
        }
        ret = 0;
    } else if (name == "DeleteCriticalSection") {
        criticalSectionDepth_.erase(a0);
        lastError_ = 0;
        ret = 0;
    } else if (name == "EnterCriticalSection") {
        if (a0) {
            ++criticalSectionDepth_[a0];
            lastError_ = 0;
        } else {
            lastError_ = 87;
        }
        ret = 0;
    } else if (name == "LeaveCriticalSection") {
        auto it = criticalSectionDepth_.find(a0);
        if (it != criticalSectionDepth_.end() && it->second) --it->second;
        lastError_ = 0;
        ret = 0;
    } else if (name == "TryEnterCriticalSection") {
        if (a0) {
            ++criticalSectionDepth_[a0];
            lastError_ = 0;
            ret = 1;
        } else {
            lastError_ = 87;
            ret = 0;
        }
    } else if (name == "TlsGetValue") {
        ret = tlsValues_[a0];
    } else if (name == "TlsSetValue") {
        tlsValues_[a0] = a1;
        ret = 1;
    } else if (name == "TlsCall") {
        ret = 0;
    } else if (name == "InterlockedIncrement" || name == "InterlockedDecrement") {
        int32_t value = 0;
        uc_mem_read(uc_, a0, &value, sizeof(value));
        value += name == "InterlockedIncrement" ? 1 : -1;
        uc_mem_write(uc_, a0, &value, sizeof(value));
        ret = uint32_t(value);
    } else if (name == "InterlockedExchange") {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        uc_mem_write(uc_, a0, &a1, sizeof(a1));
        ret = old;
    } else if (name == "InterlockedExchangeAdd") {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        const uint32_t next = old + a1;
        uc_mem_write(uc_, a0, &next, sizeof(next));
        ret = old;
    } else if (name == "InterlockedCompareExchange") {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        if (old == a2) uc_mem_write(uc_, a0, &a1, sizeof(a1));
        ret = old;
    } else if (name == "LocalAlloc") {
        ret = allocate(a1, (a0 & 0x0040u) != 0);
    } else if (name == "malloc") {
        ret = allocate(a0, false);
    } else if (name == "LocalAllocTrace") {
        if (a2 == 0x38 && a1) {
            uint32_t firstWord = 0;
            ret = uc_mem_read(uc_, a1, &firstWord, sizeof(firstWord)) == UC_ERR_OK ? firstWord : 0;
        } else {
            ret = allocate(a1 ? a1 : 1, false);
        }
    } else if (name == "LocalFree" || name == "free") {
        ret = 0;
    } else if (name == "LocalSize" || name == "HeapSize") {
        auto it = allocationSizes_.find(name == "HeapSize" ? a2 : a0);
        ret = it == allocationSizes_.end() ? 0 : it->second;
    } else if (name == "LocalReAlloc" || name == "RemoteLocalReAlloc" || name == "realloc") {
        const uint32_t oldSize = allocationSizes_.count(a0) ? allocationSizes_[a0] : 0;
        ret = allocate(a1, name == "realloc" ? false : (a2 & 0x0040u) != 0);
        if (a0 && ret && oldSize) {
            std::vector<uint8_t> bytes(std::min(oldSize, a1));
            uc_mem_read(uc_, a0, bytes.data(), bytes.size());
            uc_mem_write(uc_, ret, bytes.data(), bytes.size());
        }
    } else if (name == "HeapCreate") {
        ret = makeGuestHandle({GuestHandle::Kind::GuestHeap, 0, 0});
    } else if (name == "HeapDestroy") {
        auto it = guestHandles_.find(a0);
        if (it == guestHandles_.end() || it->second.kind != GuestHandle::Kind::GuestHeap ||
            a0 == processHeapHandle_) {
            lastError_ = 6;
            ret = 0;
        } else {
            guestHandles_.erase(it);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "GetProcessHeap") {
        if (!processHeapHandle_) {
            processHeapHandle_ = makeGuestHandle({GuestHandle::Kind::GuestHeap, 0, 0});
        }
        ret = processHeapHandle_;
    } else if (name == "HeapAlloc") {
        auto* heap = lookupGuestHandle(a0);
        if (!heap || heap->kind != GuestHandle::Kind::GuestHeap) {
            lastError_ = 6;
            ret = 0;
        } else {
            ret = allocate(a2, (a1 & 0x00000008u) != 0);
            lastError_ = ret ? 0 : 8;
        }
    } else if (name == "HeapReAlloc") {
        auto* heap = lookupGuestHandle(a0);
        if (!heap || heap->kind != GuestHandle::Kind::GuestHeap || !a2) {
            lastError_ = 6;
            ret = 0;
        } else {
            const uint32_t oldSize = allocationSizes_.count(a2) ? allocationSizes_[a2] : 0;
            ret = allocate(a3, (a1 & 0x00000008u) != 0);
            if (ret && oldSize) {
                std::vector<uint8_t> bytes(std::min(oldSize, a3));
                uc_mem_read(uc_, a2, bytes.data(), bytes.size());
                uc_mem_write(uc_, ret, bytes.data(), bytes.size());
            }
            lastError_ = ret ? 0 : 8;
        }
    } else if (name == "HeapFree") {
        auto* heap = lookupGuestHandle(a0);
        if (!heap || heap->kind != GuestHandle::Kind::GuestHeap || !a2) {
            lastError_ = 6;
            ret = 0;
        } else {
            allocationSizes_.erase(a2);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "RemoteHeapFree") {
        ret = copyGuest(a0, a2, 0x38) ? a0 : 1;
    } else if (name == "VirtualAlloc") {
        ret = allocate(a1, true);
    } else if (name == "VirtualFree") {
        ret = 1;
    } else if (name == "GetVersionExW") {
        if (a0) {
            writeU32(a0 + 4, 4);
            writeU32(a0 + 8, 20);
            writeU32(a0 + 12, 0);
            writeU32(a0 + 16, 3);
        }
        ret = a0 ? 1 : 0;
    } else if (name == "operator_new" || name == "operator_new_nothrow" ||
               name == "operator_vector_new" || name == "operator_vector_new_nothrow") {
        ret = allocate(a0, false);
    } else if (name == "operator_delete" || name == "operator_delete_nothrow" ||
               name == "operator_vector_delete" || name == "operator_vector_delete_nothrow") {
        ret = 0;
    } else if (name == "wcschr" || name == "wcsrchr") {
        const uint16_t target = uint16_t(a1 & 0xffffu);
        uint32_t found = 0;
        for (uint32_t offset = 0; a0; offset += 2) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, a0 + offset, &ch, sizeof(ch)) != UC_ERR_OK) break;
            if (ch == target) {
                found = a0 + offset;
                if (name == "wcschr") break;
            }
            if (!ch) break;
        }
        ret = found;
    } else if (name == "wcslen") {
        uint32_t count = 0;
        for (;; ++count) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, a0 + count * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
        }
        ret = count;
    } else if (name == "wcscpy") {
        uint32_t offset = 0;
        for (;; offset += 2) {
            uint16_t ch = 0;
            uc_mem_read(uc_, a1 + offset, &ch, sizeof(ch));
            uc_mem_write(uc_, a0 + offset, &ch, sizeof(ch));
            if (!ch) break;
        }
        ret = a0;
    } else if (name == "wcscspn") {
        uint32_t count = 0;
        for (;; ++count) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, a0 + count * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
            bool rejected = false;
            for (uint32_t offset = 0; a1; offset += 2) {
                uint16_t reject = 0;
                if (uc_mem_read(uc_, a1 + offset, &reject, sizeof(reject)) != UC_ERR_OK || !reject) break;
                if (ch == reject) {
                    rejected = true;
                    break;
                }
            }
            if (rejected) break;
        }
        ret = count;
    } else if (name == "wcsncmp" || name == "_wcsnicmp") {
        ret = 0;
        for (uint32_t i = 0; i < a2; ++i) {
            uint16_t left = 0;
            uint16_t right = 0;
            uc_mem_read(uc_, a0 + i * 2, &left, sizeof(left));
            uc_mem_read(uc_, a1 + i * 2, &right, sizeof(right));
            if (name == "_wcsnicmp") {
                if (left >= 'A' && left <= 'Z') left = uint16_t(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z') right = uint16_t(right - 'A' + 'a');
            }
            if (left != right || !left || !right) {
                ret = uint32_t(int(left) - int(right));
                break;
            }
        }
    } else if (name == "_wcsdup") {
        const std::string value = readUtf16(a0);
        ret = allocate(uint32_t((value.size() + 1) * 2), false);
        writeUtf16(ret, value, uint32_t(value.size() + 1));
    } else if (name == "strlen") {
        ret = uint32_t(readAscii(a0).size());
    } else if (name == "strcpy") {
        uint32_t offset = 0;
        for (;; ++offset) {
            char ch = 0;
            uc_mem_read(uc_, a1 + offset, &ch, sizeof(ch));
            uc_mem_write(uc_, a0 + offset, &ch, sizeof(ch));
            if (!ch) break;
        }
        ret = a0;
    } else if (name == "strcmp") {
        uint32_t offset = 0;
        for (;; ++offset) {
            unsigned char left = 0;
            unsigned char right = 0;
            uc_mem_read(uc_, a0 + offset, &left, sizeof(left));
            uc_mem_read(uc_, a1 + offset, &right, sizeof(right));
            if (left != right || !left || !right) {
                ret = uint32_t(int(left) - int(right));
                break;
            }
        }
    } else if (name == "strcspn") {
        const std::string source = readAscii(a0);
        const std::string reject = readAscii(a1);
        size_t count = 0;
        while (count < source.size() && reject.find(source[count]) == std::string::npos) ++count;
        ret = uint32_t(count);
    } else if (name == "_stricmp" || name == "_strnicmp") {
        std::string left = lowerAscii(readAscii(a0));
        std::string right = lowerAscii(readAscii(a1));
        if (name == "_strnicmp") {
            left = left.substr(0, a2);
            right = right.substr(0, a2);
        }
        ret = uint32_t(left.compare(right));
    } else if (name == "strtol") {
        const std::string source = readAscii(a0);
        char* end = nullptr;
        const long value = std::strtol(source.c_str(), &end, int(a2 ? a2 : 10));
        if (a1) writeU32(a1, a0 + uint32_t(end ? end - source.c_str() : 0));
        ret = uint32_t(value);
    } else if (name == "CopyRect") {
        ret = copyGuest(a0, a1, 16) ? 1 : 0;
    } else if (name == "InflateRect") {
        int32_t rect[4]{};
        if (a0 && uc_mem_read(uc_, a0, rect, sizeof(rect)) == UC_ERR_OK) {
            rect[0] -= int32_t(a1);
            rect[1] -= int32_t(a2);
            rect[2] += int32_t(a1);
            rect[3] += int32_t(a2);
            uc_mem_write(uc_, a0, rect, sizeof(rect));
            ret = 1;
        } else {
            ret = 0;
        }
    } else if (name == "IsRectEmpty") {
        int32_t rect[4]{};
        ret = a0 && uc_mem_read(uc_, a0, rect, sizeof(rect)) == UC_ERR_OK &&
              (rect[2] <= rect[0] || rect[3] <= rect[1]) ? 1 : 0;
    } else if (name == "SetRectEmpty") {
        const int32_t rect[4]{};
        ret = a0 && uc_mem_write(uc_, a0, rect, sizeof(rect)) == UC_ERR_OK ? 1 : 0;
    } else if (name == "PtInRect") {
        int32_t rect[4]{};
        ret = a0 && uc_mem_read(uc_, a0, rect, sizeof(rect)) == UC_ERR_OK &&
              int32_t(a1) >= rect[0] && int32_t(a1) < rect[2] &&
              int32_t(a2) >= rect[1] && int32_t(a2) < rect[3] ? 1 : 0;
    } else if (name == "GetAPIAddress") {
        if (!a0 && !a1 && !a2 && a3 == 0x1c) {
            ret = allocate(0x38, true);
        } else {
            ret = 0;
        }
    } else if (name == "InterlockedTestExchange") {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        if (old == a1) uc_mem_write(uc_, a0, &a2, sizeof(a2));
        ret = old;
    } else if (name == "IsDBCSLeadByteEx" || name == "iswctype") {
        ret = 0;
    } else if (name == "MultiByteToWideChar") {
        const uint32_t wideOut = stackArg(4);
        const uint32_t wideCapacity = stackArg(5);
        const int32_t byteCount = int32_t(a3);
        if (!a2) {
            lastError_ = 87;
            ret = 0;
        } else {
            std::vector<char> bytes;
            if (byteCount < 0) {
                for (uint32_t i = 0; i < 1024 * 1024; ++i) {
                    char ch = 0;
                    if (uc_mem_read(uc_, a2 + i, &ch, sizeof(ch)) != UC_ERR_OK) break;
                    bytes.push_back(ch);
                    if (!ch) break;
                }
            } else {
                bytes.resize(uint32_t(byteCount));
                if (!bytes.empty() && uc_mem_read(uc_, a2, bytes.data(), bytes.size()) != UC_ERR_OK) bytes.clear();
            }
#if defined(_WIN32)
            const int inputChars = byteCount < 0 ? -1 : int(bytes.size());
            const int needed = bytes.empty()
                ? 0
                : ::MultiByteToWideChar(a0, a1, bytes.data(), inputChars, nullptr, 0);
            if (!needed) {
                lastError_ = GetLastError();
                ret = 0;
            } else if (!wideOut || !wideCapacity) {
                lastError_ = 0;
                ret = uint32_t(needed);
            } else {
                std::vector<wchar_t> wide(static_cast<size_t>(wideCapacity));
                wchar_t* wideData = wide.data();
                const int wideLength = int(wide.size());
                const int written = ::MultiByteToWideChar(a0, a1, bytes.data(), inputChars, wideData, wideLength);
                if (!written) {
                    lastError_ = GetLastError();
                    ret = 0;
                } else {
                    uc_mem_write(uc_, wideOut, wideData, size_t(written) * sizeof(wchar_t));
                    lastError_ = 0;
                    ret = uint32_t(written);
                }
            }
#else
            const uint32_t needed = uint32_t(bytes.size());
            if (wideOut && wideCapacity) {
                const uint32_t count = std::min(needed, wideCapacity);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint16_t ch = uint8_t(bytes[i]);
                    uc_mem_write(uc_, wideOut + i * 2, &ch, sizeof(ch));
                }
            }
            ret = wideOut && wideCapacity ? std::min(needed, wideCapacity) : needed;
#endif
        }
    } else if (name == "__ll_div") {
        const int64_t dividend = (int64_t(uint64_t(a1)) << 32) | uint64_t(a0);
        const int64_t divisor = (int64_t(uint64_t(a3)) << 32) | uint64_t(a2);
        const int64_t quotient = divisor ? (dividend / divisor) : 0;
        ret = uint32_t(uint64_t(quotient));
        setReg(UC_MIPS_REG_V1, uint32_t(uint64_t(quotient) >> 32));
    } else if (name == "GetCRTFlags") {
        ret = 0;
    } else if (name == "GetCRTStorageEx") {
        ret = a1 && a2 == 0x38 ? 0 : allocate(0x100, true);
    } else if (name == "_setjmp") {
        ret = 0;
    } else if (name == "longjmp") {
        ret = a1 ? a1 : 1;
    } else {
        return false;
    }

    return true;
}

bool SyntheticDllRuntime::dispatchHostWin32(const std::string& name,
                                            const GuestCallArgs& args,
                                            uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    auto getWindowLongValue = [](const GuestWindow& window, int32_t index) -> uint32_t {
        switch (index) {
        case -4: return window.wndProc;  // GWL_WNDPROC
        case -6: return window.instance; // GWL_HINSTANCE
        case -8: return window.parent;   // GWL_HWNDPARENT
        case -12: return window.menu;    // GWL_ID
        case -16: return window.style;   // GWL_STYLE
        case -20: return window.exStyle; // GWL_EXSTYLE
        case -21: return window.userData; // GWL_USERDATA
        default: {
            auto it = window.extraLongs.find(index);
            return it == window.extraLongs.end() ? 0 : it->second;
        }
        }
    };
    auto setWindowLongValue = [&](GuestWindow& window, int32_t index, uint32_t value) -> uint32_t {
        const uint32_t previous = getWindowLongValue(window, index);
        switch (index) {
        case -4: window.wndProc = value; break;
        case -6: window.instance = value; break;
        case -8: window.parent = value; break;
        case -12: window.menu = value; break;
        case -16: window.style = value; break;
        case -20: window.exStyle = value; break;
        case -21: window.userData = value; break;
        default: window.extraLongs[index] = value; break;
        }
        return previous;
    };

    if (name == "IsProcessDying") {
        ret = 0;
    } else if (name == "GetLastError") {
        ret = lastError_;
    } else if (name == "SetLastError") {
        lastError_ = a0;
        ret = 0;
    } else if (name == "GetTickCount") {
#if defined(_WIN32)
        ret = GetTickCount();
#else
        ret = uint32_t(++tick_ * 16);
#endif
    } else if (name == "GlobalMemoryStatus") {
        if (!a0) {
            lastError_ = 87;
            ret = 0;
        } else {
#if defined(_WIN32)
            MEMORYSTATUS status{};
            status.dwLength = sizeof(status);
            GlobalMemoryStatus(&status);
            writeU32(a0, 32);
            writeU32(a0 + 4, status.dwMemoryLoad);
            writeU32(a0 + 8, uint32_t(status.dwTotalPhys));
            writeU32(a0 + 12, uint32_t(status.dwAvailPhys));
            writeU32(a0 + 16, uint32_t(status.dwTotalPageFile));
            writeU32(a0 + 20, uint32_t(status.dwAvailPageFile));
            writeU32(a0 + 24, uint32_t(status.dwTotalVirtual));
            writeU32(a0 + 28, uint32_t(status.dwAvailVirtual));
#else
            writeU32(a0, 32);
            writeU32(a0 + 4, 50);
            writeU32(a0 + 8, 64u * 1024u * 1024u);
            writeU32(a0 + 12, 32u * 1024u * 1024u);
            writeU32(a0 + 16, 64u * 1024u * 1024u);
            writeU32(a0 + 20, 32u * 1024u * 1024u);
            writeU32(a0 + 24, heapLimit_ - heapBase_);
            writeU32(a0 + 28, heapLimit_ - nextHeap_);
#endif
            ret = 0;
            lastError_ = 0;
        }
    } else if (name == "Sleep") {
#if defined(_WIN32)
        Sleep(a0);
#endif
        ret = 0;
    } else if (name == "QueryPerformanceFrequency") {
#if defined(_WIN32)
        LARGE_INTEGER value{};
        const BOOL ok = QueryPerformanceFrequency(&value);
        writeU32(a0, uint32_t(value.QuadPart));
        writeU32(a0 + 4, uint32_t(uint64_t(value.QuadPart) >> 32));
        ret = ok ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
#else
        writeU32(a0, 10000000u);
        writeU32(a0 + 4, 0);
        ret = 1;
#endif
    } else if (name == "QueryPerformanceCounter") {
#if defined(_WIN32)
        LARGE_INTEGER value{};
        const BOOL ok = QueryPerformanceCounter(&value);
        writeU32(a0, uint32_t(value.QuadPart));
        writeU32(a0 + 4, uint32_t(uint64_t(value.QuadPart) >> 32));
        ret = ok ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
#else
        const uint64_t value = ++tick_ * 160000;
        writeU32(a0, uint32_t(value));
        writeU32(a0 + 4, uint32_t(value >> 32));
        ret = 1;
#endif
    } else if (name == "GetTimeZoneInformation") {
#if defined(_WIN32)
        TIME_ZONE_INFORMATION tz{};
        ret = ::GetTimeZoneInformation(&tz);
        if (a0) uc_mem_write(uc_, a0, &tz, sizeof(tz));
#else
        if (a0) fillGuest(a0, 0, 172);
        ret = 0xffffffffu;
#endif
    } else if (name == "SystemTimeToFileTime") {
#if defined(_WIN32)
        SYSTEMTIME st{};
        FILETIME ft{};
        if (a0) uc_mem_read(uc_, a0, &st, sizeof(st));
        const BOOL ok = a0 && a1 && ::SystemTimeToFileTime(&st, &ft);
        if (ok) uc_mem_write(uc_, a1, &ft, sizeof(ft));
        ret = ok ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
#else
        if (a1) {
            writeU32(a1, 0);
            writeU32(a1 + 4, 0);
        }
        ret = a0 && a1 ? 1 : 0;
#endif
    } else if (name == "GetLocalTime" || name == "GetSystemTime") {
#if defined(_WIN32)
        SYSTEMTIME st{};
        if (name == "GetSystemTime") ::GetSystemTime(&st);
        else ::GetLocalTime(&st);
        if (a0) uc_mem_write(uc_, a0, &st, sizeof(st));
        ret = 0;
#else
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        if (name == "GetSystemTime") tm = *std::gmtime(&now);
        else tm = *std::localtime(&now);
        const std::array<uint16_t, 8> st = {
            uint16_t(tm.tm_year + 1900), uint16_t(tm.tm_mon + 1),
            uint16_t(tm.tm_wday), uint16_t(tm.tm_mday), uint16_t(tm.tm_hour),
            uint16_t(tm.tm_min), uint16_t(tm.tm_sec), 0,
        };
        if (a0) uc_mem_write(uc_, a0, st.data(), st.size() * sizeof(uint16_t));
        ret = 0;
#endif
    } else if (name == "CloseHandle") {
        ret = closeGuestHandle(a0);
    } else if (name == "FindClose") {
        ret = closeGuestHandle(a0);
    } else if (name == "ReleaseMutex") {
        auto* handle = lookupGuestHandle(a0);
#if defined(_WIN32)
        if (handle && handle->kind == GuestHandle::Kind::HostMutex && handle->hostValue) {
            ret = ReleaseMutex(reinterpret_cast<HANDLE>(handle->hostValue)) ? 1 : 0;
            if (!ret) lastError_ = GetLastError();
        } else
#endif
        {
            ret = handle ? 1 : 0;
            if (!ret) lastError_ = 6;
        }
    } else if (name == "CreateEventW") {
#if defined(_WIN32)
        HANDLE host = CreateEventW(nullptr, a1 != 0, a2 != 0, nullptr);
        if (host) ret = makeGuestHandle({GuestHandle::Kind::HostEvent, reinterpret_cast<uintptr_t>(host), 0});
        else {
            ret = 0;
            lastError_ = GetLastError();
        }
#else
        ret = makeGuestHandle({GuestHandle::Kind::HostEvent, 0, 0});
#endif
    } else if (name == "CreateMutexW") {
#if defined(_WIN32)
        HANDLE host = CreateMutexW(nullptr, a1 != 0, nullptr);
        if (host) ret = makeGuestHandle({GuestHandle::Kind::HostMutex, reinterpret_cast<uintptr_t>(host), 0});
        else {
            ret = 0;
            lastError_ = GetLastError();
        }
#else
        ret = makeGuestHandle({GuestHandle::Kind::HostMutex, 0, 0});
#endif
    } else if (name == "WaitForSingleObject") {
        auto* handle = lookupGuestHandle(a0);
#if defined(_WIN32)
        if (handle && handle->hostValue) {
            ret = WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), a1);
            if (ret == 0xffffffffu) lastError_ = GetLastError();
        } else
#endif
        {
            ret = handle ? 0 : 0xffffffffu;
            if (!handle) lastError_ = 6;
        }
    } else if (name == "EventModify") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostEvent) {
            lastError_ = 6;
            ret = 0;
        } else {
#if defined(_WIN32)
            HANDLE host = reinterpret_cast<HANDLE>(handle->hostValue);
            BOOL ok = FALSE;
            if (host) {
                if (a1 == 1) ok = PulseEvent(host);
                else if (a1 == 2) ok = ResetEvent(host);
                else if (a1 == 3) ok = SetEvent(host);
            }
            ret = ok ? 1 : 0;
            if (!ret) lastError_ = GetLastError();
#else
            ret = 1;
#endif
        }
    } else if (name == "CreateFileW") {
#if defined(_WIN32)
        const std::filesystem::path hostPath = resolveGuestPath(readUtf16(a0));
        HANDLE host = INVALID_HANDLE_VALUE;
        if (!hostPath.empty()) {
            host = CreateFileW(hostPath.wstring().c_str(), a1, a2, nullptr, stackArg(4), stackArg(5), nullptr);
        }
        if (host == INVALID_HANDLE_VALUE) {
            lastError_ = GetLastError();
            ret = 0xffffffffu;
        } else {
            ret = makeGuestHandle({GuestHandle::Kind::HostFile, reinterpret_cast<uintptr_t>(host), 0});
            lastError_ = 0;
        }
#else
        lastError_ = 2;
        ret = 0xffffffffu;
#endif
    } else if (name == "DeleteFileW") {
#if defined(_WIN32)
        const std::filesystem::path hostPath = resolveGuestPath(readUtf16(a0));
        const BOOL ok = !hostPath.empty() && DeleteFileW(hostPath.wstring().c_str());
        ret = ok ? 1 : 0;
        lastError_ = ret ? 0 : GetLastError();
#else
        lastError_ = 2;
        ret = 0;
#endif
    } else if (name == "FindFirstFileW") {
#if defined(_WIN32)
        const std::filesystem::path hostPath = resolveGuestPath(readUtf16(a0));
        WIN32_FIND_DATAW data{};
        HANDLE host = INVALID_HANDLE_VALUE;
        if (!hostPath.empty()) {
            host = FindFirstFileW(hostPath.wstring().c_str(), &data);
        }
        if (host == INVALID_HANDLE_VALUE) {
            lastError_ = GetLastError();
            ret = 0xffffffffu;
        } else {
            if (a1) {
                writeU32(a1, data.dwFileAttributes);
                uc_mem_write(uc_, a1 + 4, &data.ftCreationTime, sizeof(data.ftCreationTime));
                uc_mem_write(uc_, a1 + 12, &data.ftLastAccessTime, sizeof(data.ftLastAccessTime));
                uc_mem_write(uc_, a1 + 20, &data.ftLastWriteTime, sizeof(data.ftLastWriteTime));
                writeU32(a1 + 28, data.nFileSizeHigh);
                writeU32(a1 + 32, data.nFileSizeLow);
                writeU32(a1 + 36, 0);
                for (uint32_t i = 0; i < 260; ++i) {
                    const uint16_t ch = uint16_t(data.cFileName[i]);
                    uc_mem_write(uc_, a1 + 40 + i * 2, &ch, sizeof(ch));
                    if (!ch) break;
                }
            }
            ret = makeGuestHandle({GuestHandle::Kind::HostFind, reinterpret_cast<uintptr_t>(host), 0});
            lastError_ = 0;
        }
#else
        lastError_ = 2;
        ret = 0xffffffffu;
#endif
    } else if (name == "ReadFile" || name == "WriteFile") {
        auto* handle = lookupGuestHandle(a0);
        writeU32(a3, 0);
        if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
            lastError_ = 6;
            ret = 0;
        } else if (!a1 && a2) {
            lastError_ = 87;
            ret = 0;
        } else {
#if defined(_WIN32)
            DWORD transferred = 0;
            if (name == "ReadFile") {
                std::vector<uint8_t> bytes(a2);
                const BOOL ok = ReadFile(reinterpret_cast<HANDLE>(handle->hostValue), bytes.data(), a2, &transferred, nullptr);
                if (ok && transferred) uc_mem_write(uc_, a1, bytes.data(), transferred);
                ret = ok ? 1 : 0;
            } else {
                std::vector<uint8_t> bytes(a2);
                if (a2) uc_mem_read(uc_, a1, bytes.data(), bytes.size());
                const BOOL ok = WriteFile(reinterpret_cast<HANDLE>(handle->hostValue), bytes.data(), a2, &transferred, nullptr);
                ret = ok ? 1 : 0;
            }
            writeU32(a3, transferred);
            lastError_ = ret ? 0 : GetLastError();
#else
            lastError_ = 6;
            ret = 0;
#endif
        }
    } else if (name == "GetFileSize") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else {
#if defined(_WIN32)
            DWORD high = 0;
            ret = GetFileSize(reinterpret_cast<HANDLE>(handle->hostValue), a1 ? &high : nullptr);
            if (a1) writeU32(a1, high);
            lastError_ = ret == 0xffffffffu ? GetLastError() : 0;
#else
            lastError_ = 6;
            ret = 0xffffffffu;
#endif
        }
    } else if (name == "SetFilePointer") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else {
#if defined(_WIN32)
            LONG high = 0;
            if (a2) uc_mem_read(uc_, a2, &high, sizeof(high));
            ret = SetFilePointer(reinterpret_cast<HANDLE>(handle->hostValue), LONG(a1), a2 ? &high : nullptr, a3);
            if (a2) writeU32(a2, uint32_t(high));
            lastError_ = ret == 0xffffffffu ? GetLastError() : 0;
#else
            lastError_ = 6;
            ret = 0xffffffffu;
#endif
        }
    } else if (name == "SetFileTime") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
            lastError_ = 6;
            ret = 0;
        } else {
#if defined(_WIN32)
            FILETIME creation{}, access{}, write{};
            FILETIME* creationPtr = nullptr;
            FILETIME* accessPtr = nullptr;
            FILETIME* writePtr = nullptr;
            if (a1 && uc_mem_read(uc_, a1, &creation, sizeof(creation)) == UC_ERR_OK) creationPtr = &creation;
            if (a2 && uc_mem_read(uc_, a2, &access, sizeof(access)) == UC_ERR_OK) accessPtr = &access;
            if (a3 && uc_mem_read(uc_, a3, &write, sizeof(write)) == UC_ERR_OK) writePtr = &write;
            ret = SetFileTime(reinterpret_cast<HANDLE>(handle->hostValue), creationPtr, accessPtr, writePtr) ? 1 : 0;
            lastError_ = ret ? 0 : GetLastError();
#else
            ret = 0;
            lastError_ = 6;
#endif
        }
    } else if (name == "GetModuleFileNameW") {
        ret = writeUtf16(a1, mainModulePath_, a2);
        lastError_ = ret ? 0 : 122;
    } else if (name == "OutputDebugStringW") {
        spdlog::info("OutputDebugStringW: {}", readUtf16(a0));
        ret = 0;
    } else if (name == "LoadLibraryW" || name == "GetModuleHandleW") {
        const std::string dll = lowerAscii(readUtf16(a0));
        ret = dll.empty() || dll == "coredll.dll" ? 0x70000000 : 0;
        lastError_ = ret ? 0 : 126;
    } else if (name == "GetProcAddressA" || name == "GetProcAddressW") {
        lastError_ = 127;
        ret = 0;
    } else if (name == "GetCursorPos") {
        if (!a0) {
            lastError_ = 87;
            ret = 0;
        } else {
#if defined(_WIN32)
            POINT point{};
            ret = GetCursorPos(&point) ? 1 : 0;
            writeU32(a0, uint32_t(point.x));
            writeU32(a0 + 4, uint32_t(point.y));
            if (!ret) lastError_ = GetLastError();
#else
            writeU32(a0, 0);
            writeU32(a0 + 4, 0);
            ret = 1;
#endif
        }
    } else if (name == "GetSystemMetrics") {
#if defined(_WIN32)
        if (a0 == 0 && framebufferWidth_ > 0) ret = uint32_t(framebufferWidth_);
        else if (a0 == 1 && framebufferHeight_ > 0) ret = uint32_t(framebufferHeight_);
        else ret = uint32_t(GetSystemMetrics(int(a0)));
#else
        if (a0 == 0) ret = uint32_t(framebufferWidth_ > 0 ? framebufferWidth_ : 800);
        else if (a0 == 1) ret = uint32_t(framebufferHeight_ > 0 ? framebufferHeight_ : 480);
        else ret = 0;
#endif
    } else if (name == "GetSysColor") {
#if defined(_WIN32)
        ret = uint32_t(GetSysColor(int(a0)));
#else
        ret = 0x00ffffffu;
#endif
    } else if (name == "RegisterWindowMessageW") {
#if defined(_WIN32)
        const std::string value = readUtf16(a0);
        std::wstring wide(value.begin(), value.end());
        ret = RegisterWindowMessageW(wide.c_str());
        if (!ret) lastError_ = GetLastError();
#else
        ret = 0xc000u;
#endif
    } else if (name == "RegisterClassW") {
        std::array<uint8_t, 40> bytes{};
        if (!a0 || uc_mem_read(uc_, a0, bytes.data(), bytes.size()) != UC_ERR_OK) {
            lastError_ = 87;
            ret = 0;
        } else {
            uint32_t classNamePtr = 0;
            std::memcpy(&classNamePtr, bytes.data() + 36, sizeof(classNamePtr));
            std::string className = classNamePtr < 0x10000
                ? ("#" + std::to_string(classNamePtr))
                : lowerAscii(readUtf16(classNamePtr));
            if (className.empty()) className = "#anonymous";
            auto existing = windowClassesByName_.find(className);
            if (existing == windowClassesByName_.end()) {
                GuestWindowClass wndClass{};
                wndClass.bytes = bytes;
                wndClass.name = className;
                wndClass.atom = nextAtom_++;
                existing = windowClassesByName_.emplace(className, wndClass).first;
                windowClassNamesByAtom_[existing->second.atom] = className;
            } else {
                existing->second.bytes = bytes;
            }
            ret = existing->second.atom;
            lastError_ = 0;
        }
    } else if (name == "GetClassInfoW") {
        std::string className;
        if (a1 < 0x10000) {
            auto it = windowClassNamesByAtom_.find(uint16_t(a1));
            if (it != windowClassNamesByAtom_.end()) className = it->second;
        } else {
            className = lowerAscii(readUtf16(a1));
        }
        auto it = windowClassesByName_.find(className);
        if (it != windowClassesByName_.end() && a2) {
            uc_mem_write(uc_, a2, it->second.bytes.data(), it->second.bytes.size());
            ret = 1;
            lastError_ = 0;
        } else {
            lastError_ = 1411;
            ret = 0;
        }
    } else if (name == "FindWindowW") {
        ret = 0;
        const std::string className = a0 < 0x10000
            ? (windowClassNamesByAtom_.count(uint16_t(a0)) ? windowClassNamesByAtom_[uint16_t(a0)] : std::string{})
            : lowerAscii(readUtf16(a0));
        const std::string title = readUtf16(a1);
        for (const auto& [hwnd, window] : windows_) {
            if ((!a0 || window.className == className) && (!a1 || window.title == title)) {
                ret = hwnd;
                break;
            }
        }
        lastError_ = ret ? 0 : 1400;
    } else if (name == "CreateWindowExW") {
        std::string className;
        if (a1 < 0x10000) {
            auto it = windowClassNamesByAtom_.find(uint16_t(a1));
            if (it != windowClassNamesByAtom_.end()) className = it->second;
        } else {
            className = lowerAscii(readUtf16(a1));
        }
        const uint32_t parent = stackArg(8);
        const uint32_t menu = stackArg(9);
        const uint32_t instance = stackArg(10);
        const uint32_t param = stackArg(11);
        auto normalizePos = [](uint32_t value) -> int32_t {
            return value == 0x80000000u ? 0 : int32_t(value);
        };
        auto normalizeSize = [](uint32_t value, int32_t fallback) -> int32_t {
            const int32_t signedValue = int32_t(value);
            return value == 0x80000000u || signedValue <= 0 ? fallback : signedValue;
        };
        uint32_t wndProc = 0;
        auto cls = windowClassesByName_.find(className);
        if (cls != windowClassesByName_.end()) {
            std::memcpy(&wndProc, cls->second.bytes.data() + 4, sizeof(wndProc));
        }
        ret = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
        GuestWindow window{};
        window.hwnd = ret;
        window.className = className;
        window.title = readUtf16(a2);
        window.style = a3;
        window.exStyle = a0;
        window.parent = parent;
        window.menu = menu;
        window.instance = instance;
        window.param = param;
        window.wndProc = wndProc;
        window.x = normalizePos(stackArg(4));
        window.y = normalizePos(stackArg(5));
        window.width = normalizeSize(stackArg(6), framebufferWidth_ > 0 ? framebufferWidth_ : 800);
        window.height = normalizeSize(stackArg(7), framebufferHeight_ > 0 ? framebufferHeight_ : 480);
        window.visible = (a3 & 0x10000000u) != 0; // WS_VISIBLE
        windows_[ret] = window;
        const uint32_t createStruct = allocate(48, true);
        if (createStruct) {
            writeU32(createStruct, param);
            writeU32(createStruct + 4, instance);
            writeU32(createStruct + 8, menu);
            writeU32(createStruct + 12, parent);
            writeU32(createStruct + 16, uint32_t(window.height));
            writeU32(createStruct + 20, uint32_t(window.width));
            writeU32(createStruct + 24, uint32_t(window.y));
            writeU32(createStruct + 28, uint32_t(window.x));
            writeU32(createStruct + 32, window.style);
            writeU32(createStruct + 36, a2);
            writeU32(createStruct + 40, a1);
            writeU32(createStruct + 44, window.exStyle);
            GuestMessage create{};
            create.hwnd = ret;
            create.message = 0x0001; // WM_CREATE
            create.lParam = createStruct;
            create.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(create);
        }
        GuestMessage size{};
        size.hwnd = ret;
        size.message = 0x0005; // WM_SIZE
        size.lParam = (uint32_t(uint16_t(window.height)) << 16) | uint16_t(window.width);
        size.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(size);
        lastError_ = 0;
    } else if (name == "GetClientRect") {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            writeGuestRect(a1, 0, 0, it->second.width, it->second.height);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "GetDlgItem") {
        auto parent = windows_.find(a0);
        if (parent == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            ret = 0;
            for (const auto& [hwnd, window] : windows_) {
                if (window.parent == a0 && window.menu == a1) {
                    ret = hwnd;
                    break;
                }
            }
            lastError_ = ret ? 0 : 1421;
        }
    } else if (name == "InvalidateRect") {
        if (!windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            GuestMessage message{};
            message.hwnd = a0;
            message.message = 0x000f; // WM_PAINT
            message.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(message);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "ValidateRect") {
        ret = windows_.count(a0) ? 1 : 0;
        lastError_ = ret ? 0 : 1400;
    } else if (name == "KillTimer") {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "BeginPaint") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            ret = makeGuestDc(a0);
            if (a1) {
                std::array<uint8_t, 64> paint{};
                uc_mem_write(uc_, a1, paint.data(), paint.size());
                writeU32(a1, ret);
                writeU32(a1 + 4, 1);
                writeGuestRect(a1 + 8, 0, 0, it->second.width, it->second.height);
            }
            lastError_ = 0;
        }
    } else if (name == "EndPaint") {
        uint32_t hdc = 0;
        if (a1) uc_mem_read(uc_, a1, &hdc, sizeof(hdc));
        if (hdc) {
            dcs_.erase(hdc);
            guestHandles_.erase(hdc);
        }
        ret = windows_.count(a0) ? 1 : 0;
        lastError_ = ret ? 0 : 1400;
    } else if (name == "GetDC" || name == "GetDCEx") {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            ret = makeGuestDc(a0);
            lastError_ = 0;
        }
    } else if (name == "ReleaseDC") {
        auto handle = guestHandles_.find(a1);
        if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestDc) {
            lastError_ = 6;
            ret = 0;
        } else {
            dcs_.erase(a1);
            guestHandles_.erase(handle);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "CreateSolidBrush") {
        ret = makeGuestBrush(a0);
        lastError_ = 0;
    } else if (name == "CreateFontIndirectW") {
        std::array<uint8_t, 92> font{};
        if (!a0 || uc_mem_read(uc_, a0, font.data(), font.size()) != UC_ERR_OK) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestFont(font);
            lastError_ = 0;
        }
    } else if (name == "GetStockObject") {
        ret = makeStockObject(int32_t(a0));
        lastError_ = ret ? 0 : 87;
    } else if (name == "SelectObject") {
        GuestDc* dc = lookupGuestDc(a0);
        auto object = guestHandles_.find(a1);
        if (!dc || object == guestHandles_.end()) {
            lastError_ = 6;
            ret = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestBrush) {
            ret = dc->selectedBrush;
            dc->selectedBrush = a1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestFont) {
            ret = dc->selectedFont;
            dc->selectedFont = a1;
            lastError_ = 0;
        } else {
            lastError_ = 6;
            ret = 0;
        }
    } else if (name == "DeleteObject") {
        auto object = guestHandles_.find(a0);
        if (object == guestHandles_.end()) {
            lastError_ = 6;
            ret = 0;
        } else if (object->second.filePointer) {
            ret = 0;
            lastError_ = 6;
        } else if (object->second.kind == GuestHandle::Kind::GuestBrush) {
            brushes_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestFont) {
            fonts_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostBitmap) {
#if defined(_WIN32)
            if (object->second.hostValue) DeleteObject(reinterpret_cast<HGDIOBJ>(object->second.hostValue));
#endif
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else {
            lastError_ = 6;
            ret = 0;
        }
    } else if (name == "SetBkColor" || name == "SetTextColor" || name == "SetBkMode" || name == "SetTextAlign") {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else if (name == "SetBkColor") {
            ret = dc->bkColor;
            dc->bkColor = a1;
            lastError_ = 0;
        } else if (name == "SetTextColor") {
            ret = dc->textColor;
            dc->textColor = a1;
            lastError_ = 0;
        } else if (name == "SetBkMode") {
            ret = dc->bkMode;
            dc->bkMode = a1;
            lastError_ = 0;
        } else {
            ret = dc->textAlign;
            dc->textAlign = a1;
            lastError_ = 0;
        }
    } else if (name == "FillRect") {
        GuestDc* dc = lookupGuestDc(a0);
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        auto brush = brushes_.find(a2);
        if (!dc || !readGuestRect(a1, left, top, right, bottom) || brush == brushes_.end()) {
            lastError_ = 87;
            ret = 0;
        } else {
            fillFramebufferRect(*dc, left, top, right, bottom, colorRefToPixel(brush->second.colorRef));
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "PatBlt") {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        const uint32_t rop = stackArg(5);
        if (!dc || brush == brushes_.end() || rop != 0x00f00021u) {
            lastError_ = dc ? 120 : 6;
            ret = 0;
        } else {
            fillFramebufferRect(*dc, int32_t(a1), int32_t(a2),
                                int32_t(a1) + int32_t(a3),
                                int32_t(a2) + int32_t(stackArg(4)),
                                colorRefToPixel(brush->second.colorRef));
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "Rectangle") {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        if (!dc || brush == brushes_.end()) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            const uint32_t pixel = colorRefToPixel(brush->second.colorRef);
            const int32_t left = int32_t(a1);
            const int32_t top = int32_t(a2);
            const int32_t right = int32_t(a3);
            const int32_t bottom = int32_t(stackArg(4));
            drawFramebufferLine(*dc, left, top, right - 1, top, pixel);
            drawFramebufferLine(*dc, left, bottom - 1, right - 1, bottom - 1, pixel);
            drawFramebufferLine(*dc, left, top, left, bottom - 1, pixel);
            drawFramebufferLine(*dc, right - 1, top, right - 1, bottom - 1, pixel);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "MoveToEx") {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0;
        } else {
            if (a3) {
                writeU32(a3, uint32_t(dc->x));
                writeU32(a3 + 4, uint32_t(dc->y));
            }
            dc->x = int32_t(a1);
            dc->y = int32_t(a2);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "LineTo") {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        if (!dc || brush == brushes_.end()) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            drawFramebufferLine(*dc, dc->x, dc->y, int32_t(a1), int32_t(a2),
                                colorRefToPixel(brush->second.colorRef));
            dc->x = int32_t(a1);
            dc->y = int32_t(a2);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "StretchDIBits") {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else {
            const bool ok = stackArg(11) == 0 && stackArg(12) == 0x00cc0020u &&
                stretchDibToFramebuffer(*dc, int32_t(a1), int32_t(a2), int32_t(a3),
                                        int32_t(stackArg(4)), int32_t(stackArg(5)),
                                        int32_t(stackArg(6)), int32_t(stackArg(7)),
                                        int32_t(stackArg(8)), stackArg(9), stackArg(10));
            if (ok) {
                ret = uint32_t(std::abs(int32_t(stackArg(8))));
                lastError_ = 0;
            } else {
                lastError_ = 120;
                ret = 0xffffffffu;
            }
        }
    } else if (name == "ExtTextOutW" || name == "DrawTextW") {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "CreateCompatibleDC" || name == "BitBlt") {
        lastError_ = 120;
        ret = 0;
    } else if (name == "GetWindowLongW") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = getWindowLongValue(it->second, int32_t(a1));
        }
    } else if (name == "SetWindowLongW") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = setWindowLongValue(it->second, int32_t(a1), a2);
        }
    } else if (name == "GetParent") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = it->second.parent;
        }
    } else if (name == "GetWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else if (a1 == 5) {
            ret = 0;
            for (const auto& [hwnd, window] : windows_) {
                if (window.parent == a0) {
                    ret = hwnd;
                    break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 2 || a1 == 3) {
            std::vector<uint32_t> siblings;
            for (const auto& [hwnd, window] : windows_) {
                if (window.parent == it->second.parent) siblings.push_back(hwnd);
            }
            auto pos = std::find(siblings.begin(), siblings.end(), a0);
            if (pos == siblings.end()) {
                ret = 0;
            } else if (a1 == 2 && ++pos != siblings.end()) {
                ret = *pos;
            } else if (a1 == 3 && pos != siblings.begin()) {
                ret = *--pos;
            } else {
                ret = 0;
            }
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 0 || a1 == 1) {
            ret = 0;
            for (const auto& [hwnd, window] : windows_) {
                if (window.parent == it->second.parent) {
                    ret = hwnd;
                    if (a1 == 0) break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (name == "SetWindowPos") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const uint32_t flags = stackArg(6);
            const int32_t newWidth = int32_t(stackArg(4));
            const int32_t newHeight = int32_t(stackArg(5));
            const bool oldVisible = it->second.visible;
            const bool sizeChanged = !(flags & 0x0001u) &&
                (it->second.width != newWidth || it->second.height != newHeight);
            if (!(flags & 0x0002u)) {
                it->second.x = int32_t(a2);
                it->second.y = int32_t(a3);
            }
            if (!(flags & 0x0001u)) {
                it->second.width = newWidth;
                it->second.height = newHeight;
            }
            if (flags & 0x0040u) it->second.visible = true;  // SWP_SHOWWINDOW
            if (flags & 0x0080u) it->second.visible = false; // SWP_HIDEWINDOW
            if (sizeChanged) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0005; // WM_SIZE
                message.lParam = (uint32_t(uint16_t(it->second.height)) << 16) | uint16_t(it->second.width);
                message.time = uint32_t(++tick_ * 16);
                guestMessages_.push_back(message);
            }
            if (it->second.visible != oldVisible) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0018; // WM_SHOWWINDOW
                message.wParam = it->second.visible ? 1 : 0;
                message.time = uint32_t(++tick_ * 16);
                guestMessages_.push_back(message);
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "DestroyWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            GuestMessage message{};
            message.hwnd = a0;
            message.message = 0x0002; // WM_DESTROY
            message.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(message);
            windows_.erase(it);
            guestHandles_.erase(a0);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "ShowWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool wasVisible = it->second.visible;
            it->second.visible = a1 != 0;
            if (it->second.visible != wasVisible) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0018; // WM_SHOWWINDOW
                message.wParam = it->second.visible ? 1 : 0;
                message.time = uint32_t(++tick_ * 16);
                guestMessages_.push_back(message);
            }
            lastError_ = 0;
            ret = wasVisible ? 1 : 0;
        }
    } else if (name == "UpdateWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            GuestMessage message{};
            message.hwnd = a0;
            message.message = 0x000f; // WM_PAINT
            message.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(message);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "DefWindowProcW") {
        ret = 0;
    } else if (name == "GetMessagePos") {
        ret = 0;
    } else if (name == "TranslateMessage") {
        ret = a0 ? 1 : 0;
    } else if (name == "PostQuitMessage") {
        quitPosted_ = true;
        GuestMessage message{};
        message.message = 0x0012; // WM_QUIT
        message.wParam = a0;
        message.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(message);
        ret = 0;
    } else if (name == "PostMessageW") {
        if (!windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            GuestMessage message{};
            message.hwnd = a0;
            message.message = a1;
            message.wParam = a2;
            message.lParam = a3;
            message.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(message);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "GetMessageW" || name == "GetMessageWNoWait" || name == "PeekMessageW") {
        const bool peek = name == "PeekMessageW" || name == "GetMessageWNoWait";
        const uint32_t removeFlags = peek ? stackArg(4) : 1;
        GuestMessage message{};
        bool haveMessage = false;
        if (!guestMessages_.empty()) {
            message = guestMessages_.front();
            haveMessage = true;
            if (!peek || (removeFlags & 1)) guestMessages_.pop_front();
        }
        if (!haveMessage) {
            ret = 0;
            if (!peek && !quitPosted_) {
                spdlog::info("synthetic coredll.dll!GetMessageW blocking with empty guest queue");
                uc_emu_stop(uc_);
            }
        } else if (message.message == 0x0012) {
            writeGuestMessage(a0, message);
            ret = 0;
        } else {
            writeGuestMessage(a0, message);
            ret = 1;
        }
    } else if (name == "IsWindowVisible") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = it->second.visible ? 1 : 0;
        }
    } else if (name == "FindResource" || name == "FindResourceW") {
        const ResourceEntry* resource = findResource(a2, a1);
        if (!resource) {
            lastError_ = 1814;
            ret = 0;
        } else {
            const size_t index = size_t(resource - mainResources_.data());
            ret = makeGuestHandle({GuestHandle::Kind::GuestResource, index + 1, 0});
            lastError_ = 0;
        }
    } else if (name == "SizeofResource") {
        const ResourceEntry* resource = resourceFromHandle(a1);
        if (!resource) {
            lastError_ = 1814;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = uint32_t(resource->data.size());
        }
    } else if (name == "LoadResource") {
        const ResourceEntry* resource = resourceFromHandle(a1);
        if (!resource) {
            lastError_ = 1814;
            ret = 0;
        } else {
            auto loaded = loadedResourceMemory_.find(a1);
            if (loaded != loadedResourceMemory_.end()) {
                ret = loaded->second;
            } else {
                ret = allocate(uint32_t(resource->data.size() ? resource->data.size() : 1), false);
                if (ret && !resource->data.empty()) {
                    uc_mem_write(uc_, ret, resource->data.data(), resource->data.size());
                }
                loadedResourceMemory_[a1] = ret;
            }
            lastError_ = ret ? 0 : 8;
        }
    } else if (name == "LoadStringW") {
        const uint32_t blockId = (a1 >> 4) + 1;
        const uint32_t stringIndex = a1 & 0x0f;
        const ResourceEntry* resource = nullptr;
        for (const auto& candidate : mainResources_) {
            if (candidate.type.ordinal && candidate.type.id == 6 &&
                candidate.name.ordinal && candidate.name.id == blockId) {
                resource = &candidate;
                break;
            }
        }
        std::string value;
        if (resource) {
            size_t offset = 0;
            for (uint32_t i = 0; i < 16 && offset + 2 <= resource->data.size(); ++i) {
                const uint16_t length = readLe16(resource->data, offset);
                offset += 2;
                if (i == stringIndex) {
                    value = utf16FromBytes(resource->data, offset, length);
                    break;
                }
                offset += size_t(length) * 2;
            }
        }
        if (!resource) {
            lastError_ = 1814;
            ret = 0;
        } else {
            ret = a2 && a3 ? writeUtf16(a2, value, a3) : uint32_t(value.size());
            lastError_ = 0;
        }
    } else if (name == "LoadAcceleratorsW") {
        const ResourceEntry* resource = findResource(9, a1);
#if defined(_WIN32)
        if (!resource || resource->data.empty()) {
            lastError_ = 1814;
            ret = 0;
        } else {
            const int count = int(resource->data.size() / sizeof(ACCEL));
            HACCEL accel = count > 0
                ? CreateAcceleratorTableW(reinterpret_cast<LPACCEL>(const_cast<uint8_t*>(resource->data.data())), count)
                : nullptr;
            if (accel) {
                ret = makeGuestHandle({GuestHandle::Kind::HostAccelerator, reinterpret_cast<uintptr_t>(accel), 0});
                lastError_ = 0;
            } else {
                lastError_ = GetLastError();
                ret = 0;
            }
        }
#else
        lastError_ = resource ? 0 : 1814;
        ret = 0;
#endif
    } else if (name == "LoadIconW" || name == "LoadImageW") {
#if defined(_WIN32)
        const uint32_t imageType = name == "LoadImageW" ? a2 : IMAGE_ICON;
        const uint32_t desiredCx = name == "LoadImageW" ? a3 : 0;
        const uint32_t desiredCy = name == "LoadImageW" ? stackArg(4) : 0;
        const uint32_t loadFlags = name == "LoadImageW" ? stackArg(5) : LR_DEFAULTCOLOR;
        auto createIconFromMainResource = [&](uint32_t nameArg) -> HICON {
            const ResourceEntry* group = findResource(14, nameArg); // RT_GROUP_ICON
            if (!group || group->data.size() < 6) return nullptr;
            const uint16_t count = readLe16(group->data, 4);
            const uint32_t targetCx = desiredCx ? desiredCx : uint32_t(GetSystemMetrics(SM_CXICON));
            const uint32_t targetCy = desiredCy ? desiredCy : uint32_t(GetSystemMetrics(SM_CYICON));
            const ResourceEntry* iconResource = nullptr;
            int bestScore = 0x7fffffff;
            for (uint16_t i = 0; i < count; ++i) {
                const size_t offset = 6 + size_t(i) * 14;
                if (offset + 14 > group->data.size()) break;
                const uint32_t width = group->data[offset] ? group->data[offset] : 256;
                const uint32_t height = group->data[offset + 1] ? group->data[offset + 1] : 256;
                const uint32_t bytesInResource = readLe32(group->data, offset + 8);
                const uint32_t iconId = readLe16(group->data, offset + 12);
                const ResourceEntry* candidate = findResource(3, iconId); // RT_ICON
                if (!candidate || candidate->data.empty()) continue;
                const int score = std::abs(int(width) - int(targetCx)) + std::abs(int(height) - int(targetCy));
                if (!iconResource || score < bestScore ||
                    (score == bestScore && bytesInResource > iconResource->data.size())) {
                    iconResource = candidate;
                    bestScore = score;
                }
            }
            if (!iconResource) return nullptr;
            return CreateIconFromResourceEx(const_cast<BYTE*>(iconResource->data.data()),
                                            DWORD(iconResource->data.size()), TRUE, 0x00030000,
                                            int(desiredCx), int(desiredCy),
                                            loadFlags & (LR_DEFAULTCOLOR | LR_MONOCHROME));
        };
        auto createBitmapFromMainResource = [&](uint32_t nameArg) -> HBITMAP {
            const ResourceEntry* bitmap = findResource(2, nameArg); // RT_BITMAP
            if (!bitmap || bitmap->data.size() < 40) return nullptr;
            const uint32_t headerSize = readLe32(bitmap->data, 0);
            if (headerSize < 40 || headerSize > bitmap->data.size()) return nullptr;
            const uint16_t bitCount = readLe16(bitmap->data, 14);
            const uint32_t compression = readLe32(bitmap->data, 16);
            const uint32_t clrUsed = readLe32(bitmap->data, 32);
            uint32_t colorCount = clrUsed;
            if (!colorCount && bitCount <= 8) colorCount = 1u << bitCount;
            size_t bitsOffset = size_t(headerSize) + size_t(colorCount) * 4;
            if (compression == BI_BITFIELDS && !colorCount && (bitCount == 16 || bitCount == 32)) {
                bitsOffset += 12;
            }
            if (bitsOffset > bitmap->data.size()) return nullptr;
            HDC dc = GetDC(nullptr);
            if (!dc) return nullptr;
            HBITMAP result = CreateDIBitmap(dc,
                                            reinterpret_cast<const BITMAPINFOHEADER*>(bitmap->data.data()),
                                            CBM_INIT,
                                            bitmap->data.data() + bitsOffset,
                                            reinterpret_cast<const BITMAPINFO*>(bitmap->data.data()),
                                            DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            if (result && (desiredCx || desiredCy)) {
                HANDLE scaled = CopyImage(result, IMAGE_BITMAP, int(desiredCx), int(desiredCy),
                                          loadFlags & (LR_COPYDELETEORG | LR_COPYRETURNORG | LR_MONOCHROME));
                if (scaled && scaled != result) result = reinterpret_cast<HBITMAP>(scaled);
            }
            return result;
        };

        if (imageType == IMAGE_ICON) {
            HICON icon = createIconFromMainResource(a1);
            if (icon) {
                ret = makeGuestHandle({GuestHandle::Kind::HostIcon, reinterpret_cast<uintptr_t>(icon), 0});
                lastError_ = 0;
            } else {
                ret = 0;
                lastError_ = 1814;
            }
        } else if (imageType == IMAGE_BITMAP) {
            HBITMAP bitmap = createBitmapFromMainResource(a1);
            if (bitmap) {
                ret = makeGuestHandle({GuestHandle::Kind::HostBitmap, reinterpret_cast<uintptr_t>(bitmap), 0});
                lastError_ = 0;
            } else {
                ret = 0;
                lastError_ = 1814;
            }
        } else {
            ret = 0;
            lastError_ = 1814;
        }
#else
        const uint32_t type = name == "LoadImageW" ? a2 : 1;
        lastError_ = findResource(type == 0 ? 2 : 14, a1) ? 120 : 1814;
        ret = 0;
#endif
    } else if (name == "LoadMenuW") {
        const ResourceEntry* resource = findResource(4, a1);
#if defined(_WIN32)
        if (!resource || resource->data.empty()) {
            lastError_ = 1814;
            ret = 0;
        } else {
            HMENU menu = LoadMenuIndirectW(reinterpret_cast<const MENUTEMPLATEW*>(resource->data.data()));
            if (menu) {
                ret = makeGuestHandle({GuestHandle::Kind::HostMenu, reinterpret_cast<uintptr_t>(menu), 0});
                lastError_ = 0;
            } else {
                lastError_ = GetLastError();
                ret = 0;
            }
        }
#else
        lastError_ = resource ? 0 : 1814;
        ret = 0;
#endif
    } else if (name == "RemoveMenu" || name == "CheckMenuItem" || name == "CheckMenuRadioItem") {
        auto* handle = lookupGuestHandle(a0);
#if defined(_WIN32)
        if (!handle || handle->kind != GuestHandle::Kind::HostMenu || !handle->hostValue) {
            lastError_ = 1401;
            ret = name == "CheckMenuItem" ? 0xffffffffu : 0;
        } else if (name == "RemoveMenu") {
            ret = RemoveMenu(reinterpret_cast<HMENU>(handle->hostValue), a1, a2) ? 1 : 0;
            if (!ret) lastError_ = GetLastError();
        } else if (name == "CheckMenuItem") {
            ret = CheckMenuItem(reinterpret_cast<HMENU>(handle->hostValue), a1, a2);
            if (ret == 0xffffffffu) lastError_ = GetLastError();
        } else {
            ret = CheckMenuRadioItem(reinterpret_cast<HMENU>(handle->hostValue), a1, a2, a3, stackArg(4)) ? 1 : 0;
            if (!ret) lastError_ = GetLastError();
        }
#else
        lastError_ = handle ? 0 : 1401;
        ret = name == "CheckMenuItem" ? 0xffffffffu : 0;
#endif
    } else if (name == "GetSystemInfo") {
        if (a0) {
            writeU32(a0, 0); // wProcessorArchitecture + wReserved
            writeU32(a0 + 4, 0x1000);
            writeU32(a0 + 8, 0x00010000);
            writeU32(a0 + 12, 0x7ffeffff);
            writeU32(a0 + 16, 1);
            writeU32(a0 + 20, 1);
            writeU32(a0 + 24, 4000); // PROCESSOR_INTEL_386-ish placeholder is not consumed as a handle.
            writeU32(a0 + 28, 0x10000);
            writeU32(a0 + 32, 0);
            writeU32(a0 + 36, 0);
        }
        ret = 0;
    } else if (name == "GetStoreInformation") {
        if (a0) {
            writeU32(a0, 64u * 1024u * 1024u);
            writeU32(a0 + 4, 32u * 1024u * 1024u);
        }
        ret = a0 ? 1 : 0;
    } else if (name == "RegCreateKeyExW") {
        const auto path = registryPathFromHandle(a0, readUtf16(a1, 1024));
        const uint32_t resultPtr = stackArg(7);
        const uint32_t dispositionPtr = stackArg(8);
        if (!path || !resultPtr) {
            ret = 87;
        } else {
            const bool existed = registryKeyExists(*path);
            registryEnsureKey(*path);
            writeU32(resultPtr, makeRegistryHandle(*path));
            if (dispositionPtr) writeU32(dispositionPtr, existed ? 2 : 1);
            ret = 0;
        }
    } else if (name == "RegOpenKeyExW") {
        const auto path = registryPathFromHandle(a0, readUtf16(a1, 1024));
        const uint32_t resultPtr = stackArg(4);
        if (!path || !resultPtr) {
            ret = 87;
        } else if (!registryKeyExists(*path)) {
            writeU32(resultPtr, 0);
            ret = 2;
        } else {
            writeU32(resultPtr, makeRegistryHandle(*path));
            ret = 0;
        }
    } else if (name == "RegQueryValueExW") {
        const auto path = registryPathFromHandle(a0, {});
        const nlohmann::json* value = path ? registryValue(*path, readUtf16(a1, 1024)) : nullptr;
        const uint32_t typePtr = a3;
        const uint32_t dataPtr = stackArg(4);
        const uint32_t sizePtr = stackArg(5);
        if (!path || !sizePtr) {
            ret = 87;
        } else if (!value) {
            ret = 2;
        } else {
            const uint32_t type = registryTypeId(*value);
            const std::vector<uint8_t> bytes = registryValueBytes(*value);
            const uint32_t capacity = readU32(sizePtr);
            if (typePtr) writeU32(typePtr, type);
            writeU32(sizePtr, uint32_t(bytes.size()));
            if (!dataPtr) {
                ret = 0;
            } else if (capacity < bytes.size()) {
                ret = 234;
            } else {
                if (!bytes.empty()) uc_mem_write(uc_, dataPtr, bytes.data(), bytes.size());
                ret = 0;
            }
        }
    } else if (name == "RegSetValueExW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path)) {
            ret = 6;
        } else {
            const std::string valueName = normalizeRegistryValueName(readUtf16(a1, 1024));
            registry_["keys"][*path]["values"][valueName] = registryJsonFromBytes(a3, stackArg(4), stackArg(5));
            registryDirty_ = true;
            flushRegistry();
            ret = 0;
        }
    } else if (name == "RegDeleteValueW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path)) {
            ret = 6;
        } else {
            const std::string valueName = normalizeRegistryValueName(readUtf16(a1, 1024));
            auto& values = registry_["keys"][*path]["values"];
            ret = values.erase(valueName) ? 0 : 2;
            if (!ret) {
                registryDirty_ = true;
                flushRegistry();
            }
        }
    } else if (name == "RegDeleteKeyW") {
        const auto path = registryPathFromHandle(a0, readUtf16(a1, 1024));
        if (!path || !registryKeyExists(*path)) {
            ret = 2;
        } else {
            std::vector<std::string> doomed;
            for (auto it = registry_["keys"].begin(); it != registry_["keys"].end(); ++it) {
                if (it.key() == *path || it.key().rfind(*path + "\\", 0) == 0) doomed.push_back(it.key());
            }
            for (const auto& key : doomed) registry_["keys"].erase(key);
            registryDirty_ = true;
            flushRegistry();
            ret = 0;
        }
    } else if (name == "RegEnumKeyExW") {
        const auto path = registryPathFromHandle(a0, {});
        const std::vector<std::string> children = path ? registryChildNames(*path) : std::vector<std::string>{};
        if (!path || !a2 || !a3) {
            ret = 87;
        } else if (a1 >= children.size()) {
            ret = 259;
        } else {
            const uint32_t capacity = readU32(a3);
            const std::string& child = children[a1];
            if (capacity <= child.size()) {
                writeU32(a3, uint32_t(child.size() + 1));
                ret = 234;
            } else {
                writeUtf16(a2, child, capacity);
                writeU32(a3, uint32_t(child.size()));
                ret = 0;
            }
        }
    } else if (name == "RegEnumValueW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path) || !a2 || !a3) {
            ret = 87;
        } else {
            auto& values = registry_["keys"][*path]["values"];
            if (a1 >= values.size()) {
                ret = 259;
            } else {
                auto it = values.begin();
                std::advance(it, a1);
                const std::string valueName = it.key();
                const uint32_t nameCapacity = readU32(a3);
                if (nameCapacity <= valueName.size()) {
                    writeU32(a3, uint32_t(valueName.size() + 1));
                    ret = 234;
                } else {
                    writeUtf16(a2, valueName, nameCapacity);
                    writeU32(a3, uint32_t(valueName.size()));
                    if (stackArg(5)) writeU32(stackArg(5), registryTypeId(it.value()));
                    const std::vector<uint8_t> bytes = registryValueBytes(it.value());
                    const uint32_t dataPtr = stackArg(6);
                    const uint32_t sizePtr = stackArg(7);
                    if (sizePtr) {
                        const uint32_t capacity = readU32(sizePtr);
                        writeU32(sizePtr, uint32_t(bytes.size()));
                        if (dataPtr && capacity >= bytes.size() && !bytes.empty()) uc_mem_write(uc_, dataPtr, bytes.data(), bytes.size());
                        ret = dataPtr && capacity < bytes.size() ? 234 : 0;
                    } else {
                        ret = 0;
                    }
                }
            }
        }
    } else if (name == "RegQueryInfoKeyW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path)) {
            ret = 6;
        } else {
            const auto children = registryChildNames(*path);
            const auto& values = registry_["keys"][*path]["values"];
            if (stackArg(4)) writeU32(stackArg(4), uint32_t(children.size()));
            if (stackArg(6)) writeU32(stackArg(6), uint32_t(values.size()));
            ret = 0;
        }
    } else if (name == "RegFlushKey") {
        flushRegistry();
        ret = 0;
    } else if (name == "RegCloseKey") {
        if (registryRootName(a0).empty()) {
            registryHandles_.erase(a0);
            guestHandles_.erase(a0);
        }
        ret = 0;
    } else if (name == "GlobalAddAtomW") {
        const std::string atomName = lowerAscii(readUtf16(a0));
        if (atomName.empty()) {
            lastError_ = 87;
            ret = 0;
        } else {
            auto it = atomsByName_.find(atomName);
            if (it == atomsByName_.end()) {
                const uint16_t atom = nextAtom_++;
                it = atomsByName_.emplace(atomName, atom).first;
                atomNames_[atom] = atomName;
            }
            lastError_ = 0;
            ret = it->second;
        }
    } else if (name == "GlobalFindAtomW") {
        const std::string atomName = lowerAscii(readUtf16(a0));
        auto it = atomsByName_.find(atomName);
        ret = it == atomsByName_.end() ? 0 : it->second;
        lastError_ = ret ? 0 : 2;
    } else if (name == "GlobalDeleteAtom") {
        auto it = atomNames_.find(uint16_t(a0));
        if (it != atomNames_.end()) {
            atomsByName_.erase(it->second);
            atomNames_.erase(it);
        }
        ret = 0;
        lastError_ = 0;
    } else if (name == "WNetGetUserW") {
        ret = handleWNetGetUserW(a0, a1, a2);
    } else if (name == "WNetGetUniversalNameW" || name == "WNetConnectionDialog1W") {
        lastError_ = 1200;
        ret = 1200;
    } else if (name == "waveInOpen") {
#if defined(_WIN32)
        auto& winmm = winmmBridge();
        if (!winmm.waveInOpen || !a0 || !a2) {
            ret = MMSYSERR_ERROR;
        } else {
            WAVEFORMATEX format{};
            if (uc_mem_read(uc_, a2, &format, sizeof(format)) != UC_ERR_OK) {
                ret = MMSYSERR_INVALPARAM;
            } else {
                HWAVEIN host{};
                ret = winmm.waveInOpen(&host, a1, &format, 0, 0, CALLBACK_NULL);
                if (ret == MMSYSERR_NOERROR) {
                    const uint32_t guest = makeGuestHandle({
                        GuestHandle::Kind::HostWaveIn,
                        reinterpret_cast<uintptr_t>(host),
                        0,
                    });
                    writeU32(a0, guest);
                }
            }
        }
#else
        ret = 2;
#endif
    } else if (name == "waveInReset") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        ret = handle && handle->kind == GuestHandle::Kind::HostWaveIn && handle->hostValue && winmm.waveInReset
            ? winmm.waveInReset(reinterpret_cast<HWAVEIN>(handle->hostValue))
            : MMSYSERR_INVALHANDLE;
#else
        ret = 2;
#endif
    } else if (name == "waveInAddBuffer" || name == "waveInUnprepareHeader") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !a1) {
            ret = MMSYSERR_INVALHANDLE;
        } else {
            uint32_t guestData = 0;
            uint32_t guestLength = 0;
            uint32_t guestBytesRecorded = 0;
            uint32_t guestUser = 0;
            uint32_t guestFlags = 0;
            uint32_t guestLoops = 0;
            uc_mem_read(uc_, a1, &guestData, sizeof(guestData));
            uc_mem_read(uc_, a1 + 4, &guestLength, sizeof(guestLength));
            uc_mem_read(uc_, a1 + 8, &guestBytesRecorded, sizeof(guestBytesRecorded));
            uc_mem_read(uc_, a1 + 12, &guestUser, sizeof(guestUser));
            uc_mem_read(uc_, a1 + 16, &guestFlags, sizeof(guestFlags));
            uc_mem_read(uc_, a1 + 20, &guestLoops, sizeof(guestLoops));

            if (name == "waveInAddBuffer") {
                if (!winmm.waveInAddBuffer || !guestData || !guestLength || guestLength > 0x100000) {
                    ret = MMSYSERR_INVALPARAM;
                } else {
                    auto& stored = hostWaveBuffers_[a1];
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
                    writeU32(a1 + 8, header->dwBytesRecorded);
                    writeU32(a1 + 16, header->dwFlags);
                }
            } else {
                auto it = hostWaveBuffers_.find(a1);
                if (it == hostWaveBuffers_.end() || !winmm.waveInUnprepareHeader) {
                    ret = MMSYSERR_INVALPARAM;
                } else {
                    auto* header = reinterpret_cast<WAVEHDR*>(it->second.header.data());
                    ret = winmm.waveInUnprepareHeader(reinterpret_cast<HWAVEIN>(handle->hostValue), header, sizeof(*header));
                    if (guestData && header->dwBytesRecorded) {
                        uc_mem_write(uc_, guestData, it->second.data.data(),
                                     std::min<size_t>(it->second.data.size(), header->dwBytesRecorded));
                    }
                    writeU32(a1 + 8, header->dwBytesRecorded);
                    writeU32(a1 + 16, header->dwFlags);
                    hostWaveBuffers_.erase(it);
                }
            }
        }
#else
        ret = 2;
#endif
    } else if (name == "waveInMessage") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        ret = handle && handle->kind == GuestHandle::Kind::HostWaveIn && handle->hostValue && winmm.waveInMessage
            ? winmm.waveInMessage(reinterpret_cast<HWAVEIN>(handle->hostValue), a1, a2, a3)
            : MMSYSERR_INVALHANDLE;
#else
        ret = 2;
#endif
    } else if (name == "mixerGetControlDetails") {
#if defined(_WIN32)
        auto& winmm = winmmBridge();
        ret = winmm.mixerGetControlDetailsW && a1 ? MMSYSERR_INVALPARAM : MMSYSERR_ERROR;
#else
        ret = 2;
#endif
    } else {
        return false;
    }

    return true;
}

bool SyntheticDllRuntime::dispatchWinsock(const std::string& name,
                                          const GuestCallArgs& args,
                                          uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
#if defined(_WIN32)
    auto guestSocket = [&](uint32_t handle) -> SOCKET {
        auto* guest = lookupGuestHandle(handle);
        return guest && guest->kind == GuestHandle::Kind::HostSocket
            ? SOCKET(guest->hostValue)
            : INVALID_SOCKET;
    };
    auto setSocketError = [&]() {
        lastError_ = WSAGetLastError();
    };
    auto readSockaddr = [&](uint32_t address, uint32_t length, sockaddr_storage& storage, int& outLength) -> bool {
        if (!address || !length || length > sizeof(storage)) return false;
        std::memset(&storage, 0, sizeof(storage));
        outLength = int(length);
        return uc_mem_read(uc_, address, &storage, length) == UC_ERR_OK;
    };

    if (name == "WSAStartup") {
        WSADATA data{};
        ret = WSAStartup(MAKEWORD(uint8_t(a0), uint8_t(a0 >> 8)), &data);
        if (a1) {
            std::array<uint8_t, 400> guest{};
            std::memcpy(guest.data(), &data, std::min(sizeof(data), guest.size()));
            uc_mem_write(uc_, a1, guest.data(), guest.size());
        }
    } else if (name == "WSACleanup") {
        ret = WSACleanup();
        if (ret) setSocketError();
    } else if (name == "socket") {
        const SOCKET host = ::socket(int(a0), int(a1), int(a2));
        if (host == INVALID_SOCKET) {
            setSocketError();
            ret = 0xffffffffu;
        } else {
            ret = makeGuestHandle({GuestHandle::Kind::HostSocket, uintptr_t(host), 0});
            lastError_ = 0;
        }
    } else if (name == "closesocket") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostSocket) {
            lastError_ = WSAENOTSOCK;
            ret = 0xffffffffu;
        } else {
            ret = closesocket(SOCKET(handle->hostValue));
            guestHandles_.erase(a0);
            lastError_ = ret ? WSAGetLastError() : 0;
        }
    } else if (name == "connect" || name == "bind") {
        sockaddr_storage storage{};
        int length = 0;
        const SOCKET s = guestSocket(a0);
        if (s == INVALID_SOCKET || !readSockaddr(a1, a2, storage, length)) {
            lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
            ret = 0xffffffffu;
        } else {
            ret = name == "connect"
                ? ::connect(s, reinterpret_cast<sockaddr*>(&storage), length)
                : ::bind(s, reinterpret_cast<sockaddr*>(&storage), length);
            if (ret) setSocketError();
            else lastError_ = 0;
        }
    } else if (name == "listen") {
        const SOCKET s = guestSocket(a0);
        ret = s == INVALID_SOCKET ? SOCKET_ERROR : ::listen(s, int(a1));
        if (ret) setSocketError();
    } else if (name == "accept") {
        const SOCKET s = guestSocket(a0);
        sockaddr_storage storage{};
        int length = a2 ? int(readU32(a2)) : sizeof(storage);
        const SOCKET accepted = s == INVALID_SOCKET
            ? INVALID_SOCKET
            : ::accept(s, reinterpret_cast<sockaddr*>(&storage), a1 ? &length : nullptr);
        if (accepted == INVALID_SOCKET) {
            setSocketError();
            ret = 0xffffffffu;
        } else {
            if (a1 && a2 && length > 0) {
                uc_mem_write(uc_, a1, &storage, size_t(length));
                writeU32(a2, uint32_t(length));
            }
            ret = makeGuestHandle({GuestHandle::Kind::HostSocket, uintptr_t(accepted), 0});
            lastError_ = 0;
        }
    } else if (name == "recv" || name == "send") {
        const SOCKET s = guestSocket(a0);
        if (s == INVALID_SOCKET || (a2 && !a1)) {
            lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
            ret = 0xffffffffu;
        } else if (name == "recv") {
            std::vector<char> bytes(a2);
            const int got = ::recv(s, bytes.data(), int(bytes.size()), int(a3));
            if (got >= 0 && got) uc_mem_write(uc_, a1, bytes.data(), size_t(got));
            ret = got < 0 ? 0xffffffffu : uint32_t(got);
            if (got < 0) setSocketError();
            else lastError_ = 0;
        } else {
            std::vector<char> bytes(a2);
            if (a2) uc_mem_read(uc_, a1, bytes.data(), bytes.size());
            const int sent = ::send(s, bytes.data(), int(bytes.size()), int(a3));
            ret = sent < 0 ? 0xffffffffu : uint32_t(sent);
            if (sent < 0) setSocketError();
            else lastError_ = 0;
        }
    } else if (name == "ioctlsocket") {
        const SOCKET s = guestSocket(a0);
        u_long value = a2 ? readU32(a2) : 0;
        ret = s == INVALID_SOCKET ? SOCKET_ERROR : ::ioctlsocket(s, long(a1), &value);
        if (a2) writeU32(a2, uint32_t(value));
        if (ret) setSocketError();
    } else if (name == "setsockopt") {
        const SOCKET s = guestSocket(a0);
        const uint32_t optLen = stackArg(4);
        std::vector<char> value(optLen);
        if (a3 && optLen) uc_mem_read(uc_, a3, value.data(), value.size());
        ret = s == INVALID_SOCKET ? SOCKET_ERROR : ::setsockopt(s, int(a1), int(a2), value.data(), int(value.size()));
        if (ret) setSocketError();
    } else if (name == "select") {
        auto buildSet = [&](uint32_t ptr, fd_set& host, std::vector<uint32_t>& guestHandles) {
            FD_ZERO(&host);
            guestHandles.clear();
            if (!ptr) return;
            const uint32_t count = std::min<uint32_t>(readU32(ptr), FD_SETSIZE);
            for (uint32_t i = 0; i < count; ++i) {
                const uint32_t guest = readU32(ptr + 4 + i * 4);
                const SOCKET s = guestSocket(guest);
                if (s != INVALID_SOCKET) {
                    FD_SET(s, &host);
                    guestHandles.push_back(guest);
                }
            }
        };
        auto writeSet = [&](uint32_t ptr, const fd_set& host, const std::vector<uint32_t>& guestHandles) {
            if (!ptr) return;
            uint32_t count = 0;
            for (uint32_t guest : guestHandles) {
                const SOCKET s = guestSocket(guest);
                if (s != INVALID_SOCKET && FD_ISSET(s, const_cast<fd_set*>(&host))) {
                    writeU32(ptr + 4 + count * 4, guest);
                    ++count;
                }
            }
            writeU32(ptr, count);
        };
        fd_set readSet{}, writeSetHost{}, exceptSet{};
        std::vector<uint32_t> readGuest, writeGuest, exceptGuest;
        buildSet(a1, readSet, readGuest);
        buildSet(a2, writeSetHost, writeGuest);
        buildSet(a3, exceptSet, exceptGuest);
        timeval timeout{};
        timeval* timeoutPtr = nullptr;
        const uint32_t timeoutGuest = stackArg(4);
        if (timeoutGuest) {
            timeout.tv_sec = long(readU32(timeoutGuest));
            timeout.tv_usec = long(readU32(timeoutGuest + 4));
            timeoutPtr = &timeout;
        }
        const int selected = ::select(0, a1 ? &readSet : nullptr, a2 ? &writeSetHost : nullptr,
                                      a3 ? &exceptSet : nullptr, timeoutPtr);
        if (selected == SOCKET_ERROR) {
            setSocketError();
            ret = 0xffffffffu;
        } else {
            writeSet(a1, readSet, readGuest);
            writeSet(a2, writeSetHost, writeGuest);
            writeSet(a3, exceptSet, exceptGuest);
            ret = uint32_t(selected);
            lastError_ = 0;
        }
    } else if (name == "__WSAFDIsSet") {
        ret = 0;
        if (a1) {
            const uint32_t count = readU32(a1);
            for (uint32_t i = 0; i < count; ++i) {
                if (readU32(a1 + 4 + i * 4) == a0) {
                    ret = 1;
                    break;
                }
            }
        }
    } else if (name == "htonl" || name == "ntohl") {
        ret = htonl(a0);
    } else if (name == "htons" || name == "ntohs") {
        ret = htons(uint16_t(a0));
    } else if (name == "inet_addr") {
        ret = inet_addr(readAscii(a0).c_str());
    } else if (name == "inet_ntoa") {
        in_addr addr{};
        addr.S_un.S_addr = a0;
        const char* text = inet_ntoa(addr);
        ret = allocate(uint32_t(std::strlen(text ? text : "") + 1), true);
        writeAscii(ret, text ? text : "");
    } else if (name == "gethostname") {
        std::vector<char> buffer(a1 ? a1 : 1);
        const int ok = ::gethostname(buffer.data(), int(buffer.size()));
        if (!ok) {
            size_t copyLen = 0;
            while (copyLen < buffer.size() && buffer[copyLen]) ++copyLen;
            if (copyLen < buffer.size()) ++copyLen;
            if (a0 && a1) uc_mem_write(uc_, a0, buffer.data(), copyLen);
            ret = 0;
            lastError_ = 0;
        } else {
            setSocketError();
            ret = 0xffffffffu;
        }
    } else if (name == "gethostbyname") {
        hostent* host = ::gethostbyname(readAscii(a0).c_str());
        if (!host || !host->h_addr_list || !host->h_addr_list[0]) {
            setSocketError();
            ret = 0;
        } else {
            const std::string hostName = host->h_name ? host->h_name : readAscii(a0);
            const uint32_t namePtr = allocate(uint32_t(hostName.size() + 1), true);
            writeAscii(namePtr, hostName);
            const uint32_t aliasesPtr = allocate(4, true);
            const uint32_t addrPtr = allocate(4, false);
            uc_mem_write(uc_, addrPtr, host->h_addr_list[0], 4);
            const uint32_t addrListPtr = allocate(8, true);
            writeU32(addrListPtr, addrPtr);
            ret = allocate(16, true);
            writeU32(ret, namePtr);
            writeU32(ret + 4, aliasesPtr);
            uint16_t family = uint16_t(host->h_addrtype);
            uint16_t length = uint16_t(host->h_length);
            uc_mem_write(uc_, ret + 8, &family, sizeof(family));
            uc_mem_write(uc_, ret + 10, &length, sizeof(length));
            writeU32(ret + 12, addrListPtr);
            lastError_ = 0;
        }
    } else {
        return false;
    }
    return true;
#else
    (void)name; (void)args;
    ret = 0xffffffffu;
    lastError_ = 10047;
    return true;
#endif
}

bool SyntheticDllRuntime::dispatchOle32(const std::string& name,
                                        const GuestCallArgs& args,
                                        uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    auto readGuid = [&](uint32_t ptr, GUID& guid) -> bool {
        return ptr && uc_mem_read(uc_, ptr, &guid, sizeof(guid)) == UC_ERR_OK;
    };
#if defined(_WIN32)
    auto wideFromGuest = [&](uint32_t ptr) {
        const std::string text = readUtf16(ptr, 256);
        return std::wstring(text.begin(), text.end());
    };
    auto asciiFromWide = [](const wchar_t* text) {
        std::string out;
        for (const wchar_t* p = text; p && *p; ++p) out.push_back(*p < 0x80 ? char(*p) : '?');
        return out;
    };
    if (name == "CoInitializeEx") {
        ret = uint32_t(::CoInitializeEx(nullptr, a1));
    } else if (name == "CoUninitialize") {
        ::CoUninitialize();
        ret = 0;
    } else if (name == "CoTaskMemAlloc") {
        ret = allocate(a0, false);
    } else if (name == "CoTaskMemRealloc") {
        const uint32_t oldSize = allocationSizes_.count(a0) ? allocationSizes_[a0] : 0;
        ret = allocate(a1, false);
        if (ret && a0 && oldSize) copyGuest(ret, a0, std::min(oldSize, a1));
    } else if (name == "CoTaskMemFree") {
        allocationSizes_.erase(a0);
        ret = 0;
    } else if (name == "CoTaskMemSize") {
        auto it = allocationSizes_.find(a0);
        ret = it == allocationSizes_.end() ? 0 : it->second;
    } else if (name == "CoCreateGuid") {
        GUID guid{};
        const HRESULT hr = ::CoCreateGuid(&guid);
        if (SUCCEEDED(hr) && a0) uc_mem_write(uc_, a0, &guid, sizeof(guid));
        ret = uint32_t(hr);
    } else if (name == "CLSIDFromString" || name == "CLSIDFromProgID") {
        GUID guid{};
        const std::wstring text = wideFromGuest(a0);
        const HRESULT hr = name == "CLSIDFromString"
            ? ::CLSIDFromString(text.c_str(), &guid)
            : ::CLSIDFromProgID(text.c_str(), &guid);
        if (SUCCEEDED(hr) && a1) uc_mem_write(uc_, a1, &guid, sizeof(guid));
        ret = uint32_t(hr);
    } else if (name == "StringFromGUID2") {
        GUID guid{};
        wchar_t text[64]{};
        if (!readGuid(a0, guid) || !a1 || !a2) {
            ret = 0;
        } else {
            const int written = ::StringFromGUID2(guid, text, int(a2));
            writeUtf16(a1, asciiFromWide(text), a2);
            ret = uint32_t(written);
        }
    } else if (name == "StringFromCLSID" || name == "StringFromIID" || name == "ProgIDFromCLSID") {
        GUID guid{};
        LPOLESTR hostText = nullptr;
        HRESULT hr = E_INVALIDARG;
        if (readGuid(a0, guid) && a1) {
            if (name == "ProgIDFromCLSID") hr = ::ProgIDFromCLSID(guid, &hostText);
            else if (name == "StringFromIID") hr = ::StringFromIID(guid, &hostText);
            else hr = ::StringFromCLSID(guid, &hostText);
        }
        if (SUCCEEDED(hr) && hostText) {
            const std::string text = asciiFromWide(hostText);
            const uint32_t guestText = allocate(uint32_t((text.size() + 1) * 2), true);
            writeUtf16(guestText, text, uint32_t(text.size() + 1));
            writeU32(a1, guestText);
            ::CoTaskMemFree(hostText);
        }
        ret = uint32_t(hr);
    } else if (name == "CoCreateInstance" || name == "OleCreate") {
        if (name == "CoCreateInstance" && stackArg(4)) writeU32(stackArg(4), 0);
        ret = 0x80004002u; // E_NOINTERFACE: host COM pointers cannot be executed as guest vtables.
    } else if (name == "OleRun" || name == "OleSave" || name == "OleSetMenuDescriptor" ||
               name == "OleDraw" || name == "OleSetContainedObject" ||
               name == "CreateOleAdviseHolder" || name == "ReadClassStm" || name == "WriteClassStm") {
        ret = 0x80004001u; // E_NOTIMPL until an interface/vtable bridge exists.
    } else if (name == "OleIsRunning") {
        ret = 0;
    } else {
        return false;
    }
    return true;
#else
    (void)a0; (void)a1; (void)a2; (void)a3;
    if (name == "CoTaskMemAlloc") ret = allocate(a0, false);
    else if (name == "CoTaskMemFree" || name == "CoUninitialize") ret = 0;
    else return false;
    return true;
#endif
}

bool SyntheticDllRuntime::dispatchOleAut32(const std::string& name,
                                           const GuestCallArgs& args,
                                           uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    auto allocBstr = [&](uint32_t source, uint32_t chars) -> uint32_t {
        if (!chars && source) chars = uint32_t(readUtf16(source, 65536).size());
        const uint32_t base = allocate(4 + chars * 2 + 2, true);
        writeU32(base, chars * 2);
        if (source && chars) copyGuest(base + 4, source, chars * 2);
        return base ? base + 4 : 0;
    };
    if (name == "SysAllocString") {
        ret = allocBstr(a0, 0);
    } else if (name == "SysAllocStringLen") {
        ret = allocBstr(a0, a1);
    } else if (name == "SysAllocStringByteLen") {
        const uint32_t base = allocate(4 + a1 + 2, true);
        writeU32(base, a1);
        if (a0 && a1) copyGuest(base + 4, a0, a1);
        ret = base ? base + 4 : 0;
    } else if (name == "SysFreeString") {
        if (a0 >= 4) allocationSizes_.erase(a0 - 4);
        ret = 0;
    } else if (name == "SysStringLen") {
        ret = a0 >= 4 ? readU32(a0 - 4) / 2 : 0;
    } else if (name == "SysStringByteLen") {
        ret = a0 >= 4 ? readU32(a0 - 4) : 0;
    } else if (name == "VariantClear") {
        if (a0) {
            const uint16_t empty = 0;
            uc_mem_write(uc_, a0, &empty, sizeof(empty));
        }
        ret = 0;
    } else if (name == "VariantChangeType") {
        if (a0 && a1) copyGuest(a0, a1, 16);
        ret = 0;
    } else if (name == "VarUI4FromStr") {
        const std::string value = readUtf16(a0, 128);
        char* end = nullptr;
        const uint32_t parsed = uint32_t(std::strtoul(value.c_str(), &end, 0));
        if (a3) writeU32(a3, parsed);
        ret = 0;
    } else if (name == "CreateErrorInfo" || name == "SetErrorInfo" ||
               name == "LoadRegTypeLib" || name == "LoadTypeLib" || name == "RegisterTypeLib") {
        ret = 0x80004001u;
    } else {
        return false;
    }
    return true;
}

void SyntheticDllRuntime::dispatch(const ExportEntry& entry) {
    auto& mutableEntry = exportsByAddress_[reg(UC_MIPS_REG_PC)];
    mutableEntry.calls++;
    const std::string name = mutableEntry.name.empty()
        ? ("#" + std::to_string(mutableEntry.ordinal))
        : mutableEntry.name;
    const uint32_t a0 = reg(UC_MIPS_REG_A0);
    const uint32_t a1 = reg(UC_MIPS_REG_A1);
    const uint32_t a2 = reg(UC_MIPS_REG_A2);
    const uint32_t a3 = reg(UC_MIPS_REG_A3);
    const uint32_t ra = reg(UC_MIPS_REG_RA);
    const GuestCallArgs args{a0, a1, a2, a3, ra};
    if (mutableEntry.calls <= 128) {
        spdlog::info("synthetic {}!{} call {} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} ra=0x{:08x}",
                     mutableEntry.moduleName, name, mutableEntry.calls, a0, a1, a2, a3, ra);
    }

    uint32_t ret = 1;
    if (mutableEntry.moduleName == "coredll.dll" &&
        (name == "CallWindowProcW" || name == "DispatchMessageW" || name == "SendMessageW")) {
        uint32_t wndProc = a0;
        uint32_t hwnd = a1;
        uint32_t msg = a2;
        uint32_t wParam = a3;
        uint32_t lParam = stackArg(4);
        if (name == "DispatchMessageW") {
            if (a0) {
                uc_mem_read(uc_, a0, &hwnd, sizeof(hwnd));
                uc_mem_read(uc_, a0 + 4, &msg, sizeof(msg));
                uc_mem_read(uc_, a0 + 8, &wParam, sizeof(wParam));
                uc_mem_read(uc_, a0 + 12, &lParam, sizeof(lParam));
            }
            auto it = windows_.find(hwnd);
            wndProc = it == windows_.end() ? 0 : it->second.wndProc;
        } else if (name == "SendMessageW") {
            auto it = windows_.find(a0);
            wndProc = it == windows_.end() ? 0 : it->second.wndProc;
            hwnd = a0;
            msg = a1;
            wParam = a2;
            lParam = a3;
        }
        if (!wndProc) {
            ret = 0;
            setReg(UC_MIPS_REG_V0, ret);
            return;
        }
        if (mutableEntry.calls <= 128) {
            spdlog::info("synthetic coredll.dll!{} transfer wndproc=0x{:08x} hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x}",
                         name, wndProc, hwnd, msg, wParam, lParam);
        }
        setReg(UC_MIPS_REG_A0, hwnd);
        setReg(UC_MIPS_REG_A1, msg);
        setReg(UC_MIPS_REG_A2, wParam);
        setReg(UC_MIPS_REG_A3, lParam);
        setReg(UC_MIPS_REG_PC, wndProc);
        return;
    }
    if (mutableEntry.moduleName == "coredll.dll" &&
        (dispatchHostWin32(name, args, ret) || dispatchGuestMemoryApi(name, args, ret))) {
        if (mutableEntry.calls <= 128) {
            spdlog::info("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, ret);
        }
        setReg(UC_MIPS_REG_V0, ret);
        return;
    }
    if (mutableEntry.moduleName == "coredll.dll") {
        lastError_ = 120; // ERROR_CALL_NOT_IMPLEMENTED
        ret = 0;
        if (mutableEntry.calls <= 128) {
            spdlog::warn("synthetic coredll.dll!{} unsupported by translate layer -> 0", name);
        }
        setReg(UC_MIPS_REG_V0, ret);
        return;
    }

    bool handled = false;
    if (sameModule(mutableEntry.moduleName, "winsock.dll")) {
        handled = dispatchWinsock(name, args, ret);
    } else if (sameModule(mutableEntry.moduleName, "commctrl.dll")) {
        ret = 1;
        handled = true;
    } else if (sameModule(mutableEntry.moduleName, "ole32.dll")) {
        handled = dispatchOle32(name, args, ret);
    } else if (sameModule(mutableEntry.moduleName, "oleaut32.dll")) {
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

    if (mutableEntry.calls <= 128) {
        spdlog::info("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, ret);
    }
    setReg(UC_MIPS_REG_V0, ret);
}
