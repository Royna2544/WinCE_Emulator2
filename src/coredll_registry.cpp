#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

std::string lowerRegistryAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
    }
    return text;
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

std::string normalizeRegistryValueName(std::string name) {
    return lowerRegistryAscii(std::move(name));
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

std::string registryValuePreview(const nlohmann::json& value) {
    const auto data = value.find("data");
    if (data == value.end()) return "<none>";
    if (data->is_string()) return data->get<std::string>();
    if (data->is_number_unsigned()) return std::to_string(data->get<uint32_t>());
    if (data->is_number_integer()) return std::to_string(data->get<int32_t>());
    if (data->is_array() && std::all_of(data->begin(), data->end(), [](const nlohmann::json& item) {
            return item.is_string();
        })) {
        std::string joined;
        for (const auto& item : *data) {
            if (!joined.empty()) joined += ";";
            joined += item.get<std::string>();
        }
        return joined;
    }
    if (data->is_array()) return "<binary " + std::to_string(data->size()) + " bytes>";
    return data->dump();
}

uint32_t registryTypeId(const nlohmann::json& value) {
    if (value.contains("type")) {
        if (value["type"].is_number_unsigned()) return value["type"].get<uint32_t>();
        if (value["type"].is_string()) {
            const std::string type = lowerRegistryAscii(value["type"].get<std::string>());
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

} // namespace

void SyntheticDllRuntime::registerCoredllRegistryExports(SyntheticModule& module) {
    struct CoreDllRegistry {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.registry",
                {
                    {0x01C7, {"RegCloseKey", Code::CoreDllRegCloseKey, &SyntheticDllRuntime::handleRegCloseKey}},
                    {0x01C8, {"RegCreateKeyExW", Code::CoreDllRegCreateKeyExW, &SyntheticDllRuntime::handleRegCreateKeyExW}},
                    {0x01C9, {"RegDeleteKeyW", Code::CoreDllRegDeleteKeyW, &SyntheticDllRuntime::handleRegDeleteKeyW}},
                    {0x01CA, {"RegDeleteValueW", Code::CoreDllRegDeleteValueW, &SyntheticDllRuntime::handleRegDeleteValueW}},
                    {0x01CB, {"RegEnumValueW", Code::CoreDllRegEnumValueW, &SyntheticDllRuntime::handleRegEnumValueW}},
                    {0x01CC, {"RegEnumKeyExW", Code::CoreDllRegEnumKeyExW, &SyntheticDllRuntime::handleRegEnumKeyExW}},
                    {0x01CD, {"RegOpenKeyExW", Code::CoreDllRegOpenKeyExW, &SyntheticDllRuntime::handleRegOpenKeyExW}},
                    {0x01CE, {"RegQueryInfoKeyW", Code::CoreDllRegQueryInfoKeyW, &SyntheticDllRuntime::handleRegQueryInfoKeyW}},
                    {0x01CF, {"RegQueryValueExW", Code::CoreDllRegQueryValueExW, &SyntheticDllRuntime::handleRegQueryValueExW}},
                    {0x01D0, {"RegSetValueExW", Code::CoreDllRegSetValueExW, &SyntheticDllRuntime::handleRegSetValueExW}},
                    {0x0480, {"RegFlushKey", Code::CoreDllRegFlushKey, &SyntheticDllRuntime::handleRegFlushKey}},
                },
            };
        }
    };

    const CoreDllRegistry registry;
    registerHandlers(module, registry.group());
}

bool SyntheticDllRuntime::handleRegCreateKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string subKey = readUtf16(args.a1, 1024);
    const auto path = registryPathFromHandle(args.a0, subKey);
    const uint32_t resultPtr = stackArg(7);
    const uint32_t dispositionPtr = stackArg(8);
    if (!path || !resultPtr) {
        ret = 87;
        spdlog::info("RegCreateKeyExW invalid hkey=0x{:08x} subkey=\"{}\" resultPtr=0x{:08x}",
                     args.a0, subKey, resultPtr);
    } else {
        const bool existed = registryKeyExists(*path);
        registryEnsureKey(*path);
        writeU32(resultPtr, makeRegistryHandle(*path));
        if (dispositionPtr) writeU32(dispositionPtr, existed ? 2 : 1);
        ret = 0;
        spdlog::info("RegCreateKeyExW path=\"{}\" existed={} -> {}", *path, existed, ret);
    }
    return true;
}

bool SyntheticDllRuntime::handleRegOpenKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string subKey = readUtf16(args.a1, 1024);
    const auto path = registryPathFromHandle(args.a0, subKey);
    const uint32_t resultPtr = stackArg(4);
    if (!path || !resultPtr) {
        ret = 87;
        spdlog::info("RegOpenKeyExW invalid hkey=0x{:08x} subkey=\"{}\" resultPtr=0x{:08x}",
                     args.a0, subKey, resultPtr);
    } else if (!registryKeyExists(*path)) {
        writeU32(resultPtr, 0);
        ret = 2;
        spdlog::info("RegOpenKeyExW miss path=\"{}\" -> {}", *path, ret);
    } else {
        const uint32_t handle = makeRegistryHandle(*path);
        writeU32(resultPtr, handle);
        ret = 0;
        spdlog::info("RegOpenKeyExW hit path=\"{}\" handle=0x{:08x}", *path, handle);
    }
    return true;
}

bool SyntheticDllRuntime::handleRegQueryValueExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, {});
    const std::string valueName = readUtf16(args.a1, 1024);
    const nlohmann::json* value = path ? registryValue(*path, valueName) : nullptr;
    const uint32_t typePtr = args.a3;
    const uint32_t dataPtr = stackArg(4);
    const uint32_t sizePtr = stackArg(5);
    if (!path || !sizePtr) {
        ret = 87;
        spdlog::info("RegQueryValueExW invalid hkey=0x{:08x} value=\"{}\" sizePtr=0x{:08x}",
                     args.a0, valueName, sizePtr);
    } else if (!value) {
        const uint32_t capacity = readU32(sizePtr);
        ret = 2;
        spdlog::info("RegQueryValueExW miss path=\"{}\" value=\"{}\" typePtr=0x{:08x} dataPtr=0x{:08x} sizePtr=0x{:08x} capacity={} -> {}",
                     *path, valueName, typePtr, dataPtr, sizePtr, capacity, ret);
    } else {
        const uint32_t type = registryTypeId(*value);
        const std::vector<uint8_t> bytes = registryValueBytes(*value);
        const uint32_t capacity = readU32(sizePtr);
        if (typePtr) writeU32(typePtr, type);
        writeU32(sizePtr, uint32_t(bytes.size()));
        if (!dataPtr) ret = 0;
        else if (capacity < bytes.size()) ret = 234;
        else {
            if (!bytes.empty()) uc_mem_write(uc_, dataPtr, bytes.data(), bytes.size());
            ret = 0;
        }
        spdlog::info("RegQueryValueExW hit path=\"{}\" value=\"{}\" type={} bytes={} capacity={} dataPtr=0x{:08x} -> {} data=\"{}\"",
                     *path, valueName, registryTypeName(type), bytes.size(), capacity, dataPtr, ret,
                     registryValuePreview(*value));
    }
    return true;
}

bool SyntheticDllRuntime::handleRegSetValueExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, {});
    if (!path || !registryKeyExists(*path)) {
        ret = 6;
        spdlog::info("RegSetValueExW invalid hkey=0x{:08x} value=\"{}\" -> {}",
                     args.a0, readUtf16(args.a1, 1024), ret);
    } else {
        const std::string valueName = normalizeRegistryValueName(readUtf16(args.a1, 1024));
        registry_["keys"][*path]["values"][valueName] =
            registryJsonFromBytes(args.a3, stackArg(4), stackArg(5));
        registryDirty_ = true;
        flushRegistry();
        ret = 0;
        spdlog::info("RegSetValueExW path=\"{}\" value=\"{}\" type={} size={} -> {}",
                     *path, valueName, registryTypeName(args.a3), stackArg(5), ret);
    }
    return true;
}

bool SyntheticDllRuntime::handleRegDeleteValueW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, {});
    if (!path || !registryKeyExists(*path)) {
        ret = 6;
    } else {
        const std::string valueName = normalizeRegistryValueName(readUtf16(args.a1, 1024));
        auto& values = registry_["keys"][*path]["values"];
        ret = values.erase(valueName) ? 0 : 2;
        if (!ret) {
            registryDirty_ = true;
            flushRegistry();
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleRegDeleteKeyW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, readUtf16(args.a1, 1024));
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
    return true;
}

bool SyntheticDllRuntime::handleRegEnumKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, {});
    const std::vector<std::string> children = path ? registryChildNames(*path) : std::vector<std::string>{};
    if (!path || !args.a2 || !args.a3) ret = 87;
    else if (args.a1 >= children.size()) ret = 259;
    else {
        const uint32_t capacity = readU32(args.a3);
        const std::string& child = children[args.a1];
        if (capacity <= child.size()) {
            writeU32(args.a3, uint32_t(child.size() + 1));
            ret = 234;
        } else {
            writeUtf16(args.a2, child, capacity);
            writeU32(args.a3, uint32_t(child.size()));
            ret = 0;
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleRegEnumValueW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = handleRegEnumValueW(args.a0, args.a1, args.a2, args.a3);
    return true;
}

bool SyntheticDllRuntime::handleRegQueryInfoKeyW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const auto path = registryPathFromHandle(args.a0, {});
    if (!path || !registryKeyExists(*path)) ret = 6;
    else {
        const auto children = registryChildNames(*path);
        const auto& values = registry_["keys"][*path]["values"];
        if (stackArg(4)) writeU32(stackArg(4), uint32_t(children.size()));
        if (stackArg(6)) writeU32(stackArg(6), uint32_t(values.size()));
        ret = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleRegFlushKey(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    flushRegistry();
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleRegCloseKey(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (registryRootName(args.a0).empty()) {
        registryHandles_.erase(args.a0);
        guestHandles_.erase(args.a0);
    }
    ret = 0;
    return true;
}
