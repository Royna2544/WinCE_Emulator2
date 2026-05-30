#include "synthetic_dll.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
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

std::string normalizeRegistrySubKey(std::string key) {
    std::replace(key.begin(), key.end(), '/', '\\');
    while (!key.empty() && (key.front() == '\\' || key.front() == ' ')) key.erase(key.begin());
    while (!key.empty() && (key.back() == '\\' || key.back() == ' ')) key.pop_back();
    return lowerRegistryAscii(std::move(key));
}

std::string normalizeRegistryPath(std::string path) {
    path = normalizeRegistrySubKey(std::move(path));
    auto replaceRoot = [&](std::string_view from, std::string_view to) {
        if (path == from) {
            path = std::string(to);
        } else if (path.rfind(std::string(from) + "\\", 0) == 0) {
            path = std::string(to) + path.substr(from.size());
        }
    };
    replaceRoot("hkey_classes_root", "hkcr");
    replaceRoot("hkey_current_user", "hkcu");
    replaceRoot("hkey_local_machine", "hklm");
    replaceRoot("hkey_users", "hku");
    return path;
}

std::string pathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::u8string utf8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
#else
    return path.string();
#endif
}

void normalizeRegistryDatabase(nlohmann::json& registry) {
    if (!registry.is_object()) registry = nlohmann::json::object();
    nlohmann::json normalizedKeys = nlohmann::json::object();
    const auto keys = registry.find("keys");
    if (keys != registry.end() && keys->is_object()) {
        for (auto it = keys->begin(); it != keys->end(); ++it) {
            const std::string path = normalizeRegistryPath(it.key());
            if (path.empty()) continue;
            nlohmann::json key = it.value().is_object() ? it.value() : nlohmann::json::object();
            const nlohmann::json* sourceValues = nullptr;
            const auto values = key.find("values");
            if (values != key.end() && values->is_object()) {
                sourceValues = &*values;
            } else if (key.is_object()) {
                sourceValues = &key;
            }
            nlohmann::json normalizedValues = nlohmann::json::object();
            if (sourceValues) {
                for (auto valueIt = sourceValues->begin(); valueIt != sourceValues->end(); ++valueIt) {
                    normalizedValues[normalizeRegistryValueName(valueIt.key())] = valueIt.value();
                }
            }
            key["values"] = std::move(normalizedValues);
            if (normalizedKeys.contains(path)) {
                auto& existingValues = normalizedKeys[path]["values"];
                for (auto valueIt = key["values"].begin(); valueIt != key["values"].end(); ++valueIt) {
                    existingValues[valueIt.key()] = valueIt.value();
                }
            } else {
                normalizedKeys[path] = std::move(key);
            }
        }
    }
    registry["keys"] = std::move(normalizedKeys);
}

void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint <= 0x7f) {
        out.push_back(char(codePoint));
    } else if (codePoint <= 0x7ff) {
        out.push_back(char(0xc0 | (codePoint >> 6)));
        out.push_back(char(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
        out.push_back(char(0xe0 | (codePoint >> 12)));
        out.push_back(char(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(char(0x80 | (codePoint & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (codePoint >> 18)));
        out.push_back(char(0x80 | ((codePoint >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((codePoint >> 6) & 0x3f)));
        out.push_back(char(0x80 | (codePoint & 0x3f)));
    }
}

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string result;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t codePoint = 0;
        size_t extra = 0;
        if (c < 0x80) {
            codePoint = c;
        } else if ((c & 0xe0) == 0xc0) {
            codePoint = c & 0x1f;
            extra = 1;
        } else if ((c & 0xf0) == 0xe0) {
            codePoint = c & 0x0f;
            extra = 2;
        } else if ((c & 0xf8) == 0xf0) {
            codePoint = c & 0x07;
            extra = 3;
        } else {
            codePoint = '?';
        }
        ++i;
        bool valid = i + extra <= text.size();
        for (size_t j = 0; valid && j < extra; ++j, ++i) {
            const unsigned char cc = static_cast<unsigned char>(text[i]);
            if ((cc & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (cc & 0x3f);
        }
        if (!valid) {
            result.push_back(u'?');
            continue;
        }
        if (codePoint <= 0xffff) {
            result.push_back(char16_t(codePoint));
        } else {
            codePoint -= 0x10000;
            result.push_back(char16_t(0xd800 + (codePoint >> 10)));
            result.push_back(char16_t(0xdc00 + (codePoint & 0x3ff)));
        }
    }
    return result;
}

std::string utf16ToUtf8(const std::vector<uint16_t>& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < units.size()) {
            const uint32_t lo = units[i + 1];
            if (lo >= 0xdc00 && lo <= 0xdfff) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            }
        }
        appendUtf8(out, cp);
    }
    return out;
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

void SyntheticDllRuntime::setRegistryPath(const std::filesystem::path& path) {
    registryPath_ = path;
    registry_ = nlohmann::json::object();
    if (!registryPath_.empty()) {
        std::ifstream input(registryPath_);
        if (input) {
            try {
                input >> registry_;
            } catch (const std::exception& e) {
                spdlog::warn("failed to parse registry database {}: {}", pathToUtf8(registryPath_), e.what());
                registry_ = nlohmann::json::object();
            }
        }
    }
    if (!registry_.is_object()) registry_ = nlohmann::json::object();
    if (!registry_.contains("version")) registry_["version"] = 1;
    normalizeRegistryDatabase(registry_);
    registryEnsureKey("hkcr");
    registryEnsureKey("hkcu");
    registryEnsureKey("hklm");
    registryEnsureKey("hku");
    registryDirty_ = false;
    if (!registryPath_.empty()) spdlog::info("registry database: {}", pathToUtf8(registryPath_));
}

void SyntheticDllRuntime::flushRegistry() {
    if (registryPath_.empty() || !registryDirty_) return;
    if (!registryPath_.parent_path().empty()) std::filesystem::create_directories(registryPath_.parent_path());
    std::ofstream output(registryPath_, std::ios::binary | std::ios::trunc);
    if (!output) {
        spdlog::warn("failed to write registry database {}", pathToUtf8(registryPath_));
        return;
    }
    output << registry_.dump(2);
    registryDirty_ = false;
}

std::optional<std::string> SyntheticDllRuntime::registryPathFromHandle(uint32_t hkey, const std::string& subKey) const {
    std::string base = registryRootName(hkey);
    if (base.empty()) {
        auto handle = ceKernel_.handles().find(hkey);
        auto path = registryHandles_.find(hkey);
        if (handle == ceKernel_.handles().end() || handle->second.kind != GuestHandle::Kind::GuestRegistryKey ||
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
    auto appendUtf16Le = [&](const std::string& text) {
        const std::u16string wide = utf8ToUtf16(text);
        for (char16_t unit : wide) {
            bytes.push_back(uint8_t(unit));
            bytes.push_back(uint8_t(uint16_t(unit) >> 8));
        }
    };
    auto appendWideNul = [&]() {
        bytes.push_back(0);
        bytes.push_back(0);
    };
    if (type == 1 || type == 2) {
        std::string text;
        if (data != value.end() && data->is_string()) text = data->get<std::string>();
        appendUtf16Le(text);
        appendWideNul();
    } else if (type == 7) {
        if (data != value.end() && data->is_array()) {
            for (const auto& item : *data) {
                if (!item.is_string()) continue;
                appendUtf16Le(item.get<std::string>());
                appendWideNul();
            }
            appendWideNul();
        } else {
            std::string text;
            if (data != value.end() && data->is_string()) text = data->get<std::string>();
            appendUtf16Le(text);
            appendWideNul();
            appendWideNul();
        }
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
    if (type == 1 || type == 2) {
        std::vector<uint16_t> units;
        const uint32_t chars = dataSize / 2;
        for (uint32_t i = 0; dataPtr && i < chars; ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, dataPtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
            units.push_back(ch);
        }
        value["data"] = utf16ToUtf8(units);
    } else if (type == 7) {
        nlohmann::json strings = nlohmann::json::array();
        std::vector<uint16_t> current;
        const uint32_t chars = dataSize / 2;
        for (uint32_t i = 0; dataPtr && i < chars; ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, dataPtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK) break;
            if (!ch) {
                if (current.empty()) break;
                strings.push_back(utf16ToUtf8(current));
                current.clear();
            } else {
                current.push_back(ch);
            }
        }
        if (!current.empty()) strings.push_back(utf16ToUtf8(current));
        value["data"] = std::move(strings);
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

uint32_t SyntheticDllRuntime::handleRegEnumValueW(uint32_t hkey, uint32_t index,
                                                  uint32_t valueNamePtr, uint32_t valueNameSizePtr) {
    const auto path = registryPathFromHandle(hkey, {});
    if (!path || !registryKeyExists(*path) || !valueNamePtr || !valueNameSizePtr) return 87;

    auto& values = registry_["keys"][*path]["values"];
    if (index >= values.size()) return 259;

    auto it = values.begin();
    std::advance(it, index);
    const std::string valueName = it.key();
    const uint32_t nameCapacity = readU32(valueNameSizePtr);
    if (nameCapacity <= valueName.size()) {
        writeU32(valueNameSizePtr, uint32_t(valueName.size() + 1));
        return 234;
    }

    writeUtf16(valueNamePtr, valueName, nameCapacity);
    writeU32(valueNameSizePtr, uint32_t(valueName.size()));
    if (stackArg(5)) writeU32(stackArg(5), registryTypeId(it.value()));

    const std::vector<uint8_t> bytes = registryValueBytes(it.value());
    const uint32_t dataPtr = stackArg(6);
    const uint32_t sizePtr = stackArg(7);
    if (!sizePtr) return 0;

    const uint32_t capacity = readU32(sizePtr);
    writeU32(sizePtr, uint32_t(bytes.size()));
    if (dataPtr && capacity >= bytes.size() && !bytes.empty()) {
        uc_mem_write(uc_, dataPtr, bytes.data(), bytes.size());
    }
    return dataPtr && capacity < bytes.size() ? 234 : 0;
}


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
        ceKernel_.handles().erase(args.a0);
    }
    ret = 0;
    return true;
}
