#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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
    // coredll.lib /LINKERMEMBER table. The function signatures are the CE
    // winbase/kfuncs declarations for the APIs implemented in dispatch().
    // The SDK import libraries expose more than one ordinal dialect. These
    // low ordinals are the values observed in the real MFC/ATL DLL import
    // thunks, where #2-#5 are the critical-section primitives.
    registerExport(module, 0x0002, "InitializeCriticalSection");
    registerExport(module, 0x0003, "DeleteCriticalSection");
    registerExport(module, 0x0004, "EnterCriticalSection");
    registerExport(module, 0x0005, "LeaveCriticalSection");
    registerExport(module, 0x0006, "InitializeCriticalSection");
    registerExport(module, 0x0007, "DeleteCriticalSection");
    registerExport(module, 0x0008, "EnterCriticalSection");
    registerExport(module, 0x0009, "LeaveCriticalSection");
    registerExport(module, 0x000F, "LookupSyntheticHandle");
    registerExport(module, 0x0010, "SetSyntheticHandle");
    registerExport(module, 0x0011, "InterlockedIncrement");
    registerExport(module, 0x0012, "InterlockedDecrement");
    registerExport(module, 0x0013, "InterlockedExchange");
    registerExport(module, 0x0014, "InterlockedExchangeAdd");
    registerExport(module, 0x0015, "InterlockedCompareExchange");
    registerExport(module, 0x001A, "TlsGetValue");
    registerExport(module, 0x001B, "TlsSetValue");
    registerExport(module, 0x001D, "GetVersionExW");
    registerExport(module, 0x0021, "LocalAlloc");
    registerExport(module, 0x0022, "LocalReAlloc");
    registerExport(module, 0x0023, "GetLocalTime");
    registerExport(module, 0x0024, "LocalFree");
    registerExport(module, 0x0025, "GetSystemTime");
    registerExport(module, 0x0031, "LocalAlloc");
    registerExport(module, 0x0033, "LocalReAlloc");
    registerExport(module, 0x0034, "LocalSize");
    registerExport(module, 0x0035, "LocalFree");
    registerExport(module, 0x0041, "HeapCreate");
    registerExport(module, 0x0043, "HeapDestroy");
    registerExport(module, 0x0045, "HeapAlloc");
    registerExport(module, 0x0048, "HeapReAlloc");
    registerExport(module, 0x004A, "HeapSize");
    registerExport(module, 0x004C, "HeapFree");
    registerExport(module, 0x004E, "GetProcessHeap");
    registerExport(module, 0x0057, "wcscpy");
    registerExport(module, 0x0059, "wcslen");
    registerExport(module, 0x005B, "wcsncmp");
    registerExport(module, 0x006C, "strtol");
    registerExport(module, 0x0077, "_stricmp");
    registerExport(module, 0x0078, "_strnicmp");
    registerExport(module, 0x00A3, "CopyRect");
    registerExport(module, 0x00A5, "InflateRect");
    registerExport(module, 0x00A7, "IsRectEmpty");
    registerExport(module, 0x00A9, "PtInRect");
    registerExport(module, 0x00F2, "CreateFileW");
    registerExport(module, 0x00F4, "ReadFile");
    registerExport(module, 0x00F5, "WriteFile");
    registerExport(module, 0x00F6, "GetFileSize");
    registerExport(module, 0x00F7, "SetFilePointer");
    registerExport(module, 0x011E, "IsDBCSLeadByteEx");
    registerExport(module, 0x011F, "iswctype");
    registerExport(module, 0x0208, "SyntheticHandleCall");
    registerExport(module, 0x07D0, "MfcStartupProbe");
    registerExport(module, 0x0288, "GetLastError");
    registerExport(module, 0x0289, "SetLastError");
    registerExport(module, 0x0290, "VirtualAlloc");
    registerExport(module, 0x0291, "VirtualFree");
    registerExport(module, 0x0294, "LoadLibraryW");
    registerExport(module, 0x0297, "GetProcAddressW");
    registerExport(module, 0x02A0, "GetTickCount");
    registerExport(module, 0x02A2, "GetModuleFileNameW");
    registerExport(module, 0x02A3, "GetModuleHandleW");
    registerExport(module, 0x02A4, "QueryPerformanceCounter");
    registerExport(module, 0x02A5, "QueryPerformanceFrequency");
    registerExport(module, 0x02A8, "OutputDebugStringW");
    registerExport(module, 0x02BA, "CloseHandle");
    registerExport(module, 0x02BC, "CreateMutexW");
    registerExport(module, 0x02BD, "ReleaseMutex");
    registerExport(module, 0x02C4, "GetProcAddressA");
    registerExport(module, 0x02C7, "TryEnterCriticalSection");
    registerExport(module, 0x0416, "memcpy");
    registerExport(module, 0x0417, "memset");
    registerExport(module, 0x0269, "CreateEventW");
    registerExport(module, 0x026E, "Sleep");
    registerExport(module, 0x026F, "WaitForSingleObject");
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
    if (mutableEntry.calls <= 128) {
        spdlog::info("synthetic {}!{} call {} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} ra=0x{:08x}",
                     mutableEntry.moduleName, name, mutableEntry.calls, a0, a1, a2, a3, ra);
    }

    uint32_t ret = 1;

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
        if (a0 && a1 && a2) {
            std::vector<uint8_t> bytes(a2);
            if (uc_mem_read(uc_, a1, bytes.data(), bytes.size()) == UC_ERR_OK) {
                uc_mem_write(uc_, a0, bytes.data(), bytes.size());
            }
        }
        ret = a0;
    } else if (name == "memset") {
        if (a0 && a2) {
            std::vector<uint8_t> bytes(a2, uint8_t(a1 & 0xffu));
            uc_mem_write(uc_, a0, bytes.data(), bytes.size());
        }
        ret = a0;
    }
    else if (name == "LocalAlloc") ret = allocate(a1, (a0 & 0x0040u) != 0);
    else if (name == "LocalFree") ret = 0;
    else if (name == "LocalSize" || name == "HeapSize") {
        auto it = allocationSizes_.find(name == "HeapSize" ? a2 : a0);
        ret = it == allocationSizes_.end() ? 0 : it->second;
    } else if (name == "LocalReAlloc") {
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
    } else if (name == "CloseHandle" || name == "ReleaseMutex" || name == "SetEventData") ret = 1;
    else if (name == "CreateEventW" || name == "CreateMutexW") ret = nextHandle_++;
    else if (name == "WaitForSingleObject") ret = 0; // WAIT_OBJECT_0
    else if (name == "OutputDebugStringW") {
        spdlog::info("OutputDebugStringW: {}", readUtf16(a0));
        ret = 0;
    } else if (name == "LoadLibraryW" || name == "GetModuleHandleW") {
        const std::string dll = lowerAscii(readUtf16(a0));
        ret = dll.empty() || dll == "coredll.dll" ? 0x70000000 : 0;
    } else if (name == "GetProcAddressA" || name == "GetProcAddressW") ret = 0;
    else if (name == "GetModuleFileNameW") ret = 0;
    else if (name == "CreateFileW") {
        lastError_ = 2; // ERROR_FILE_NOT_FOUND until host file mapping exists.
        ret = 0xffffffffu;
    } else if (name == "ReadFile" || name == "WriteFile") {
        writeU32(a3, 0);
        lastError_ = 6; // ERROR_INVALID_HANDLE
        ret = 0;
    } else if (name == "GetFileSize" || name == "SetFilePointer") {
        lastError_ = 6;
        ret = 0xffffffffu;
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
