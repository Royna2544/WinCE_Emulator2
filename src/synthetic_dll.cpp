#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
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

bool sameModule(std::string name, std::string_view wanted) {
    name = lowerAscii(std::move(name));
    return name == wanted;
}
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
            registerExport(*module, 0x0004, "CoInitializeEx");
            registerExport(*module, 0x0005, "CoUninitialize");
            registerExport(*module, 0x0006, "CoCreateInstance");
            registerExport(*module, 0x000B, "CoTaskMemAlloc");
            registerExport(*module, 0x000C, "CoTaskMemRealloc");
            registerExport(*module, 0x000D, "CoTaskMemFree");
            registerExport(*module, 0x000E, "CoTaskMemSize");
            registerExport(*module, 0x000F, "StringFromCLSID");
            registerExport(*module, 0x0010, "CLSIDFromString");
            registerExport(*module, 0x0011, "ProgIDFromCLSID");
            registerExport(*module, 0x0012, "CLSIDFromProgID");
            registerExport(*module, 0x0013, "StringFromGUID2");
            registerExport(*module, 0x0014, "StringFromIID");
            registerExport(*module, 0x001B, "CoCreateGuid");
            registerExport(*module, 0x0020, "ReadClassStm");
            registerExport(*module, 0x0021, "WriteClassStm");
            registerExport(*module, 0x001C, "OleCreate");
            registerExport(*module, 0x001D, "OleSave");
            registerExport(*module, 0x001E, "OleRun");
            registerExport(*module, 0x001F, "OleIsRunning");
            registerExport(*module, 0x0025, "CreateOleAdviseHolder");
            registerExport(*module, 0x0026, "OleSetMenuDescriptor");
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
    registerExport(module, 0x0017, "GetLocalTime");
    registerExport(module, 0x0019, "GetSystemTime");
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
    registerExport(module, 0x003B, "wcschr");
    registerExport(module, 0x003D, "wcscpy");
    registerExport(module, 0x003E, "wcscspn");
    registerExport(module, 0x003F, "wcslen");
    registerExport(module, 0x0041, "wcsncmp");
    registerExport(module, 0x0045, "wcsrchr");
    registerExport(module, 0x004A, "_wcsdup");
    registerExport(module, 0x0058, "GlobalMemoryStatus");
    registerExport(module, 0x0060, "CopyRect");
    registerExport(module, 0x0062, "InflateRect");
    registerExport(module, 0x0064, "IsRectEmpty");
    registerExport(module, 0x0066, "PtInRect");
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
    registerExport(module, 0x011D, "CallWindowProcW");
    registerExport(module, 0x0195, "waveInUnprepareHeader");
    registerExport(module, 0x0196, "waveInAddBuffer");
    registerExport(module, 0x0199, "waveInReset");
    registerExport(module, 0x019C, "waveInMessage");
    registerExport(module, 0x019D, "waveInOpen");
    registerExport(module, 0x01BE, "WNetConnectionDialog1W");
    registerExport(module, 0x01C2, "WNetGetUniversalNameW");
    registerExport(module, 0x01C3, "WNetGetUserW");
    registerExport(module, 0x01EF, "CreateEventW");
    registerExport(module, 0x01F0, "Sleep");
    registerExport(module, 0x01F1, "WaitForSingleObject");
    registerExport(module, 0x0204, "GetLastError");
    registerExport(module, 0x0205, "SetLastError");
    registerExport(module, 0x0208, "TlsCall");
    registerExport(module, 0x020C, "VirtualAlloc");
    registerExport(module, 0x020D, "VirtualFree");
    registerExport(module, 0x0210, "LoadLibraryW");
    registerExport(module, 0x0212, "GetProcAddressW");
    registerExport(module, 0x0214, "FindResourceW");
    registerExport(module, 0x0217, "GetTickCount");
    registerExport(module, 0x0219, "GetModuleFileNameW");
    registerExport(module, 0x021A, "QueryPerformanceCounter");
    registerExport(module, 0x021B, "QueryPerformanceFrequency");
    registerExport(module, 0x021D, "OutputDebugStringW");
    registerExport(module, 0x0229, "CloseHandle");
    registerExport(module, 0x022B, "CreateMutexW");
    registerExport(module, 0x022C, "ReleaseMutex");
    registerExport(module, 0x02CD, "GetVersionExW");
    registerExport(module, 0x02DD, "LocalAllocTrace");
    registerExport(module, 0x02DE, "GetCursorPos");
    registerExport(module, 0x034B, "RemoveMenu");
    registerExport(module, 0x034E, "LoadMenuW");
    registerExport(module, 0x0350, "CheckMenuItem");
    registerExport(module, 0x0351, "CheckMenuRadioItem");
    registerExport(module, 0x036A, "LoadStringW");
    registerExport(module, 0x036E, "GetClassInfoW");
    registerExport(module, 0x0375, "GetSystemMetrics");
    registerExport(module, 0x0376, "IsWindowVisible");
    registerExport(module, 0x0379, "GetSysColor");
    registerExport(module, 0x037B, "RegisterWindowMessageW");
    registerExport(module, 0x037C, "RegisterTaskBar");
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
    registerExport(module, 0x05D3, "InterlockedExchangeAdd");
    registerExport(module, 0x05D4, "InterlockedCompareExchange");
    registerExport(module, 0x05E3, "RegisterDesktop");
    registerExport(module, 0x05EF, "GlobalAddAtomW");
    registerExport(module, 0x05F0, "GlobalDeleteAtom");
    registerExport(module, 0x05F1, "GlobalFindAtomW");
    registerExport(module, 0x0635, "mixerGetControlDetails");
    registerExport(module, 0x0646, "RemoteHeapFree");
    registerExport(module, 0x07D0, "_setjmp");
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
#if defined(_WIN32)
    if (it->second.hostValue) {
        if (it->second.kind == GuestHandle::Kind::HostFind) {
            FindClose(reinterpret_cast<HANDLE>(it->second.hostValue));
        } else {
            CloseHandle(reinterpret_cast<HANDLE>(it->second.hostValue));
        }
    }
#endif
    guestHandles_.erase(it);
    lastError_ = 0;
    return 1;
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
        ret = makeGuestHandle({GuestHandle::Kind::Pseudo, 0, 0});
    } else if (name == "HeapDestroy") {
        ret = 1;
    } else if (name == "GetProcessHeap") {
        ret = 1;
    } else if (name == "HeapAlloc") {
        ret = allocate(a2, (a1 & 0x00000008u) != 0);
    } else if (name == "HeapReAlloc") {
        ret = allocate(a3, (a1 & 0x00000008u) != 0);
    } else if (name == "HeapFree") {
        ret = 1;
    } else if (name == "RemoteHeapFree") {
        ret = copyGuest(a0, a2, 0x38) ? a0 : 1;
    } else if (name == "VirtualAlloc") {
        ret = allocate(a1, true);
    } else if (name == "VirtualFree") {
        ret = 1;
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
            writeU32(a0 + 8, status.dwTotalPhys);
            writeU32(a0 + 12, status.dwAvailPhys);
            writeU32(a0 + 16, status.dwTotalPageFile);
            writeU32(a0 + 20, status.dwAvailPageFile);
            writeU32(a0 + 24, status.dwTotalVirtual);
            writeU32(a0 + 28, status.dwAvailVirtual);
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
        ret = makeGuestHandle({GuestHandle::Kind::Pseudo, 0, 0});
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
        ret = makeGuestHandle({GuestHandle::Kind::Pseudo, 0, 0});
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
        ret = uint32_t(GetSystemMetrics(int(a0)));
#else
        if (a0 == 0) ret = 800;
        else if (a0 == 1) ret = 480;
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
    } else if (name == "IsWindowVisible") {
        lastError_ = 1400;
        ret = 0;
    } else if (name == "LoadMenuW" || name == "RemoveMenu" ||
               name == "CheckMenuItem" || name == "CheckMenuRadioItem" ||
               name == "GetClassInfoW") {
        lastError_ = 1401;
        ret = 0;
    } else if (name == "FindResourceW" || name == "LoadStringW") {
        lastError_ = 1813;
        ret = 0;
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
    if (mutableEntry.moduleName == "coredll.dll" && name == "CallWindowProcW") {
        if (!a0) {
            ret = 0;
            setReg(UC_MIPS_REG_V0, ret);
            return;
        }
        const uint32_t lParam = stackArg(4);
        if (mutableEntry.calls <= 128) {
            spdlog::info("synthetic coredll.dll!CallWindowProcW transfer wndproc=0x{:08x} hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x}",
                         a0, a1, a2, a3, lParam);
        }
        setReg(UC_MIPS_REG_A0, a1);
        setReg(UC_MIPS_REG_A1, a2);
        setReg(UC_MIPS_REG_A2, a3);
        setReg(UC_MIPS_REG_A3, lParam);
        setReg(UC_MIPS_REG_PC, a0);
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

    if (mutableEntry.moduleName == "WINSOCK.dll") {
        if (name == "WSAStartup") {
            if (a1) {
                std::array<uint8_t, 400> data{};
                uc_mem_write(uc_, a1, data.data(), data.size());
            }
            ret = 0;
        } else if (name == "WSACleanup" || name == "closesocket") ret = 0;
        else if (name == "socket" || name == "accept") ret = nextHandle_++;
        else if (name == "htons" || name == "ntohs") ret = ((a0 & 0xffu) << 8) | ((a0 >> 8) & 0xffu);
        else if (name == "htonl" || name == "ntohl") {
            ret = ((a0 & 0xffu) << 24) | ((a0 & 0xff00u) << 8) |
                  ((a0 >> 8) & 0xff00u) | ((a0 >> 24) & 0xffu);
        } else {
            lastError_ = 10035; // WSAEWOULDBLOCK-ish placeholder.
            ret = 0xffffffffu;
        }
    } else if (mutableEntry.moduleName == "commctrl.dll") {
        ret = 1;
    } else if (mutableEntry.moduleName == "ole32.dll" ||
               mutableEntry.moduleName == "OLEAUT32.dll") {
        if (name == "CoTaskMemAlloc") ret = allocate(a0, false);
        else if (name == "CoTaskMemRealloc") ret = allocate(a1, false);
        else if (name == "CoTaskMemFree") ret = 0;
        else if (name == "CoTaskMemSize") {
            auto it = allocationSizes_.find(a0);
            ret = it == allocationSizes_.end() ? 0 : it->second;
        } else if (name == "SysAllocString" || name == "SysAllocStringLen") ret = allocate((a1 ? a1 : 64) * 2 + 8, true);
        else if (name == "SysFreeString") ret = 0;
        else if (name == "SysStringLen" || name == "SysStringByteLen") ret = 0;
        else if (name == "CoCreateInstance" || name == "OleCreate") ret = 0x80004001u; // E_NOTIMPL
        else ret = 0; // S_OK-style default for COM init/uninit helpers.
    } else if (name == "SystemStarted") ret = 1;
    else if (name == "SystemMemoryLow") ret = 0;
    else if (name == "ExitThread") {
        ret = 0;
        uc_emu_stop(uc_);
    }
    else if (name == "LookupSyntheticHandle") {
        auto it = syntheticHandleValues_.find(a0);
        ret = it == syntheticHandleValues_.end() ? 0 : it->second;
    } else if (name == "SetSyntheticHandle") {
        syntheticHandleValues_[a0] = a1;
        ret = 1;
    } else if (name == "MfcStartupProbe") {
        ret = 0;
    } else if (name == "FileTimeToLocalFileTime") {
        if (a0 && a1) {
            std::array<uint8_t, 8> fileTime{};
            if (uc_mem_read(uc_, a0, fileTime.data(), fileTime.size()) == UC_ERR_OK) {
                uc_mem_write(uc_, a1, fileTime.data(), fileTime.size());
            }
        }
        ret = 1;
    } else if (name == "SyntheticHandleCall") ret = nextHandle_++;
    else if (name == "memcpy") {
        copyGuest(a0, a1, a2);
        ret = a0;
    } else if (name == "memset") {
        fillGuest(a0, uint8_t(a1 & 0xffu), a2);
        ret = a0;
    }
    else if (name == "LocalAlloc") ret = allocate(a1, (a0 & 0x0040u) != 0);
    else if (name == "LocalAllocTrace") ret = allocate(a1 ? a1 : 1, false);
    else if (name == "LocalFree") ret = 0;
    else if (name == "LocalSize" || name == "HeapSize") {
        auto it = allocationSizes_.find(name == "HeapSize" ? a2 : a0);
        ret = it == allocationSizes_.end() ? 0 : it->second;
    } else if (name == "LocalReAlloc" || name == "RemoteLocalReAlloc") {
        const uint32_t oldSize = allocationSizes_.count(a0) ? allocationSizes_[a0] : 0;
        ret = allocate(a1, (a2 & 0x0040u) != 0);
        if (a0 && ret && oldSize) {
            std::vector<uint8_t> bytes(std::min(oldSize, a1));
            uc_mem_read(uc_, a0, bytes.data(), bytes.size());
            uc_mem_write(uc_, ret, bytes.data(), bytes.size());
        }
    } else if (name == "HeapCreate") ret = nextHandle_++;
    else if (name == "HeapDestroy") ret = 1;
    else if (name == "GetProcessHeap") ret = 1;
    else if (name == "HeapAlloc") ret = allocate(a2, (a1 & 0x00000008u) != 0);
    else if (name == "HeapReAlloc") {
        ret = allocate(a3, (a1 & 0x00000008u) != 0);
    } else if (name == "HeapFree") ret = 1;
    else if (name == "RemoteHeapFree") {
        // SDK name/ordinal is authoritative, but this target calls the ordinal
        // with a CRT object-copy shape during startup. Preserve that evidence
        // here and in TODO.md instead of relabeling the export.
        ret = copyGuest(a0, a2, 0x38) ? a0 : 1;
    }
    else if (name == "VirtualAlloc") ret = allocate(a1, true);
    else if (name == "VirtualFree") ret = 1;
    else if (name == "operator_new" || name == "operator_new_nothrow" ||
             name == "operator_vector_new" || name == "operator_vector_new_nothrow") {
        ret = allocate(a0, false);
    } else if (name == "operator_delete" || name == "operator_delete_nothrow" ||
               name == "operator_vector_delete" || name == "operator_vector_delete_nothrow") {
        ret = 0;
    } else if (name == "TlsGetValue") ret = tlsValues_[a0];
    else if (name == "TlsSetValue") {
        tlsValues_[a0] = a1;
        ret = 1;
    } else if (name == "TlsCall") {
        ret = 0;
    } else if (name == "GetLastError") ret = lastError_;
    else if (name == "SetLastError") {
        lastError_ = a0;
        ret = 0;
    } else if (name == "GetTickCount") ret = uint32_t(++tick_ * 16);
    else if (name == "Sleep") ret = 0;
    else if (name == "QueryPerformanceFrequency") {
        writeU32(a0, 10000000u);
        writeU32(a0 + 4, 0);
        ret = 1;
    } else if (name == "QueryPerformanceCounter") {
        const uint64_t value = ++tick_ * 160000;
        writeU32(a0, uint32_t(value));
        writeU32(a0 + 4, uint32_t(value >> 32));
        ret = 1;
    } else if (name == "GetLocalTime" || name == "GetSystemTime") {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        if (name == "GetSystemTime") gmtime_s(&tm, &now);
        else localtime_s(&tm, &now);
#else
        if (name == "GetSystemTime") tm = *std::gmtime(&now);
        else tm = *std::localtime(&now);
#endif
        const std::array<uint16_t, 8> st = {
            uint16_t(tm.tm_year + 1900), uint16_t(tm.tm_mon + 1),
            uint16_t(tm.tm_wday), uint16_t(tm.tm_mday), uint16_t(tm.tm_hour),
            uint16_t(tm.tm_min), uint16_t(tm.tm_sec), 0,
        };
        if (a0) uc_mem_write(uc_, a0, st.data(), st.size() * sizeof(uint16_t));
        ret = 0;
    } else if (name == "GetVersionExW") {
        writeU32(a0 + 4, 4);
        writeU32(a0 + 8, 20);
        writeU32(a0 + 12, 0);
        writeU32(a0 + 16, 3);
        ret = 1;
    } else if (name == "InitializeCriticalSection" || name == "DeleteCriticalSection" ||
               name == "EnterCriticalSection" || name == "LeaveCriticalSection") {
        ret = 0;
    } else if (name == "TryEnterCriticalSection") ret = 1;
    else if (name == "InterlockedIncrement" || name == "InterlockedDecrement") {
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
    } else if (name == "CloseHandle") ret = closeGuestHandle(a0);
    else if (name == "ReleaseMutex") {
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
    } else if (name == "SetEventData") ret = 1;
    else if (name == "CreateEventW") {
#if defined(_WIN32)
        HANDLE host = CreateEventW(nullptr, a1 != 0, a2 != 0, nullptr);
        if (host) ret = makeGuestHandle({GuestHandle::Kind::HostEvent, reinterpret_cast<uintptr_t>(host), 0});
        else {
            ret = 0;
            lastError_ = GetLastError();
        }
#else
        ret = makeGuestHandle({GuestHandle::Kind::Pseudo, 0, 0});
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
        ret = makeGuestHandle({GuestHandle::Kind::Pseudo, 0, 0});
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
            ret = handle ? 0 : 0xffffffffu; // WAIT_OBJECT_0 or WAIT_FAILED
            if (!handle) lastError_ = 6;
        }
    }
    else if (name == "CheckMenuRadioItem") {
        ret = fillGuest(a0, uint8_t(a1 & 0xffu), a2) ? a0 : 0;
    } else if (name == "CheckMenuItem") {
        ret = copyGuest(a0, a1, a2) ? a0 : 0xffffffffu;
    } else if (name == "LoadMenuW") {
        ret = copyGuest(a0, a1, a2) ? a0 : nextHandle_++;
    } else if (name == "RegisterTaskBar") {
        ret = a0 && a0 < 0x01000000u ? allocate(a0, false) : 0;
    } else if (name == "RegisterDesktop") {
        std::string value = readAscii(a1);
        const uint32_t buffer = allocate(uint32_t(value.size() + 1), true);
        writeAscii(buffer, value);
        if (a0) {
            std::array<uint8_t, 0x28> object{};
            uc_mem_write(uc_, a0, object.data(), object.size());
            writeU32(a0, buffer);
            writeU32(a0 + 4, uint32_t(value.size()));
            writeU32(a0 + 8, uint32_t(value.size()));
        }
        ret = a0;
    } else if (name == "RegisterWindowMessageW") ret = 0;
    else if (name == "RemoveMenu") ret = 0;
    else if (name == "GlobalAddAtomW") {
        const std::string atomName = lowerAscii(readUtf16(a0));
        if (atomName.empty()) {
            lastError_ = 87; // ERROR_INVALID_PARAMETER
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
    } else if (name == "GetAPIAddress") ret = 0;
    else if (name == "GetCRTFlags") ret = 0;
    else if (name == "GetCRTStorageEx") ret = allocate(0x100, true);
    else if (name == "WNetGetUserW") {
        const std::string user = "User";
        if (a2) writeU32(a2, uint32_t(user.size() + 1));
        if (a1) writeUtf16(a1, user, a2 ? std::max<uint32_t>(1, uint32_t(user.size() + 1)) : 32);
        ret = 0; // NO_ERROR
    } else if (name == "WNetGetUniversalNameW" || name == "WNetConnectionDialog1W") {
        lastError_ = 1200; // ERROR_BAD_DEVICE / no network provider.
        ret = 1200;
    } else if (name == "waveInOpen" || name == "waveInReset" || name == "waveInAddBuffer" ||
               name == "waveInUnprepareHeader" || name == "waveInMessage" ||
               name == "mixerGetControlDetails") {
        ret = 2; // MMSYSERR_NODRIVER
    }
    else if (name == "OutputDebugStringW") {
        spdlog::info("OutputDebugStringW: {}", readUtf16(a0));
        ret = 0;
    } else if (name == "LoadLibraryW" || name == "GetModuleHandleW") {
        const std::string dll = lowerAscii(readUtf16(a0));
        ret = dll.empty() || dll == "coredll.dll" ? 0x70000000 : 0;
    } else if (name == "GetProcAddressA" || name == "GetProcAddressW") ret = 0;
    else if (name == "GetModuleFileNameW") {
        ret = writeUtf16(a1, mainModulePath_, a2);
        lastError_ = ret ? 0 : 122; // ERROR_INSUFFICIENT_BUFFER for tiny/null buffers.
    }
    else if (name == "CreateFileW") {
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
    } else if (name == "ReadFile" || name == "WriteFile") {
        auto* handle = lookupGuestHandle(a0);
        writeU32(a3, 0);
        if (!handle || handle->kind != GuestHandle::Kind::HostFile || !handle->hostValue) {
            lastError_ = 6; // ERROR_INVALID_HANDLE
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
    } else if (name == "IsDBCSLeadByteEx" || name == "iswctype") ret = 0;

    if (mutableEntry.calls <= 128) {
        spdlog::info("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, ret);
    }
    setReg(UC_MIPS_REG_V0, ret);
}
