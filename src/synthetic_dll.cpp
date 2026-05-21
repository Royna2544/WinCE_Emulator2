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
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
constexpr uint32_t kErrorFileNotFound = 2;
constexpr uint32_t kErrorPathNotFound = 3;

bool supportedSourceRasterOp(uint32_t rop) {
    switch (rop) {
    case 0x00000042u: // BLACKNESS
    case 0x00330008u: // NOTSRCCOPY
    case 0x00550009u: // DSTINVERT
    case 0x00660046u: // SRCINVERT
    case 0x008800c6u: // SRCAND
    case 0x00cc0020u: // SRCCOPY
    case 0x00ee0086u: // SRCPAINT
    case 0x00ff0062u: // WHITENESS
        return true;
    default:
        return false;
    }
}

uint32_t applySourceRasterOp(uint32_t rop, uint32_t src, uint32_t dst) {
    const uint32_t s = src & 0x00ffffffu;
    const uint32_t d = dst & 0x00ffffffu;
    uint32_t out = 0;
    switch (rop) {
    case 0x00000042u: out = 0; break;
    case 0x00330008u: out = ~s; break;
    case 0x00550009u: out = ~d; break;
    case 0x00660046u: out = s ^ d; break;
    case 0x008800c6u: out = s & d; break;
    case 0x00cc0020u: out = s; break;
    case 0x00ee0086u: out = s | d; break;
    case 0x00ff0062u: out = 0x00ffffffu; break;
    default: out = s; break;
    }
    return 0xff000000u | (out & 0x00ffffffu);
}

std::vector<uint32_t> defaultIndexedPalette(uint16_t bpp) {
    if (bpp == 1) return {0xff000000u, 0xffffffffu};
    if (bpp != 4 && bpp != 8) return {};
    const uint32_t count = 1u << bpp;
    std::vector<uint32_t> palette(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t v = uint8_t((i * 255u) / (count - 1u));
        palette[i] = 0xff000000u | (uint32_t(v) << 16) | (uint32_t(v) << 8) | v;
    }
    return palette;
}

uint8_t expand5To8(uint16_t value) {
    value &= 0x1fu;
    return uint8_t((value << 3) | (value >> 2));
}

uint32_t decodeRgb555(uint16_t value) {
    const uint8_t r = expand5To8(value >> 10);
    const uint8_t g = expand5To8(value >> 5);
    const uint8_t b = expand5To8(value);
    return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint16_t encodeRgb555(uint32_t pixel) {
    const uint16_t r = uint16_t(((pixel >> 16) & 0xffu) * 31u / 255u);
    const uint16_t g = uint16_t(((pixel >> 8) & 0xffu) * 31u / 255u);
    const uint16_t b = uint16_t((pixel & 0xffu) * 31u / 255u);
    return uint16_t((r << 10) | (g << 5) | b);
}

uint32_t decodeRgb565(uint16_t value) {
    const uint8_t r = expand5To8(value >> 11);
    const uint8_t g = uint8_t(((value >> 5) & 0x3fu) * 255u / 63u);
    const uint8_t b = expand5To8(value);
    return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint16_t encodeRgb565(uint32_t pixel) {
    const uint16_t r = uint16_t(((pixel >> 16) & 0xffu) * 31u / 255u);
    const uint16_t g = uint16_t(((pixel >> 8) & 0xffu) * 63u / 255u);
    const uint16_t b = uint16_t((pixel & 0xffu) * 31u / 255u);
    return uint16_t((r << 11) | (g << 5) | b);
}

uint32_t maskShift(uint32_t mask) {
    if (!mask) return 0;
    uint32_t shift = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

uint32_t maskBits(uint32_t mask) {
    uint32_t bits = 0;
    while (mask) {
        bits += mask & 1u;
        mask >>= 1;
    }
    return bits;
}

uint8_t expandMaskedChannel(uint32_t value, uint32_t mask) {
    if (!mask) return 0;
    const uint32_t shift = maskShift(mask);
    const uint32_t bits = maskBits(mask);
    const uint32_t raw = (value & mask) >> shift;
    const uint32_t maxValue = (1u << bits) - 1u;
    return uint8_t((raw * 255u + maxValue / 2u) / maxValue);
}

uint32_t compressMaskedChannel(uint32_t pixel, uint32_t shift, uint32_t mask) {
    if (!mask) return 0;
    const uint32_t bits = maskBits(mask);
    const uint32_t maxValue = (1u << bits) - 1u;
    const uint32_t raw = ((pixel >> shift) & 0xffu) * maxValue / 255u;
    return (raw << maskShift(mask)) & mask;
}

uint32_t decodeMasked16(uint16_t value, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    const uint32_t v = value;
    const uint8_t r = expandMaskedChannel(v, redMask);
    const uint8_t g = expandMaskedChannel(v, greenMask);
    const uint8_t b = expandMaskedChannel(v, blueMask);
    return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint16_t encodeMasked16(uint32_t pixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    return uint16_t(compressMaskedChannel(pixel, 16, redMask) |
                    compressMaskedChannel(pixel, 8, greenMask) |
                    compressMaskedChannel(pixel, 0, blueMask));
}

void ceDefault16BitMasks(uint32_t& redMask, uint32_t& greenMask, uint32_t& blueMask) {
    redMask = 0x0000f800u;
    greenMask = 0x000007e0u;
    blueMask = 0x0000001fu;
}

uint32_t decodeBitmap16(uint16_t value, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    if (!redMask && !greenMask && !blueMask) return decodeRgb565(value);
    return decodeMasked16(value, redMask, greenMask, blueMask);
}

uint16_t encodeBitmap16(uint32_t pixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    if (!redMask && !greenMask && !blueMask) return encodeRgb565(pixel);
    return encodeMasked16(pixel, redMask, greenMask, blueMask);
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

std::string normalizedPathKey(const std::filesystem::path& path) {
    std::string text = pathToUtf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '/', '\\');
    while (text.size() > 3 && text.back() == '\\') text.pop_back();
    return lowerAscii(text);
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

std::u16string utf8ToUtf16(std::string_view text) {
    std::u16string out;
    for (size_t i = 0; i < text.size();) {
        const uint8_t first = static_cast<uint8_t>(text[i++]);
        uint32_t codePoint = 0xfffdu;
        size_t extra = 0;
        if (first < 0x80) {
            codePoint = first;
        } else if ((first & 0xe0u) == 0xc0u) {
            codePoint = first & 0x1fu;
            extra = 1;
        } else if ((first & 0xf0u) == 0xe0u) {
            codePoint = first & 0x0fu;
            extra = 2;
        } else if ((first & 0xf8u) == 0xf0u) {
            codePoint = first & 0x07u;
            extra = 3;
        }
        bool valid = extra == 0 || i + extra <= text.size();
        for (size_t j = 0; valid && j < extra; ++j) {
            const uint8_t byte = static_cast<uint8_t>(text[i + j]);
            if ((byte & 0xc0u) != 0x80u) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (byte & 0x3fu);
        }
        if (valid) i += extra;
        if (!valid || codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu)) {
            codePoint = 0xfffdu;
        }
        if (codePoint <= 0xffffu) {
            out.push_back(static_cast<char16_t>(codePoint));
        } else {
            codePoint -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xd800u + (codePoint >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00u + (codePoint & 0x3ffu)));
        }
    }
    return out;
}

void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint <= 0x7fu) {
        out.push_back(char(codePoint));
    } else if (codePoint <= 0x7ffu) {
        out.push_back(char(0xc0u | (codePoint >> 6)));
        out.push_back(char(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        out.push_back(char(0xe0u | (codePoint >> 12)));
        out.push_back(char(0x80u | ((codePoint >> 6) & 0x3fu)));
        out.push_back(char(0x80u | (codePoint & 0x3fu)));
    } else {
        out.push_back(char(0xf0u | (codePoint >> 18)));
        out.push_back(char(0x80u | ((codePoint >> 12) & 0x3fu)));
        out.push_back(char(0x80u | ((codePoint >> 6) & 0x3fu)));
        out.push_back(char(0x80u | (codePoint & 0x3fu)));
    }
}

std::string utf16ToUtf8(const std::vector<uint16_t>& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t codePoint = units[i];
        if (codePoint >= 0xd800u && codePoint <= 0xdbffu && i + 1 < units.size()) {
            const uint32_t low = units[i + 1];
            if (low >= 0xdc00u && low <= 0xdfffu) {
                codePoint = 0x10000u + ((codePoint - 0xd800u) << 10) + (low - 0xdc00u);
                ++i;
            } else {
                codePoint = 0xfffdu;
            }
        } else if (codePoint >= 0xdc00u && codePoint <= 0xdfffu) {
            codePoint = 0xfffdu;
        }
        appendUtf8(out, codePoint);
    }
    return out;
}

#if defined(_WIN32)
std::string wideZToUtf8(const wchar_t* text, size_t maxChars) {
    std::vector<uint16_t> units;
    if (!text) return {};
    units.reserve(std::min<size_t>(maxChars, 260));
    for (size_t i = 0; i < maxChars && text[i]; ++i) {
        units.push_back(uint16_t(text[i]));
    }
    return utf16ToUtf8(units);
}

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

uint32_t guestAnsiCodePage(uint32_t codePage) {
    // The target SDK path is L.kor, so guest CP_ACP/CP_THREAD_ACP is Korean.
    return (codePage == 0 || codePage == 3) ? 949u : codePage;
}

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

bool sameModule(std::string name, std::string_view wanted) {
    name = lowerAscii(std::move(name));
    return name == wanted;
}

bool isRegistryApiName(const std::string& name) {
    return name == "RegCreateKeyExW" || name == "RegOpenKeyExW" ||
           name == "RegQueryValueExW" || name == "RegSetValueExW" ||
           name == "RegDeleteValueW" || name == "RegDeleteKeyW" ||
           name == "RegEnumKeyExW" || name == "RegEnumValueW" ||
           name == "RegQueryInfoKeyW" || name == "RegFlushKey" ||
           name == "RegCloseKey";
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
    bridge.waveInGetID = reinterpret_cast<decltype(bridge.waveInGetID)>(
        GetProcAddress(bridge.module, "waveInGetID"));
    bridge.waveInMessage = reinterpret_cast<decltype(bridge.waveInMessage)>(
        GetProcAddress(bridge.module, "waveInMessage"));
    bridge.waveOutGetNumDevs = reinterpret_cast<decltype(bridge.waveOutGetNumDevs)>(
        GetProcAddress(bridge.module, "waveOutGetNumDevs"));
    bridge.waveOutOpen = reinterpret_cast<decltype(bridge.waveOutOpen)>(
        GetProcAddress(bridge.module, "waveOutOpen"));
    bridge.waveOutClose = reinterpret_cast<decltype(bridge.waveOutClose)>(
        GetProcAddress(bridge.module, "waveOutClose"));
    bridge.waveOutSetVolume = reinterpret_cast<decltype(bridge.waveOutSetVolume)>(
        GetProcAddress(bridge.module, "waveOutSetVolume"));
    bridge.waveOutReset = reinterpret_cast<decltype(bridge.waveOutReset)>(
        GetProcAddress(bridge.module, "waveOutReset"));
    bridge.waveOutPrepareHeader = reinterpret_cast<decltype(bridge.waveOutPrepareHeader)>(
        GetProcAddress(bridge.module, "waveOutPrepareHeader"));
    bridge.waveOutUnprepareHeader = reinterpret_cast<decltype(bridge.waveOutUnprepareHeader)>(
        GetProcAddress(bridge.module, "waveOutUnprepareHeader"));
    bridge.waveOutWrite = reinterpret_cast<decltype(bridge.waveOutWrite)>(
        GetProcAddress(bridge.module, "waveOutWrite"));
    bridge.mixerGetControlDetailsW = reinterpret_cast<decltype(bridge.mixerGetControlDetailsW)>(
        GetProcAddress(bridge.module, "mixerGetControlDetailsW"));
    return bridge;
}

struct HostPresenterWindow {
    SyntheticDllRuntime* runtime{};
    uint32_t guestHwnd{};
    uint32_t* framebuffer{};
    int width{};
    int height{};
};

int hostPresenterDisplayWidth(const HostPresenterWindow& presenter) {
    return std::max(1, presenter.width);
}

int hostPresenterDisplayHeight(const HostPresenterWindow& presenter) {
    return std::max(1, presenter.height);
}

std::wstring widenLossy(const std::string& value) {
    std::wstring wide;
    wide.reserve(value.size());
    for (unsigned char ch : value) wide.push_back(wchar_t(ch));
    return wide;
}

const wchar_t* hostPresenterClassName() {
    return L"FakeCEHostPresenterWindow";
}

LRESULT CALLBACK hostPresenterWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        if (presenter && presenter->framebuffer && presenter->width > 0 && presenter->height > 0) {
            RECT client{};
            GetClientRect(hwnd, &client);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = presenter->width;
            info.bmiHeader.biHeight = -presenter->height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            StretchDIBits(dc, 0, 0, client.right - client.left, client.bottom - client.top,
                          0, 0, presenter->width, presenter->height,
                          presenter->framebuffer, &info, DIB_RGB_COLORS, SRCCOPY);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
        if (presenter && presenter->runtime) {
            RECT client{};
            GetClientRect(hwnd, &client);
            const int clientWidth = std::max(1L, client.right - client.left);
            const int clientHeight = std::max(1L, client.bottom - client.top);
            const int hostX = int16_t(LOWORD(lParam));
            const int hostY = int16_t(HIWORD(lParam));
            const int x = std::clamp(MulDiv(hostX, presenter->width, clientWidth), 0, presenter->width - 1);
            const int y = std::clamp(MulDiv(hostY, presenter->height, clientHeight), 0, presenter->height - 1);
            uint32_t guestMessage = 0;
            if (message == WM_LBUTTONDOWN) {
                SetFocus(hwnd);
                SetCapture(hwnd);
                guestMessage = 0x0201; // WM_LBUTTONDOWN
            } else if (message == WM_LBUTTONUP) {
                ReleaseCapture();
                guestMessage = 0x0202; // WM_LBUTTONUP
            }
            presenter->runtime->queueHostMouseMessage(presenter->guestHwnd, guestMessage, x, y);
        }
        return 0;
    }
    if (message == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (message == WM_NCDESTROY) {
        delete presenter;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

ATOM registerHostPresenterClass() {
    static ATOM atom = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = hostPresenterWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = hostPresenterClassName();
        return RegisterClassW(&wc);
    }();
    return atom;
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

void SyntheticDllRuntime::setMainModuleBase(uint32_t base) {
    mainModuleBase_ = base;
}

void SyntheticDllRuntime::setFileSystemRoots(std::vector<std::filesystem::path> roots) {
    fileSystemRoots_.clear();
    for (auto& root : roots) {
        if (!root.empty()) fileSystemRoots_.push_back(std::move(root));
    }
}

void SyntheticDllRuntime::setGpsCommPort(std::string port) {
    gpsCommPort_ = std::move(port);
    if (!gpsCommPort_.empty()) spdlog::info("gps comm requested: {}", gpsCommPort_);
}

void SyntheticDllRuntime::setFramebuffer(uint32_t* bgra, int width, int height) {
    framebuffer_ = bgra;
    framebufferWidth_ = width;
    framebufferHeight_ = height;
}

void SyntheticDllRuntime::registerLoadedModule(const std::string& moduleName,
                                               const std::filesystem::path& path,
                                               uint32_t base,
                                               const std::map<std::string, uint32_t>& exportsByName,
                                               const std::map<uint16_t, uint32_t>& exportsByOrdinal) {
    if (!base) return;
    std::string nameKey = lowerAscii(pathToUtf8(std::filesystem::path(moduleName).filename()));
    if (nameKey.empty() && !path.empty()) nameKey = lowerAscii(pathToUtf8(path.filename()));
    LoadedModuleInfo info{nameKey, path, base, exportsByName, exportsByOrdinal};
    if (!nameKey.empty()) loadedModulesByName_[nameKey] = info;
    if (!path.empty()) loadedModulesByPath_[lowerAscii(pathToUtf8(path))] = info;
    loadedModulesByBase_[base] = info;
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

bool SyntheticDllRuntime::hasHostWindows() const {
#if defined(_WIN32)
    for (const auto& [guestHwnd, window] : windows_) {
        (void)guestHwnd;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (hwnd && IsWindow(hwnd)) return true;
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (hwnd && IsWindow(hwnd)) return true;
    }
#endif
    return false;
}

void SyntheticDllRuntime::runHostMessageLoopUntilClosed() {
#if defined(_WIN32)
    if (!hasHostWindows()) return;
    for (auto& [guestHwnd, window] : windows_) {
        (void)guestHwnd;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (!hwnd || !IsWindow(hwnd)) continue;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (!hwnd || !IsWindow(hwnd)) continue;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }
    spdlog::info("entering host GUI message loop; close the presenter window to exit");
    MSG message{};
    while (hasHostWindows()) {
        enqueueDueTimers();
        while (!guestMessages_.empty() && hasHostWindows()) {
            uint32_t pc = 0;
            uint32_t ra = 0;
            uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
            uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
            spdlog::info("resuming guest for queued message pc=0x{:08x} ra=0x{:08x} queued={}",
                         pc, ra, guestMessages_.size());
            const uc_err err = uc_emu_start(uc_, pc, 0, 0, 50000000);
            uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
            uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
            if (err != UC_ERR_OK) {
                spdlog::warn("interactive emulation stopped err={} ({}) pc=0x{:08x} ra=0x{:08x}",
                             int(err), uc_strerror(err), pc, ra);
                return;
            }
            pumpHostMessages();
            enqueueDueTimers();
        }
        const DWORD waitMs = std::max<DWORD>(1, std::min<DWORD>(50, timerWaitMilliseconds()));
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (guestMessages_.empty()) MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
    }
#endif
}

std::optional<SyntheticModule> SyntheticDllRuntime::createModule(const std::string& dllName) {
    if (sameModule(dllName, "coredll.dll")) return createCoredll();
    if (sameModule(dllName, "commctrl.dll")) return createCommctrl();
    if (sameModule(dllName, "winsock.dll") || sameModule(dllName, "ws2.dll")) {
        const std::string moduleName = sameModule(dllName, "ws2.dll") ? "WS2.dll" : "WINSOCK.dll";
        auto module = createGenericOrdinalDll(moduleName, 128);
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
            registerExport(*module, 0x01F0, "__ComQueryInterface");
            registerExport(*module, 0x01F1, "__ComAddRef");
            registerExport(*module, 0x01F2, "__ComRelease");
            comQueryInterfaceStub_ = module->imageBase + module->exportsByOrdinal[0x01F0];
            comAddRefStub_ = module->imageBase + module->exportsByOrdinal[0x01F1];
            comReleaseStub_ = module->imageBase + module->exportsByOrdinal[0x01F2];
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
    registerExport(module, 0x0014, "FileTimeToSystemTime");
    registerExport(module, 0x0017, "GetLocalTime");
    registerExport(module, 0x0023, "GetLocalTime");
    registerExport(module, 0x0019, "GetSystemTime");
    registerExport(module, 0x0025, "GetSystemTime");
    registerExport(module, 0x001B, "GetTimeZoneInformation");
    registerExport(module, 0x002C, "GetAPIAddress");
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
    registerExport(module, 0x0059, "SystemParametersInfoW");
    registerExport(module, 0x0041, "wcsncmp");
    registerExport(module, 0x0045, "wcsrchr");
    registerExport(module, 0x0049, "wcsstr");
    registerExport(module, 0x004A, "_wcsdup");
    registerExport(module, 0x004E, "_wtol");
    registerExport(module, 0x0058, "GlobalMemoryStatus");
    registerExport(module, 0x005A, "CreateDIBSection");
    registerExport(module, 0x005E, "LoadAcceleratorsW");
    registerExport(module, 0x005F, "RegisterClassW");
    registerExport(module, 0x0060, "CopyRect");
    registerExport(module, 0x0061, "EqualRect");
    registerExport(module, 0x0062, "InflateRect");
    registerExport(module, 0x0064, "IsRectEmpty");
    registerExport(module, 0x0066, "PtInRect");
    registerExport(module, 0x0067, "SetRect");
    registerExport(module, 0x0068, "SetRectEmpty");
    registerExport(module, 0x006C, "ClearCommError");
    registerExport(module, 0x0071, "GetCommState");
    registerExport(module, 0x0073, "PurgeComm");
    registerExport(module, 0x0075, "SetCommMask");
    registerExport(module, 0x0076, "SetCommState");
    registerExport(module, 0x0077, "SetCommTimeouts");
    registerExport(module, 0x0078, "SetupComm");
    registerExport(module, 0x00A0, "CreateDirectoryW");
    registerExport(module, 0x00A5, "DeleteFileW");
    registerExport(module, 0x00A6, "GetFileAttributesW");
    registerExport(module, 0x00A7, "FindFirstFileW");
    registerExport(module, 0x00A8, "CreateFileW");
    registerExport(module, 0x00AA, "ReadFile");
    registerExport(module, 0x00AB, "WriteFile");
    registerExport(module, 0x00AC, "GetFileSize");
    registerExport(module, 0x00AD, "SetFilePointer");
    registerExport(module, 0x00AF, "FlushFileBuffers");
    registerExport(module, 0x00B1, "SetFileTime");
    registerExport(module, 0x00B3, "DeviceIoControl");
    registerExport(module, 0x00B4, "FindClose");
    registerExport(module, 0x00B5, "FindNextFileW");
    registerExport(module, 0x00C0, "IsDBCSLeadByteEx");
    registerExport(module, 0x00C1, "iswctype");
    registerExport(module, 0x00C4, "MultiByteToWideChar");
    registerExport(module, 0x00C5, "WideCharToMultiByte");
    registerExport(module, 0x00DD, "CharLowerW");
    registerExport(module, 0x00E0, "CharUpperW");
    registerExport(module, 0x00E5, "_wcsnicmp");
    registerExport(module, 0x00E6, "_wcsicmp");
    registerExport(module, 0x00EA, "FormatMessageW");
    registerExport(module, 0x00F6, "CreateWindowExW");
    registerExport(module, 0x00F7, "SetWindowPos");
    registerExport(module, 0x00F8, "GetWindowRect");
    registerExport(module, 0x00F9, "GetClientRect");
    registerExport(module, 0x00FA, "InvalidateRect");
    registerExport(module, 0x00FB, "GetWindow");
    registerExport(module, 0x00FF, "ScreenToClient");
    registerExport(module, 0x0101, "GetWindowTextW");
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
    registerExport(module, 0x0110, "MoveWindow");
    registerExport(module, 0x0100, "ScreenToClient");
    registerExport(module, 0x0114, "GetWindowTextLengthW");
    registerExport(module, 0x0116, "ValidateRect");
    registerExport(module, 0x011F, "EnableWindow");
    registerExport(module, 0x0120, "IsWindowEnabled");
    registerExport(module, 0x011D, "CallWindowProcW");
    registerExport(module, 0x011E, "FindWindowW");
    registerExport(module, 0x0143, "GetStoreInformation");
    registerExport(module, 0x017B, "waveOutGetNumDevs");
    registerExport(module, 0x017E, "waveOutSetVolume");
    registerExport(module, 0x0180, "waveOutClose");
    registerExport(module, 0x0181, "waveOutPrepareHeader");
    registerExport(module, 0x0182, "waveOutUnprepareHeader");
    registerExport(module, 0x0183, "waveOutWrite");
    registerExport(module, 0x0186, "waveOutReset");
    registerExport(module, 0x018F, "waveOutOpen");
    registerExport(module, 0x0193, "waveInClose");
    registerExport(module, 0x0195, "waveInUnprepareHeader");
    registerExport(module, 0x0196, "waveInAddBuffer");
    registerExport(module, 0x0199, "waveInReset");
    registerExport(module, 0x019B, "waveInGetID");
    registerExport(module, 0x019C, "waveInMessage");
    registerExport(module, 0x019D, "waveInOpen");
    registerExport(module, 0x01BE, "WNetConnectionDialog1W");
    registerExport(module, 0x01C2, "WNetGetUniversalNameW");
    registerExport(module, 0x01C3, "WNetGetUserW");
    registerExport(module, 0x01C8, "RegCreateKeyExW");
    registerExport(module, 0x01EC, "CreateThread");
    registerExport(module, 0x01ED, "CreateProcessW");
    registerExport(module, 0x01EF, "CreateEventW");
    registerExport(module, 0x01EE, "EventModify");
    registerExport(module, 0x01F0, "Sleep");
    registerExport(module, 0x01F1, "WaitForSingleObject");
    registerExport(module, 0x01F4, "ResumeThread");
    registerExport(module, 0x0202, "SetThreadPriority");
    registerExport(module, 0x0203, "GetThreadPriority");
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
    registerExport(module, 0x0224, "CreateFileMappingW");
    registerExport(module, 0x0225, "MapViewOfFile");
    registerExport(module, 0x0226, "UnmapViewOfFile");
    registerExport(module, 0x0227, "FlushViewOfFile");
    registerExport(module, 0x0228, "CreateFileForMapping");
    registerExport(module, 0x0229, "CloseHandle");
    registerExport(module, 0x022B, "CreateMutexW");
    registerExport(module, 0x022C, "ReleaseMutex");
    registerExport(module, 0x022D, "KernelIoControl");
    registerExport(module, 0x0280, "GetProcessIndexFromID");
    registerExport(module, 0x02AA, "SetCursor");
    registerExport(module, 0x02AB, "LoadCursorW");
    registerExport(module, 0x02B4, "GetDlgItem");
    registerExport(module, 0x02B5, "GetDlgCtrlID");
    registerExport(module, 0x02CD, "GetVersionExW");
    registerExport(module, 0x02CF, "sprintf");
    registerExport(module, 0x02DD, "LocalAllocTrace");
    registerExport(module, 0x02DE, "GetCursorPos");
    registerExport(module, 0x02D8, "LoadIconW");
    registerExport(module, 0x02D9, "_snprintf");
    registerExport(module, 0x02DA, "LoadImageW");
    registerExport(module, 0x02BE, "SetForegroundWindow");
    registerExport(module, 0x02C0, "SetFocus");
    registerExport(module, 0x02C1, "GetFocus");
    registerExport(module, 0x02C2, "GetActiveWindow");
    registerExport(module, 0x02C3, "GetCapture");
    registerExport(module, 0x02C4, "SetCapture");
    registerExport(module, 0x02C5, "ReleaseCapture");
    registerExport(module, 0x034B, "RemoveMenu");
    registerExport(module, 0x034E, "LoadMenuW");
    registerExport(module, 0x0350, "CheckMenuItem");
    registerExport(module, 0x0351, "CheckMenuRadioItem");
    registerExport(module, 0x035A, "MessageBoxW");
    registerExport(module, 0x036A, "LoadStringW");
    registerExport(module, 0x036B, "SetTimer");
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
    registerExport(module, 0x0377, "AdjustWindowRectEx");
    registerExport(module, 0x0379, "GetSysColor");
    registerExport(module, 0x0394, "GetDeviceCaps");
    registerExport(module, 0x03A9, "GetSysColorBrush");
    registerExport(module, 0x037F, "CreateFontIndirectW");
    registerExport(module, 0x0380, "ExtTextOutW");
    registerExport(module, 0x0385, "CreateBitmap");
    registerExport(module, 0x0387, "BitBlt");
    registerExport(module, 0x038A, "TransparentImage");
    registerExport(module, 0x038E, "CreateCompatibleDC");
    registerExport(module, 0x0386, "CreateCompatibleBitmap");
    registerExport(module, 0x0389, "StretchBlt");
    registerExport(module, 0x026D, "CeSetThreadPriority");
    registerExport(module, 0x0390, "DeleteObject");
    registerExport(module, 0x038F, "DeleteDC");
    registerExport(module, 0x0397, "GetStockObject");
    registerExport(module, 0x0396, "GetObjectW");
    registerExport(module, 0x0399, "SelectObject");
    registerExport(module, 0x039A, "SetBkColor");
    registerExport(module, 0x039B, "SetBkMode");
    registerExport(module, 0x039C, "SetTextColor");
    registerExport(module, 0x039E, "CreatePen");
    registerExport(module, 0x03A2, "CreatePenIndirect");
    registerExport(module, 0x03A3, "CreateSolidBrush");
    registerExport(module, 0x03A7, "FillRect");
    registerExport(module, 0x03AA, "PatBlt");
    registerExport(module, 0x03AC, "Polyline");
    registerExport(module, 0x03AD, "Rectangle");
    registerExport(module, 0x03B1, "DrawTextW");
    registerExport(module, 0x03D4, "CreateRectRgn");
    registerExport(module, 0x03C8, "CombineRgn");
    registerExport(module, 0x037B, "RegisterWindowMessageW");
    registerExport(module, 0x037C, "RegisterTaskBar");
    registerExport(module, 0x03E1, "atoi");
    registerExport(module, 0x03E3, "atof");
    registerExport(module, 0x04A1, "GetDCEx");
    registerExport(module, 0x0448, "_snwprintf");
    registerExport(module, 0x0449, "swprintf");
    registerExport(module, 0x044B, "vswprintf");
    registerExport(module, 0x044E, "printf");
    registerExport(module, 0x0454, "fgetc");
    registerExport(module, 0x0455, "fgets");
    registerExport(module, 0x0459, "fopen");
    registerExport(module, 0x045E, "fclose");
    registerExport(module, 0x0460, "fread");
    registerExport(module, 0x0461, "fwrite");
    registerExport(module, 0x0462, "fflush");
    registerExport(module, 0x0465, "feof");
    registerExport(module, 0x0466, "ferror");
    registerExport(module, 0x046A, "fseek");
    registerExport(module, 0x046B, "ftell");
    registerExport(module, 0x046C, "_vsnwprintf");
    registerExport(module, 0x0479, "_wfopen");
    registerExport(module, 0x01C7, "RegCloseKey");
    registerExport(module, 0x01CD, "RegOpenKeyExW");
    registerExport(module, 0x01CF, "RegQueryValueExW");
    registerExport(module, 0x01C9, "RegDeleteKeyW");
    registerExport(module, 0x01CA, "RegDeleteValueW");
    registerExport(module, 0x01CB, "RegEnumValueW");
    registerExport(module, 0x01CC, "RegEnumKeyExW");
    registerExport(module, 0x01CE, "RegQueryInfoKeyW");
    registerExport(module, 0x01D0, "RegSetValueExW");
    registerExport(module, 0x020A, "IsBadReadPtr");
    registerExport(module, 0x020B, "IsBadWritePtr");
    registerExport(module, 0x0480, "RegFlushKey");
    registerExport(module, 0x03FA, "free");
    registerExport(module, 0x040C, "longjmp");
    registerExport(module, 0x0411, "malloc");
    registerExport(module, 0x0413, "memcmp");
    registerExport(module, 0x0414, "memcpy");
    registerExport(module, 0x0416, "memmove");
    registerExport(module, 0x0417, "memset");
    registerExport(module, 0x041D, "rand");
    registerExport(module, 0x041E, "realloc");
    registerExport(module, 0x0424, "sqrt");
    registerExport(module, 0x0425, "srand");
    registerExport(module, 0x0427, "strcat");
    registerExport(module, 0x0429, "strcmp");
    registerExport(module, 0x042A, "strcpy");
    registerExport(module, 0x042B, "strcspn");
    registerExport(module, 0x042C, "strlen");
    registerExport(module, 0x0431, "strtok");
    registerExport(module, 0x0446, "operator_delete");
    registerExport(module, 0x0447, "operator_new");
    registerExport(module, 0x0499, "GetModuleHandleW");
    registerExport(module, 0x04BD, "IsProcessDying");
    registerExport(module, 0x04CB, "GetCRTStorageEx");
    registerExport(module, 0x04CC, "GetCRTFlags");
    registerExport(module, 0x04CE, "GetProcAddressA");
    registerExport(module, 0x04D1, "TryEnterCriticalSection");
    registerExport(module, 0x04D5, "GetFileAttributesExW");
    registerExport(module, 0x057C, "strtol");
    registerExport(module, 0x057D, "strtoul");
    registerExport(module, 0x0582, "_stricmp");
    registerExport(module, 0x0583, "_strnicmp");
    registerExport(module, 0x0438, "_ultow");
    registerExport(module, 0x0443, "toupper");
    registerExport(module, 0x05B0, "operator_vector_new");
    registerExport(module, 0x05B1, "operator_vector_delete");
    registerExport(module, 0x05D1, "KernelLibIoControl");
    registerExport(module, 0x05D3, "InterlockedExchangeAdd");
    registerExport(module, 0x05D4, "InterlockedCompareExchange");
    registerExport(module, 0x05E3, "RegisterDesktop");
    registerExport(module, 0x05EF, "GlobalAddAtomW");
    registerExport(module, 0x05F0, "GlobalDeleteAtom");
    registerExport(module, 0x05F1, "GlobalFindAtomW");
    registerExport(module, 0x0576, "SetWindowRgn");
    registerExport(module, 0x0577, "GetWindowRgn");
    registerExport(module, 0x0635, "mixerGetControlDetails");
    registerExport(module, 0x0646, "RemoteHeapFree");
    registerExport(module, 0x066B, "fmodf");
    registerExport(module, 0x0673, "MoveToEx");
    registerExport(module, 0x0674, "LineTo");
    registerExport(module, 0x0676, "SetTextAlign");
    registerExport(module, 0x0682, "SetDIBColorTable");
    registerExport(module, 0x0683, "StretchDIBits");
    registerExport(module, 0x06BD, "SetBitmapBits");
    registerExport(module, 0x06BE, "SetDIBitsToDevice");
    registerExport(module, 0x07D0, "_setjmp");
    registerExport(module, 0x07D5, "__ll_div");
    registerExport(module, 0x07E6, "__fpadd");
    registerExport(module, 0x0628, "__ehvec_ctor");
    registerExport(module, 0x07E7, "__dpadd");
    registerExport(module, 0x07E9, "__dpsub");
    registerExport(module, 0x07EA, "__fpmul");
    registerExport(module, 0x07EB, "__dpmul");
    registerExport(module, 0x07ED, "__dpdiv");
    registerExport(module, 0x07EE, "__fptoli");
    registerExport(module, 0x07EF, "__fptoul");
    registerExport(module, 0x07F2, "__dptoli");
    registerExport(module, 0x07F3, "__dptoul");
    registerExport(module, 0x07F4, "__litodp");
    registerExport(module, 0x07F5, "__ultodp");
    registerExport(module, 0x07F6, "__fptodp");
    registerExport(module, 0x07F7, "__dptofp");
    registerExport(module, 0x07F0, "__litofp");
    registerExport(module, 0x07FA, "__lts");
    registerExport(module, 0x07FB, "__les");
    registerExport(module, 0x07FC, "__eqs");
    registerExport(module, 0x07FD, "__ges");
    registerExport(module, 0x07FF, "__nes");
    registerExport(module, 0x0800, "__ltd");
    registerExport(module, 0x0801, "__led");
    registerExport(module, 0x0802, "__eqd");
    registerExport(module, 0x0803, "__ged");
    registerExport(module, 0x0532, "operator_new");
    registerExport(module, 0x0533, "operator_vector_new");
    registerExport(module, 0x0535, "operator_new_nothrow");
    registerExport(module, 0x0536, "operator_vector_new_nothrow");
    registerExport(module, 0x0531, "operator_delete");
    registerExport(module, 0x0534, "operator_vector_delete");
    registerExport(module, 0x0537, "operator_delete_nothrow");
    registerExport(module, 0x0538, "operator_vector_delete_nothrow");

    destroyWindowContinuationStub_ = module.imageBase + 0x0001f000;
    auto& destroyContinuation = exportsByAddress_[destroyWindowContinuationStub_];
    destroyContinuation.moduleName = module.moduleName;
    destroyContinuation.name = "__DestroyWindowContinue";
    writeStub(destroyWindowContinuationStub_);
    createWindowContinuationStub_ = module.imageBase + 0x0001f008;
    auto& createContinuation = exportsByAddress_[createWindowContinuationStub_];
    createContinuation.moduleName = module.moduleName;
    createContinuation.name = "__CreateWindowContinue";
    writeStub(createWindowContinuationStub_);
    blockingApiContinuationStub_ = module.imageBase + 0x0001f010;
    auto& blockingContinuation = exportsByAddress_[blockingApiContinuationStub_];
    blockingContinuation.moduleName = module.moduleName;
    blockingContinuation.name = "__BlockingApiContinue";
    writeStub(blockingApiContinuationStub_);
    spdlog::info("mapped synthetic COREDLL.dll base=0x{:08x} ordinals={}",
                 module.imageBase, module.exportsByOrdinal.size());
    return module;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createCommctrl() {
    SyntheticModule module;
    module.moduleName = "commctrl.dll";
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00010000;
    nextModuleBase_ += 0x00010000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic commctrl.dll");
    }
    for (uint16_t ordinal = 1; ordinal <= 128; ++ordinal) {
        registerExport(module, ordinal, {});
    }

    // Names and ordinals are from the Windows CE 4.2 Standard SDK MIPSII
    // commctrl.lib COFF import-object headers, not from /LINKERMEMBER.
    registerExport(module, 0x0001, "InitCommonControls");
    registerExport(module, 0x0002, "InitCommonControlsEx");
    registerExport(module, 0x0003, "CommandBar_Create");
    registerExport(module, 0x0004, "CommandBar_Show");
    registerExport(module, 0x0005, "CommandBar_AddBitmap");
    registerExport(module, 0x0006, "CommandBar_InsertComboBox");
    registerExport(module, 0x0007, "CommandBar_InsertControl");
    registerExport(module, 0x0008, "CommandBar_InsertMenubar");
    registerExport(module, 0x0009, "CommandBar_GetMenu");
    registerExport(module, 0x000A, "CommandBar_AddAdornments");
    registerExport(module, 0x000B, "CommandBar_GetItemWindow");
    registerExport(module, 0x000C, "CommandBar_Height");
    registerExport(module, 0x000D, "IsCommandBarMessage");
    registerExport(module, 0x000E, "CreateUpDownControl");
    registerExport(module, 0x000F, "?CreateToolbar@@YAPAUHWND__@@PAU1@KIHPAUHINSTANCE__@@IPBU_TBBUTTON@@H@Z");
    registerExport(module, 0x0010, "CreateToolbarEx");
    registerExport(module, 0x0011, "CreateStatusWindowW");
    registerExport(module, 0x0012, "PropertySheetW");
    registerExport(module, 0x0013, "CreatePropertySheetPageW");
    registerExport(module, 0x0014, "DestroyPropertySheetPage");
    registerExport(module, 0x0015, "DrawStatusTextW");
    registerExport(module, 0x0016, "InvertRect");
    registerExport(module, 0x002A, "CommandBar_InsertMenubarEx");
    registerExport(module, 0x002B, "CommandBar_DrawMenuBar");
    registerExport(module, 0x002C, "CommandBar_AlignAdornments");
    registerExport(module, 0x0030, "InitCapEdit");
    registerExport(module, 0x0033, "InitDateClasses");
    registerExport(module, 0x0034, "InitProgressClass");
    registerExport(module, 0x0035, "InitReBarClass");
    registerExport(module, 0x0036, "InitSBEdit");
    registerExport(module, 0x0037, "InitStatusClass");
    registerExport(module, 0x0038, "InitTTButton");
    registerExport(module, 0x0039, "InitTTStatic");
    registerExport(module, 0x003A, "InitToolTipsClass");
    registerExport(module, 0x003B, "InitToolbarClass");
    registerExport(module, 0x003C, "InitTrackBar");
    registerExport(module, 0x003D, "InitUpDownClass");
    registerExport(module, 0x0041, "ListView_SetItemSpacing");
    registerExport(module, 0x0045, "Tab_Init");

    spdlog::info("mapped synthetic commctrl.dll base=0x{:08x} ordinals={}",
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
        auto window = windows_.find(guestHandle);
        if (window != windows_.end()) destroyHostWindow(window->second);
        windows_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestDc) {
        dcs_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestBrush) {
        brushes_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestFont) {
        fonts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::GuestRegistryKey) {
        registryHandles_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostFile ||
               it->second.kind == GuestHandle::Kind::HostSerialDevice ||
               it->second.kind == GuestHandle::Kind::GuestSerialDevice) {
        fileHandleDebugNames_.erase(guestHandle);
        fileReadCounts_.erase(guestHandle);
        fileSeekCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostCrtFile) {
        fileHandleDebugNames_.erase(guestHandle);
        fileReadCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostFind) {
        fileHandleDebugNames_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostWaveOut) {
        waveOutStates_.erase(guestHandle);
        hostWaveBuffers_.clear();
    } else if (it->second.kind == GuestHandle::Kind::GuestFileMapping) {
        fileMappings_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostBitmap) {
        bitmaps_.erase(guestHandle);
        if (it->second.filePointer) releaseAllocation(it->second.filePointer);
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
            auto& winmm = winmmBridge();
            if (winmm.waveInClose) winmm.waveInClose(reinterpret_cast<HWAVEIN>(it->second.hostValue));
        } else if (it->second.kind == GuestHandle::Kind::HostWaveOut) {
            auto& winmm = winmmBridge();
            if (winmm.waveOutClose) winmm.waveOutClose(reinterpret_cast<HWAVEOUT>(it->second.hostValue));
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

uint32_t SyntheticDllRuntime::openGuestSerialDevice(const std::string& guestPath, uint32_t access, uint32_t share) {
#if defined(_WIN32)
    if (!gpsCommPort_.empty()) {
        const std::wstring hostPort = normalizeHostCommPort(gpsCommPort_);
        const DWORD desiredAccess = access ? access : (GENERIC_READ | GENERIC_WRITE);
        HANDLE host = CreateFileW(hostPort.c_str(), desiredAccess, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::string displayName = narrowAsciiLossy(hostPort);
        if (host != INVALID_HANDLE_VALUE) {
            const uint32_t guest = makeGuestHandle({GuestHandle::Kind::HostSerialDevice,
                                                    reinterpret_cast<uintptr_t>(host), 0});
            fileHandleDebugNames_[guest] = guestPath + " -> " + displayName;
            lastError_ = 0;
            spdlog::info("CreateFileW guest device=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x}",
                         guestPath, displayName, guest, desiredAccess, share);
            return guest;
        }
        spdlog::warn("CreateFileW guest device=\"{}\" host=\"{}\" unavailable lastError={}; using disconnected guest serial device",
                     guestPath, displayName, GetLastError());
    }
#endif
    const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
    fileHandleDebugNames_[guest] = gpsCommPort_.empty() ? guestPath + " -> disconnected"
                                                       : guestPath + " -> disconnected (" + gpsCommPort_ + ")";
    lastError_ = 0;
    spdlog::info("CreateFileW guest device=\"{}\" guestHandle=0x{:08x} disconnected access=0x{:08x} share=0x{:08x}",
                 guestPath, guest, access, share);
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
        lastError_ = 120;
        auto debugName = fileHandleDebugNames_.find(handleValue);
        spdlog::info("DeviceIoControl guest device handle=0x{:08x} name=\"{}\" code=0x{:08x} inSize={} outSize={} -> 0 lastError={}",
                     handleValue, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
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

uint32_t SyntheticDllRuntime::makeGuestDc(uint32_t hwnd) {
    GuestDc dc{};
    dc.hwnd = hwnd;
    dc.selectedBrush = makeStockObject(4); // BLACK_BRUSH
    dc.selectedPen = makeStockObject(7); // BLACK_PEN
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

uint32_t SyntheticDllRuntime::makeGuestPen(uint32_t style, uint32_t width, uint32_t colorRef, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestPen, 0, stock ? 1u : 0u});
    pens_[handle] = GuestPen{style, width, colorRef, stock};
    return handle;
}

uint32_t SyntheticDllRuntime::makeGuestFont(const std::array<uint8_t, 92>& logFont, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestFont, 0, stock ? 1u : 0u});
    fonts_[handle] = GuestFont{logFont, stock};
    return handle;
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
    case 6: handle = makeGuestPen(0, 1, 0x00ffffff, true); break; // WHITE_PEN
    case 7: handle = makeGuestPen(0, 1, 0x00000000, true); break; // BLACK_PEN
    case 8: handle = makeGuestPen(5, 1, 0xffffffffu, true); break; // NULL_PEN
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

uint32_t SyntheticDllRuntime::makeGuestWindow(const std::string& className, const std::string& title,
                                              uint32_t style, uint32_t exStyle, uint32_t parent,
                                              uint32_t menu, uint32_t instance, uint32_t param,
                                              int32_t x, int32_t y, int32_t width, int32_t height,
                                              bool visible, uint32_t wndProc) {
    const uint32_t hwnd = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
    GuestWindow window{};
    window.hwnd = hwnd;
    window.className = lowerAscii(className);
    window.title = title;
    window.style = style;
    window.exStyle = exStyle;
    window.parent = parent;
    window.menu = menu;
    window.instance = instance;
    window.param = param;
    window.wndProc = wndProc;
    window.x = x;
    window.y = y;
    window.width = std::max<int32_t>(1, width);
    window.height = std::max<int32_t>(1, height);
    window.visible = visible;
    windows_[hwnd] = window;
    ensureHostWindow(hwnd, windows_[hwnd]);
    return hwnd;
}

uint32_t SyntheticDllRuntime::loadMenuResourceHandle(uint32_t nameArg) {
    const ResourceEntry* resource = findResource(4, nameArg);
#if defined(_WIN32)
    if (!resource || resource->data.empty()) {
        spdlog::info("LoadMenuW miss name=0x{:08x}", nameArg);
        lastError_ = 1814;
        return 0;
    }
    HMENU menu = LoadMenuIndirectW(reinterpret_cast<const MENUTEMPLATEW*>(resource->data.data()));
    if (!menu) {
        spdlog::info("LoadMenuW host LoadMenuIndirectW failed name=0x{:08x} error={}", nameArg, GetLastError());
        lastError_ = GetLastError();
        return 0;
    }
    lastError_ = 0;
    spdlog::info("LoadMenuW hit name=0x{:08x} size={}", nameArg, resource->data.size());
    return makeGuestHandle({GuestHandle::Kind::HostMenu, reinterpret_cast<uintptr_t>(menu), 0});
#else
    lastError_ = resource ? 0 : 1814;
    return 0;
#endif
}

void SyntheticDllRuntime::ensureHostWindow(uint32_t guestHwnd, GuestWindow& window) {
#if defined(_WIN32)
    if (window.parent || !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    if (!window.hostHwnd) {
        if (!registerHostPresenterClass()) {
            spdlog::warn("host presenter RegisterClassW failed error={}", GetLastError());
            return;
        }
        auto* presenter = new HostPresenterWindow{this, guestHwnd, framebuffer_, framebufferWidth_, framebufferHeight_};
        const std::wstring title = widenLossy(window.title.empty() ? "FakeCE" : window.title);
        RECT rect{0, 0, hostPresenterDisplayWidth(*presenter), hostPresenterDisplayHeight(*presenter)};
        AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
        HWND hwnd = CreateWindowExW(0, hostPresenterClassName(), title.c_str(),
                                    WS_OVERLAPPEDWINDOW,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    rect.right - rect.left, rect.bottom - rect.top,
                                    nullptr, nullptr, GetModuleHandleW(nullptr), presenter);
        if (!hwnd) {
            spdlog::warn("host presenter CreateWindowExW failed guest=0x{:08x} error={}", guestHwnd, GetLastError());
            delete presenter;
            return;
        }
        window.hostHwnd = reinterpret_cast<uintptr_t>(hwnd);
        spdlog::info("created host presenter HWND={} for guest HWND=0x{:08x} guest={}x{} framebuffer={}x{}",
                     static_cast<void*>(hwnd), guestHwnd, window.width, window.height,
                     presenter->width, presenter->height);
    }
    HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
    if (window.visible) ShowWindow(hwnd, SW_SHOWNORMAL);
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
#else
    (void)guestHwnd;
    (void)window;
#endif
}

void SyntheticDllRuntime::destroyHostWindow(GuestWindow& window) {
#if defined(_WIN32)
    if (window.hostHwnd) {
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (IsWindow(hwnd)) {
            SetWindowTextW(hwnd, L"FakeCE presenter (guest HWND destroyed)");
            ShowWindow(hwnd, SW_SHOWNORMAL);
            InvalidateRect(hwnd, nullptr, FALSE);
            retainedHostWindows_.push_back(window.hostHwnd);
        }
        window.hostHwnd = 0;
    }
#else
    (void)window;
#endif
}

void SyntheticDllRuntime::invalidateHostWindows() {
#if defined(_WIN32)
    for (auto& [guestHwnd, window] : windows_) {
        (void)guestHwnd;
        if (!window.hostHwnd) continue;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (!IsWindow(hwnd)) continue;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (!hwnd || !IsWindow(hwnd)) continue;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }
#endif
}

void SyntheticDllRuntime::pumpHostMessages() {
#if defined(_WIN32)
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
#endif
}

void SyntheticDllRuntime::enqueueDueTimers() {
    const uint64_t now = hostTickMilliseconds();
    for (auto& [key, timer] : timers_) {
        (void)key;
        if (timer.intervalMs == 0 || now < timer.nextDueMs) continue;
        GuestMessage message{};
        message.hwnd = timer.hwnd;
        message.message = 0x0113; // WM_TIMER
        message.wParam = timer.id;
        message.lParam = timer.callback;
        message.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(message);
        const uint32_t interval = std::max<uint32_t>(1, timer.intervalMs);
        do {
            timer.nextDueMs += interval;
        } while (timer.nextDueMs <= now);
    }
}

uint32_t SyntheticDllRuntime::timerWaitMilliseconds() const {
    if (timers_.empty()) return 50;
    const uint64_t now = hostTickMilliseconds();
    uint64_t best = 50;
    for (const auto& [key, timer] : timers_) {
        (void)key;
        if (timer.nextDueMs <= now) return 1;
        best = std::min<uint64_t>(best, timer.nextDueMs - now);
    }
    return uint32_t(std::max<uint64_t>(1, best));
}

uint32_t SyntheticDllRuntime::windowAtPoint(uint32_t rootGuestHwnd, int32_t x, int32_t y,
                                            int32_t& clientX, int32_t& clientY) const {
    uint32_t best = windows_.count(rootGuestHwnd) ? rootGuestHwnd : 0;
    int32_t bestX = 0;
    int32_t bestY = 0;
    auto belongsToRoot = [&](uint32_t hwnd) {
        if (!rootGuestHwnd) return true;
        for (uint32_t current = hwnd; current;) {
            if (current == rootGuestHwnd) return true;
            auto it = windows_.find(current);
            if (it == windows_.end()) break;
            current = it->second.parent;
        }
        return false;
    };
    auto originOf = [&](uint32_t hwnd) {
        int32_t ox = 0;
        int32_t oy = 0;
        for (uint32_t current = hwnd; current;) {
            auto it = windows_.find(current);
            if (it == windows_.end()) break;
            ox += it->second.x;
            oy += it->second.y;
            current = it->second.parent;
        }
        return std::pair<int32_t, int32_t>{ox, oy};
    };
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
        const uint32_t hwnd = it->first;
        const GuestWindow& window = it->second;
        if (window.destroyed || !window.visible || !belongsToRoot(hwnd)) continue;
        const auto [ox, oy] = originOf(hwnd);
        if (x < ox || y < oy || x >= ox + window.width || y >= oy + window.height) continue;
        best = hwnd;
        bestX = ox;
        bestY = oy;
        break;
    }
    clientX = x - bestX;
    clientY = y - bestY;
    return best;
}

void SyntheticDllRuntime::queueHostMouseMessage(uint32_t rootGuestHwnd, uint32_t message,
                                                int32_t hostX, int32_t hostY) {
    int32_t clientX = hostX;
    int32_t clientY = hostY;
    uint32_t hwnd = 0;
    if (message == 0x0202 && hostPointerCaptureWindow_) {
        auto captured = windows_.find(hostPointerCaptureWindow_);
        if (captured != windows_.end() && !captured->second.destroyed) hwnd = hostPointerCaptureWindow_;
    } else if (capturedWindow_) {
        auto captured = windows_.find(capturedWindow_);
        if (captured != windows_.end() && !captured->second.destroyed) hwnd = capturedWindow_;
    }
    if (!hwnd) {
        hwnd = windowAtPoint(rootGuestHwnd, hostX, hostY, clientX, clientY);
    }
    if (!hwnd) return;
    auto originOf = [&](uint32_t target) {
        int32_t x = 0;
        int32_t y = 0;
        for (uint32_t current = target; current;) {
            auto it = windows_.find(current);
            if (it == windows_.end()) break;
            x += it->second.x;
            y += it->second.y;
            current = it->second.parent;
        }
        return std::pair<int32_t, int32_t>{x, y};
    };
    const auto [originX, originY] = originOf(hwnd);
    clientX = hostX - originX;
    clientY = hostY - originY;
    if (message == 0x0201) {
        hostPointerCaptureWindow_ = hwnd;
        if (focusedWindow_ != hwnd) {
            auto focused = windows_.find(focusedWindow_);
            if (focused != windows_.end() && !focused->second.destroyed) {
                guestMessages_.push_back({focusedWindow_, 0x0008, hwnd, 0, uint32_t(++tick_ * 16), 0, 0});
            }
            guestMessages_.push_back({hwnd, 0x0007, focusedWindow_, 0, uint32_t(++tick_ * 16), 0, 0});
            focusedWindow_ = hwnd;
        }
    } else if (message == 0x0202) {
        hostPointerCaptureWindow_ = 0;
    }
    GuestMessage guest{};
    guest.hwnd = hwnd;
    guest.message = message;
    guest.wParam = message == 0x0201 ? 1 : 0;
    guest.lParam = uint32_t(uint16_t(clientX) | (uint32_t(uint16_t(clientY)) << 16));
    guest.time = uint32_t(++tick_ * 16);
    guest.x = uint32_t(clientX);
    guest.y = uint32_t(clientY);
    guestMessages_.push_back(guest);
    spdlog::info("queued host mouse msg=0x{:04x} root=0x{:08x} hwnd=0x{:08x} point={},{} client={},{} queued={}",
                 message, rootGuestHwnd, hwnd, hostX, hostY, clientX, clientY, guestMessages_.size());
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
    invalidateHostWindows();
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
    invalidateHostWindows();
}

bool SyntheticDllRuntime::drawHostTextToDc(const GuestDc& dc,
                                           int32_t x,
                                           int32_t y,
                                           uint32_t options,
                                           uint32_t rectPtr,
                                           uint32_t textPtr,
                                           int32_t textChars,
                                           uint32_t drawTextFormat,
                                           bool drawTextCall) {
#if defined(_WIN32)
    const bool opaqueRectOnly = !drawTextCall && !textPtr && textChars == 0 && rectPtr && (options & 0x0002u);
    if (!textPtr && !opaqueRectOnly) return false;
    std::wstring text;
    if (textPtr && textChars < 0) {
        for (uint32_t i = 0; i < 0x10000; ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, textPtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK) return false;
            if (!ch) break;
            text.push_back(wchar_t(ch));
        }
    } else if (textPtr && textChars > 0) {
        text.reserve(uint32_t(textChars));
        for (uint32_t i = 0; i < uint32_t(textChars); ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, textPtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK) return false;
            text.push_back(wchar_t(ch));
        }
    }
    if (text.empty() && !drawTextCall && !opaqueRectOnly) return true;

    int32_t originX = 0;
    int32_t originY = 0;
    if (!dc.selectedBitmap) {
        auto window = windows_.find(dc.hwnd);
        if (window != windows_.end()) {
            originX = window->second.x;
            originY = window->second.y;
        }
    }

    RECT textRect{};
    RECT* rectArg = nullptr;
    if (rectPtr) {
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        if (!readGuestRect(rectPtr, left, top, right, bottom)) return false;
        textRect = RECT{left + originX, top + originY, right + originX, bottom + originY};
        rectArg = &textRect;
    }

    bool hostFontOwned = false;
    auto selectedFont = [&]() -> HFONT {
        LOGFONTW logFont{};
        auto font = fonts_.find(dc.selectedFont);
        if (font == fonts_.end() || font->second.stock) {
            HFONT stockFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            if (!stockFont || GetObjectW(stockFont, sizeof(logFont), &logFont) != sizeof(logFont)) {
                return stockFont;
            }
        } else {
            const size_t bytes = std::min(sizeof(logFont), font->second.logFont.size());
            std::memcpy(&logFont, font->second.logFont.data(), bytes);
        }
        logFont.lfQuality = NONANTIALIASED_QUALITY;
        HFONT hostFont = CreateFontIndirectW(&logFont);
        if (hostFont) {
            hostFontOwned = true;
            return hostFont;
        }
        return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    };

    auto drawIntoDib = [&](int width, int height, uint32_t* pixels) -> bool {
        if (!pixels || width <= 0 || height <= 0) return false;
        HDC screen = GetDC(nullptr);
        if (!screen) return false;
        HDC memDc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!memDc) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* dibBits = nullptr;
        HBITMAP dib = CreateDIBSection(memDc, &info, DIB_RGB_COLORS, &dibBits, nullptr, 0);
        if (!dib || !dibBits) {
            if (dib) DeleteObject(dib);
            DeleteDC(memDc);
            return false;
        }
        std::memcpy(dibBits, pixels, size_t(width) * size_t(height) * 4);
        HGDIOBJ oldBitmap = SelectObject(memDc, dib);
        HFONT hostFont = selectedFont();
        HGDIOBJ oldFont = hostFont ? SelectObject(memDc, hostFont) : nullptr;
        SetTextColor(memDc, COLORREF(dc.textColor));
        SetBkColor(memDc, COLORREF(dc.bkColor));
        SetBkMode(memDc, dc.bkMode == 1 ? TRANSPARENT : OPAQUE);
        BOOL ok = FALSE;
        if (drawTextCall) {
            RECT drawRect = rectArg ? *rectArg : RECT{x + originX, y + originY, width, height};
            ok = DrawTextW(memDc, text.c_str(), int(text.size()), &drawRect, drawTextFormat) != 0;
        } else {
            ok = ExtTextOutW(memDc, x + originX, y + originY, options, rectArg,
                             text.c_str(), UINT(text.size()), nullptr);
        }
        GdiFlush();
        if (ok) std::memcpy(pixels, dibBits, size_t(width) * size_t(height) * 4);
        if (oldFont) SelectObject(memDc, oldFont);
        if (hostFont && hostFontOwned) {
            DeleteObject(hostFont);
        }
        SelectObject(memDc, oldBitmap);
        DeleteObject(dib);
        DeleteDC(memDc);
        return ok != FALSE;
    };

    auto dstBitmap = bitmaps_.find(dc.selectedBitmap);
    if (dstBitmap != bitmaps_.end()) {
        GuestBitmap& bitmap = dstBitmap->second;
        if (!bitmap.bits || bitmap.width <= 0 || bitmap.heightRaw == 0 || bitmap.stride == 0) return false;
        const int32_t height = std::abs(bitmap.heightRaw);
        const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
        if (!byteCount || byteCount > 0x2000000ull) return false;
        std::vector<uint8_t> raw(static_cast<size_t>(byteCount));
        if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK) return false;
        std::vector<uint32_t> pixels(size_t(bitmap.width) * size_t(height));
        for (int32_t py = 0; py < height; ++py) {
            for (int32_t px = 0; px < bitmap.width; ++px) {
                uint32_t pixel = 0xffffffffu;
                readBitmapPixel(bitmap, raw, height, px, py, pixel);
                pixels[size_t(py) * size_t(bitmap.width) + size_t(px)] = pixel;
            }
        }
        if (!drawIntoDib(bitmap.width, height, pixels.data())) return false;
        for (int32_t py = 0; py < height; ++py) {
            for (int32_t px = 0; px < bitmap.width; ++px) {
                writeBitmapPixel(bitmap, raw, height, px, py,
                                 pixels[size_t(py) * size_t(bitmap.width) + size_t(px)] | 0xff000000u);
            }
        }
        return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
    }

    if (!framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return false;
    const bool ok = drawIntoDib(framebufferWidth_, framebufferHeight_, framebuffer_);
    if (ok) invalidateHostWindows();
    return ok;
#else
    (void)dc;
    (void)x;
    (void)y;
    (void)options;
    (void)rectPtr;
    (void)textPtr;
    (void)textChars;
    (void)drawTextFormat;
    (void)drawTextCall;
    return false;
#endif
}

bool SyntheticDllRuntime::readBitmapPixel(const GuestBitmap& bitmap,
                                          const std::vector<uint8_t>& bits,
                                          int32_t height,
                                          int32_t x,
                                          int32_t y,
                                          uint32_t& pixel) const {
    if (x < 0 || x >= bitmap.width || y < 0 || y >= height || bitmap.stride == 0) return false;
    const int32_t rowIndex = bitmap.heightRaw < 0 ? y : (height - 1 - y);
    const uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
    if (bitmap.bpp == 32) {
        const size_t o = size_t(x) * 4;
        pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
    } else if (bitmap.bpp == 24) {
        const size_t o = size_t(x) * 3;
        pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
    } else if (bitmap.bpp == 16) {
        const size_t o = size_t(x) * 2;
        const uint16_t v = uint16_t(row[o] | (row[o + 1] << 8));
        pixel = decodeBitmap16(v, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
    } else if (bitmap.bpp == 8 && !bitmap.palette.empty()) {
        pixel = bitmap.palette[std::min<size_t>(row[x], bitmap.palette.size() - 1)];
    } else if (bitmap.bpp == 4 && !bitmap.palette.empty()) {
        const uint8_t packed = row[size_t(x) / 2];
        const uint8_t index = uint8_t((x & 1) ? (packed & 0x0f) : (packed >> 4));
        pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
    } else if (bitmap.bpp == 1 && !bitmap.palette.empty()) {
        const uint8_t packed = row[size_t(x) / 8];
        const uint8_t index = uint8_t((packed >> (7 - (x & 7))) & 1);
        pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
    } else {
        return false;
    }
    return true;
}

bool SyntheticDllRuntime::writeBitmapPixel(const GuestBitmap& bitmap,
                                           std::vector<uint8_t>& bits,
                                           int32_t height,
                                           int32_t x,
                                           int32_t y,
                                           uint32_t pixel) const {
    if (x < 0 || x >= bitmap.width || y < 0 || y >= height || bitmap.stride == 0) return false;
    const int32_t rowIndex = bitmap.heightRaw < 0 ? y : (height - 1 - y);
    uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
    const uint8_t r = uint8_t((pixel >> 16) & 0xff);
    const uint8_t g = uint8_t((pixel >> 8) & 0xff);
    const uint8_t b = uint8_t(pixel & 0xff);
    if (bitmap.bpp == 32) {
        const size_t o = size_t(x) * 4;
        row[o + 0] = b;
        row[o + 1] = g;
        row[o + 2] = r;
        row[o + 3] = 0xff;
    } else if (bitmap.bpp == 24) {
        const size_t o = size_t(x) * 3;
        row[o + 0] = b;
        row[o + 1] = g;
        row[o + 2] = r;
    } else if (bitmap.bpp == 16) {
        const uint16_t v = encodeBitmap16(pixel, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
        const size_t o = size_t(x) * 2;
        row[o + 0] = uint8_t(v & 0xff);
        row[o + 1] = uint8_t(v >> 8);
    } else if ((bitmap.bpp == 8 || bitmap.bpp == 4 || bitmap.bpp == 1) && !bitmap.palette.empty()) {
        uint32_t bestIndex = 0;
        uint32_t bestDistance = UINT32_MAX;
        const uint32_t maxColors = bitmap.bpp == 8 ? 256u : (bitmap.bpp == 4 ? 16u : 2u);
        const uint32_t limit = std::min<uint32_t>(maxColors, uint32_t(bitmap.palette.size()));
        for (uint32_t i = 0; i < limit; ++i) {
            const uint32_t p = bitmap.palette[i];
            const int32_t dr = int32_t((p >> 16) & 0xff) - int32_t(r);
            const int32_t dg = int32_t((p >> 8) & 0xff) - int32_t(g);
            const int32_t db = int32_t(p & 0xff) - int32_t(b);
            const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
            if (distance < bestDistance) {
                bestDistance = distance;
                bestIndex = i;
                if (!distance) break;
            }
        }
        if (bitmap.bpp == 8) {
            row[x] = uint8_t(bestIndex);
        } else if (bitmap.bpp == 4) {
            uint8_t& packed = row[size_t(x) / 2];
            if (x & 1) {
                packed = uint8_t((packed & 0xf0) | (bestIndex & 0x0f));
            } else {
                packed = uint8_t((packed & 0x0f) | ((bestIndex & 0x0f) << 4));
            }
        } else {
            uint8_t& packed = row[size_t(x) / 8];
            const uint8_t mask = uint8_t(1u << (7 - (x & 7)));
            packed = bestIndex ? uint8_t(packed | mask) : uint8_t(packed & ~mask);
        }
    } else {
        return false;
    }
    return true;
}

void SyntheticDllRuntime::dumpGuestBitmapPpm(uint32_t bitmapHandle,
                                             const GuestBitmap& bitmap,
                                             const std::string& tag) {
    const int32_t height = std::abs(bitmap.heightRaw);
    if (bitmap.width <= 0 || height <= 0 || !bitmap.bits || !bitmap.stride) return;
    const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
    if (!byteCount || byteCount > 0x2000000ull) return;
    std::vector<uint8_t> bits(static_cast<size_t>(byteCount));
    if (uc_mem_read(uc_, bitmap.bits, bits.data(), bits.size()) != UC_ERR_OK) return;

    const uint32_t sequence = ++splashBlitDumpCounter_;
    char path[160]{};
    std::snprintf(path, sizeof(path), "frame_probe_splash_%02u_%s_bitmap_%08x.ppm",
                  sequence, tag.c_str(), bitmapHandle);
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "P6\n" << bitmap.width << " " << height << "\n255\n";
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < bitmap.width; ++x) {
            uint32_t pixel = 0;
            if (!readBitmapPixel(bitmap, bits, height, x, y, pixel)) pixel = 0xffff00ffu;
            const uint8_t rgb[3]{
                uint8_t((pixel >> 16) & 0xff),
                uint8_t((pixel >> 8) & 0xff),
                uint8_t(pixel & 0xff),
            };
            out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
    }
    spdlog::info("dumped splash source bitmap handle=0x{:08x} tag={} file={}",
                 bitmapHandle, tag, path);
}

void SyntheticDllRuntime::dumpFramebufferPpm(const std::string& tag) {
    if (!framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    char path[160]{};
    std::snprintf(path, sizeof(path), "frame_probe_splash_%02u_%s_frame.ppm",
                  splashBlitDumpCounter_, tag.c_str());
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    out << "P6\n" << framebufferWidth_ << " " << framebufferHeight_ << "\n255\n";
    for (int32_t y = 0; y < framebufferHeight_; ++y) {
        const uint32_t* row = framebuffer_ + size_t(y) * size_t(framebufferWidth_);
        for (int32_t x = 0; x < framebufferWidth_; ++x) {
            const uint32_t pixel = row[x];
            const uint8_t rgb[3]{
                uint8_t((pixel >> 16) & 0xff),
                uint8_t((pixel >> 8) & 0xff),
                uint8_t(pixel & 0xff),
            };
            out.write(reinterpret_cast<const char*>(rgb), sizeof(rgb));
        }
    }
    spdlog::info("dumped splash framebuffer tag={} file={}", tag, path);
}

bool SyntheticDllRuntime::handleCreateBitmap(const GuestCallArgs& args, uint32_t& ret) {
    const int32_t width = int32_t(args.a0);
    const int32_t height = int32_t(args.a1);
    const uint32_t planes = args.a2 ? args.a2 : 1;
    const uint32_t bpp = planes * (args.a3 ? args.a3 : 1);
    const uint32_t srcBits = stackArg(4);
    if (width <= 0 || height == 0 || (bpp != 1 && bpp != 4 && bpp != 8 &&
                                      bpp != 16 && bpp != 24 && bpp != 32)) {
        lastError_ = 87;
        ret = 0;
        return true;
    }

    const uint32_t absHeight = uint32_t(height < 0 ? -height : height);
    const uint32_t stride = ((uint32_t(width) * bpp + 31) / 32) * 4;
    const uint32_t byteCount = std::max<uint32_t>(stride * absHeight, 4);
    const uint32_t bits = allocate(byteCount, true);
    ret = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, bits});
    if (ret) {
        if (srcBits) {
            std::vector<uint8_t> raw(byteCount);
            if (uc_mem_read(uc_, srcBits, raw.data(), raw.size()) == UC_ERR_OK) {
                uc_mem_write(uc_, bits, raw.data(), raw.size());
            }
        }
        GuestBitmap bitmap{};
        bitmap.width = width;
        bitmap.heightRaw = height < 0 ? height : -height;
        bitmap.bpp = uint16_t(bpp);
        bitmap.stride = stride;
        bitmap.bits = bits;
        if (bitmap.bpp == 16) ceDefault16BitMasks(bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
        bitmap.palette = defaultIndexedPalette(uint16_t(bpp));
        bitmaps_[ret] = std::move(bitmap);
    }
    lastError_ = ret ? 0 : 8;
    spdlog::info("CreateBitmap {}x{} planes={} bpp={} bits=0x{:08x} bitmap=0x{:08x}",
                 width, height, planes, bpp, bits, ret);
    return true;
}

bool SyntheticDllRuntime::handleGetObjectW(const GuestCallArgs& args, uint32_t& ret) {
    auto bitmap = bitmaps_.find(args.a0);
    if (bitmap == bitmaps_.end()) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (!args.a2) {
        lastError_ = 0;
        ret = 24;
        return true;
    }

    const GuestBitmap& bm = bitmap->second;
    const int32_t height = bm.heightRaw < 0 ? -bm.heightRaw : bm.heightRaw;
    std::array<uint8_t, 24> raw{};
    auto putU32 = [&](uint32_t offset, uint32_t value) {
        raw[offset + 0] = uint8_t(value & 0xff);
        raw[offset + 1] = uint8_t((value >> 8) & 0xff);
        raw[offset + 2] = uint8_t((value >> 16) & 0xff);
        raw[offset + 3] = uint8_t((value >> 24) & 0xff);
    };
    auto putU16 = [&](uint32_t offset, uint16_t value) {
        raw[offset + 0] = uint8_t(value & 0xff);
        raw[offset + 1] = uint8_t(value >> 8);
    };
    putU32(0, 0);
    putU32(4, uint32_t(bm.width));
    putU32(8, uint32_t(height));
    putU32(12, bm.stride);
    putU16(16, 1);
    putU16(18, bm.bpp);
    putU32(20, bm.bits);
    const uint32_t bytes = std::min<uint32_t>(args.a1, uint32_t(raw.size()));
    if (bytes && uc_mem_write(uc_, args.a2, raw.data(), bytes) != UC_ERR_OK) {
        lastError_ = 998;
        ret = 0;
    } else {
        lastError_ = 0;
        ret = bytes;
    }
    return true;
}

bool SyntheticDllRuntime::handleSetDIBColorTable(const GuestCallArgs& args, uint32_t& ret) {
    GuestDc* dc = lookupGuestDc(args.a0);
    auto bitmap = dc ? bitmaps_.find(dc->selectedBitmap) : bitmaps_.end();
    if (!dc || bitmap == bitmaps_.end() || !args.a3 || bitmap->second.bpp > 8) {
        lastError_ = dc ? 87 : 6;
        ret = 0;
        return true;
    }

    GuestBitmap& bm = bitmap->second;
    const uint32_t maxColors = 1u << bm.bpp;
    const uint32_t start = std::min<uint32_t>(args.a1, maxColors);
    const uint32_t count = std::min<uint32_t>(args.a2, maxColors - start);
    if (bm.palette.empty()) bm.palette = defaultIndexedPalette(bm.bpp);
    if (bm.palette.size() < maxColors) bm.palette.resize(maxColors, 0xff000000u);

    std::vector<uint8_t> raw(size_t(count) * 4);
    if (count && uc_mem_read(uc_, args.a3, raw.data(), raw.size()) != UC_ERR_OK) {
        lastError_ = 998;
        ret = 0;
        return true;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t b = raw[size_t(i) * 4 + 0];
        const uint8_t g = raw[size_t(i) * 4 + 1];
        const uint8_t r = raw[size_t(i) * 4 + 2];
        bm.palette[size_t(start + i)] =
            0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }
    lastError_ = 0;
    ret = count;
    spdlog::info("SetDIBColorTable hdc=0x{:08x} bitmap=0x{:08x} start={} count={} ret={}",
                 args.a0, dc->selectedBitmap, args.a1, args.a2, ret);
    return true;
}

bool SyntheticDllRuntime::handleSetBitmapBits(const GuestCallArgs& args, uint32_t& ret) {
    auto bitmap = bitmaps_.find(args.a0);
    if (bitmap == bitmaps_.end() || !args.a2) {
        lastError_ = bitmap == bitmaps_.end() ? 6 : 87;
        ret = 0;
        return true;
    }

    GuestBitmap& bm = bitmap->second;
    const uint32_t height = uint32_t(bm.heightRaw < 0 ? -bm.heightRaw : bm.heightRaw);
    const uint32_t byteCount = std::min<uint32_t>(args.a1, bm.stride * height);
    std::vector<uint8_t> raw(byteCount);
    if (byteCount && uc_mem_read(uc_, args.a2, raw.data(), raw.size()) != UC_ERR_OK) {
        lastError_ = 998;
        ret = 0;
    } else if (byteCount && uc_mem_write(uc_, bm.bits, raw.data(), raw.size()) != UC_ERR_OK) {
        lastError_ = 998;
        ret = 0;
    } else {
        lastError_ = 0;
        ret = byteCount;
    }
    spdlog::info("SetBitmapBits bitmap=0x{:08x} requested={} copied={}", args.a0, args.a1, ret);
    return true;
}

bool SyntheticDllRuntime::handleSetDIBitsToDevice(const GuestCallArgs& args, uint32_t& ret) {
    GuestDc* dc = lookupGuestDc(args.a0);
    if (!dc) {
        lastError_ = 6;
        ret = 0;
        return true;
    }

    const int32_t dstW = int32_t(args.a3);
    const int32_t dstH = int32_t(stackArg(4));
    const int32_t scanLines = int32_t(stackArg(8) ? stackArg(8) : stackArg(4));
    const bool supported = stackArg(11) == 0;
    const int32_t srcX = int32_t(stackArg(5));
    const int32_t srcY = int32_t(stackArg(6) + stackArg(7));
    auto dstBitmap = bitmaps_.find(dc->selectedBitmap);
    const bool ok = supported && (dstBitmap != bitmaps_.end()
        ? stretchDibToBitmap(dstBitmap->second, int32_t(args.a1), int32_t(args.a2), dstW, scanLines,
                             srcX, srcY, dstW, scanLines, stackArg(9), stackArg(10))
        : stretchDibToFramebuffer(*dc, int32_t(args.a1), int32_t(args.a2), dstW, scanLines,
                                  srcX, srcY, dstW, scanLines, stackArg(9), stackArg(10)));
    if (ok) {
        ret = uint32_t(std::abs(scanLines));
        lastError_ = 0;
    } else {
        spdlog::info("SetDIBitsToDevice failed dst=0x{:08x} dstBitmap=0x{:08x} "
                     "dst={}x{} srcOrigin={},{} startScan={} scanLines={} bits=0x{:08x} "
                     "info=0x{:08x} usage={}",
                     args.a0, dc->selectedBitmap, dstW, dstH, srcX, int32_t(stackArg(6)),
                     stackArg(7), stackArg(8), stackArg(9), stackArg(10), stackArg(11));
        lastError_ = 120;
        ret = 0;
    }
    return true;
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
    if (headerSize < 40 || planes != 1 || (compression != 0 && compression != 3) ||
        dibWidth <= 0 || dibHeightRaw == 0) {
        return false;
    }
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    if (bpp == 16) {
        ceDefault16BitMasks(redMask, greenMask, blueMask);
        if (compression == 3) {
            const uint32_t maskOffset = headerSize >= 52 ? 40 : headerSize;
            redMask = readU32(infoPtr + maskOffset);
            greenMask = readU32(infoPtr + maskOffset + 4);
            blueMask = readU32(infoPtr + maskOffset + 8);
        }
    }
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
                pixel = decodeBitmap16(v, redMask, greenMask, blueMask);
            } else if (bpp == 8 && !palette.empty()) {
                pixel = palette[std::min<size_t>(p[sx], palette.size() - 1)];
            } else if (bpp == 4 && !palette.empty()) {
                const uint8_t packed = p[size_t(sx) / 2];
                const uint8_t index = uint8_t((sx & 1) ? (packed & 0x0f) : (packed >> 4));
                pixel = palette[std::min<size_t>(index, palette.size() - 1)];
            } else if (bpp == 1 && !palette.empty()) {
                const uint8_t packed = p[size_t(sx) / 8];
                const uint8_t index = uint8_t((packed >> (7 - (sx & 7))) & 1);
                pixel = palette[std::min<size_t>(index, palette.size() - 1)];
            } else {
                return false;
            }
            framebuffer_[size_t(dstPy) * size_t(framebufferWidth_) + size_t(dstPx)] = pixel;
        }
    }
    invalidateHostWindows();
    return true;
}

bool SyntheticDllRuntime::stretchDibToBitmap(const GuestBitmap& dstBitmap,
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
    if (!dstBitmap.bits || dstBitmap.width <= 0 || dstBitmap.heightRaw == 0 || dstBitmap.stride == 0 ||
        !bitsPtr || !infoPtr || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
        return false;
    }

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
    if (headerSize < 40 || planes != 1 || (compression != 0 && compression != 3) ||
        dibWidth <= 0 || dibHeightRaw == 0) {
        return false;
    }
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    if (bpp == 16) {
        ceDefault16BitMasks(redMask, greenMask, blueMask);
        if (compression == 3) {
            const uint32_t maskOffset = headerSize >= 52 ? 40 : headerSize;
            redMask = readU32(infoPtr + maskOffset);
            greenMask = readU32(infoPtr + maskOffset + 4);
            blueMask = readU32(infoPtr + maskOffset + 8);
        }
    }

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

    const uint32_t srcStride = ((uint32_t(dibWidth) * uint32_t(bpp) + 31u) / 32u) * 4u;
    const int32_t dstHeight = std::abs(dstBitmap.heightRaw);
    const uint64_t srcBytes = uint64_t(srcStride) * uint64_t(dibHeight);
    const uint64_t dstBytes = uint64_t(dstBitmap.stride) * uint64_t(dstHeight);
    if (!srcStride || !srcBytes || !dstBytes || srcBytes > 0x2000000ull || dstBytes > 0x2000000ull) return false;

    std::vector<uint8_t> srcBits(static_cast<size_t>(srcBytes));
    std::vector<uint8_t> dstBits(static_cast<size_t>(dstBytes));
    if (uc_mem_read(uc_, bitsPtr, srcBits.data(), srcBits.size()) != UC_ERR_OK ||
        uc_mem_read(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) != UC_ERR_OK) {
        return false;
    }

    auto readDibPixel = [&](int32_t x, int32_t y, uint32_t& pixel) -> bool {
        if (x < 0 || x >= dibWidth || y < 0 || y >= dibHeight) return false;
        const int32_t rowIndex = topDown ? y : (dibHeight - 1 - y);
        const uint8_t* row = srcBits.data() + size_t(rowIndex) * size_t(srcStride);
        if (bpp == 32) {
            const size_t o = size_t(x) * 4;
            pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
        } else if (bpp == 24) {
            const size_t o = size_t(x) * 3;
            pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
        } else if (bpp == 16) {
            const size_t o = size_t(x) * 2;
            const uint16_t v = uint16_t(row[o] | (row[o + 1] << 8));
            pixel = decodeBitmap16(v, redMask, greenMask, blueMask);
        } else if (bpp == 8 && !palette.empty()) {
            pixel = palette[std::min<size_t>(row[x], palette.size() - 1)];
        } else if (bpp == 4 && !palette.empty()) {
            const uint8_t packed = row[size_t(x) / 2];
            const uint8_t index = uint8_t((x & 1) ? (packed & 0x0f) : (packed >> 4));
            pixel = palette[std::min<size_t>(index, palette.size() - 1)];
        } else if (bpp == 1 && !palette.empty()) {
            const uint8_t packed = row[size_t(x) / 8];
            const uint8_t index = uint8_t((packed >> (7 - (x & 7))) & 1);
            pixel = palette[std::min<size_t>(index, palette.size() - 1)];
        } else {
            return false;
        }
        return true;
    };

    auto writeDestPixel = [&](int32_t x, int32_t y, uint32_t pixel) -> bool {
        if (x < 0 || x >= dstBitmap.width || y < 0 || y >= dstHeight) return false;
        const int32_t rowIndex = dstBitmap.heightRaw < 0 ? y : (dstHeight - 1 - y);
        uint8_t* row = dstBits.data() + size_t(rowIndex) * size_t(dstBitmap.stride);
        const uint8_t r = uint8_t((pixel >> 16) & 0xff);
        const uint8_t g = uint8_t((pixel >> 8) & 0xff);
        const uint8_t b = uint8_t(pixel & 0xff);
        if (dstBitmap.bpp == 32) {
            const size_t o = size_t(x) * 4;
            row[o + 0] = b;
            row[o + 1] = g;
            row[o + 2] = r;
            row[o + 3] = 0xff;
        } else if (dstBitmap.bpp == 24) {
            const size_t o = size_t(x) * 3;
            row[o + 0] = b;
            row[o + 1] = g;
            row[o + 2] = r;
        } else if (dstBitmap.bpp == 16) {
            const uint16_t v = encodeBitmap16(pixel, dstBitmap.redMask, dstBitmap.greenMask, dstBitmap.blueMask);
            const size_t o = size_t(x) * 2;
            row[o + 0] = uint8_t(v & 0xff);
            row[o + 1] = uint8_t(v >> 8);
        } else if (dstBitmap.bpp == 8 && !dstBitmap.palette.empty()) {
            uint32_t bestIndex = 0;
            uint32_t bestDistance = UINT32_MAX;
            for (uint32_t i = 0; i < dstBitmap.palette.size(); ++i) {
                const uint32_t p = dstBitmap.palette[i];
                const int32_t dr = int32_t((p >> 16) & 0xff) - int32_t(r);
                const int32_t dg = int32_t((p >> 8) & 0xff) - int32_t(g);
                const int32_t db = int32_t(p & 0xff) - int32_t(b);
                const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                    if (!distance) break;
                }
            }
            row[x] = uint8_t(bestIndex);
        } else if ((dstBitmap.bpp == 4 || dstBitmap.bpp == 1) && !dstBitmap.palette.empty()) {
            uint32_t bestIndex = 0;
            uint32_t bestDistance = UINT32_MAX;
            const uint32_t maxColors = dstBitmap.bpp == 4 ? 16u : 2u;
            const uint32_t limit = std::min<uint32_t>(maxColors, uint32_t(dstBitmap.palette.size()));
            for (uint32_t i = 0; i < limit; ++i) {
                const uint32_t p = dstBitmap.palette[i];
                const int32_t dr = int32_t((p >> 16) & 0xff) - int32_t(r);
                const int32_t dg = int32_t((p >> 8) & 0xff) - int32_t(g);
                const int32_t db = int32_t(p & 0xff) - int32_t(b);
                const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                    if (!distance) break;
                }
            }
            if (dstBitmap.bpp == 4) {
                uint8_t& packed = row[size_t(x) / 2];
                if (x & 1) {
                    packed = uint8_t((packed & 0xf0) | (bestIndex & 0x0f));
                } else {
                    packed = uint8_t((packed & 0x0f) | ((bestIndex & 0x0f) << 4));
                }
            } else {
                uint8_t& packed = row[size_t(x) / 8];
                const uint8_t mask = uint8_t(1u << (7 - (x & 7)));
                packed = bestIndex ? uint8_t(packed | mask) : uint8_t(packed & ~mask);
            }
        } else {
            return false;
        }
        return true;
    };

    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    const int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    const int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            uint32_t pixel = 0;
            if (readDibPixel(sx, sy, pixel)) {
                writeDestPixel(outLeft + x, outTop + y, pixel);
            }
        }
    }
    return uc_mem_write(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::bitBltToFramebuffer(const GuestDc& dstDc,
                                              const GuestBitmap& bitmap,
                                              int32_t dstX,
                                              int32_t dstY,
                                              int32_t dstW,
                                              int32_t dstH,
                                              int32_t srcX,
                                              int32_t srcY,
                                              int32_t srcW,
                                              int32_t srcH,
                                              uint32_t rop) {
    if (!framebuffer_ || !bitmap.bits || bitmap.width <= 0 || bitmap.heightRaw == 0 ||
        bitmap.stride == 0 || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
        return false;
    }
    if (!supportedSourceRasterOp(rop)) return false;
    const int32_t bitmapHeight = std::abs(bitmap.heightRaw);
    const bool topDown = bitmap.heightRaw < 0;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    if (outW <= 0 || outH <= 0) return false;

    const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(bitmapHeight);
    if (byteCount == 0 || byteCount > 0x2000000ull) return false;
    std::vector<uint8_t> bits(static_cast<size_t>(byteCount));
    if (uc_mem_read(uc_, bitmap.bits, bits.data(), bits.size()) != UC_ERR_OK) return false;

    int32_t originX = 0;
    int32_t originY = 0;
    auto window = windows_.find(dstDc.hwnd);
    if (window != windows_.end()) {
        originX = window->second.x;
        originY = window->second.y;
    }
    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    outLeft += originX;
    outTop += originY;

    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < 0 || dstPy >= framebufferHeight_) continue;
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        if (sy < 0 || sy >= bitmapHeight) continue;
        const int32_t rowIndex = topDown ? sy : (bitmapHeight - 1 - sy);
        const uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < 0 || dstPx >= framebufferWidth_) continue;
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            if (sx < 0 || sx >= bitmap.width) continue;

            uint32_t pixel = 0;
            if (bitmap.bpp == 32) {
                const size_t o = size_t(sx) * 4;
                pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) |
                        (uint32_t(row[o + 1]) << 8) | row[o];
            } else if (bitmap.bpp == 24) {
                const size_t o = size_t(sx) * 3;
                pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) |
                        (uint32_t(row[o + 1]) << 8) | row[o];
            } else if (bitmap.bpp == 16) {
                const size_t o = size_t(sx) * 2;
                const uint16_t v = uint16_t(row[o] | (row[o + 1] << 8));
                pixel = decodeBitmap16(v, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
            } else if (bitmap.bpp == 8 && !bitmap.palette.empty()) {
                pixel = bitmap.palette[std::min<size_t>(row[sx], bitmap.palette.size() - 1)];
            } else if (bitmap.bpp == 4 && !bitmap.palette.empty()) {
                const uint8_t packed = row[size_t(sx) / 2];
                const uint8_t index = uint8_t((sx & 1) ? (packed & 0x0f) : (packed >> 4));
                pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
            } else if (bitmap.bpp == 1 && !bitmap.palette.empty()) {
                const uint8_t packed = row[size_t(sx) / 8];
                const uint8_t index = uint8_t((packed >> (7 - (sx & 7))) & 1);
                pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
            } else {
                return false;
            }
            uint32_t& dstPixel = framebuffer_[size_t(dstPy) * size_t(framebufferWidth_) + size_t(dstPx)];
            dstPixel = applySourceRasterOp(rop, pixel, dstPixel);
        }
    }
    invalidateHostWindows();
    return true;
}

bool SyntheticDllRuntime::bitBltToBitmap(const GuestBitmap& dstBitmap,
                                         const GuestBitmap& srcBitmap,
                                         int32_t dstX,
                                         int32_t dstY,
                                         int32_t dstW,
                                         int32_t dstH,
                                         int32_t srcX,
                                         int32_t srcY,
                                         int32_t srcW,
                                         int32_t srcH,
                                         uint32_t rop) {
    if (!dstBitmap.bits || !srcBitmap.bits || dstBitmap.width <= 0 || srcBitmap.width <= 0 ||
        dstBitmap.heightRaw == 0 || srcBitmap.heightRaw == 0 || dstBitmap.stride == 0 ||
        srcBitmap.stride == 0 || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
        return false;
    }
    if (!supportedSourceRasterOp(rop)) return false;
    const int32_t dstHeight = std::abs(dstBitmap.heightRaw);
    const int32_t srcHeight = std::abs(srcBitmap.heightRaw);
    const uint64_t dstBytes = uint64_t(dstBitmap.stride) * uint64_t(dstHeight);
    const uint64_t srcBytes = uint64_t(srcBitmap.stride) * uint64_t(srcHeight);
    if (!dstBytes || !srcBytes || dstBytes > 0x2000000ull || srcBytes > 0x2000000ull) return false;

    std::vector<uint8_t> dstBits(static_cast<size_t>(dstBytes));
    std::vector<uint8_t> srcBits(static_cast<size_t>(srcBytes));
    if (uc_mem_read(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) != UC_ERR_OK ||
        uc_mem_read(uc_, srcBitmap.bits, srcBits.data(), srcBits.size()) != UC_ERR_OK) {
        return false;
    }

    auto readBitmapPixel = [](const GuestBitmap& bitmap, const std::vector<uint8_t>& bits,
                              int32_t height, int32_t x, int32_t y, uint32_t& pixel) -> bool {
        if (x < 0 || x >= bitmap.width || y < 0 || y >= height) return false;
        const int32_t rowIndex = bitmap.heightRaw < 0 ? y : (height - 1 - y);
        const uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
        if (bitmap.bpp == 32) {
            const size_t o = size_t(x) * 4;
            pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
        } else if (bitmap.bpp == 24) {
            const size_t o = size_t(x) * 3;
            pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
        } else if (bitmap.bpp == 16) {
            const size_t o = size_t(x) * 2;
            const uint16_t v = uint16_t(row[o] | (row[o + 1] << 8));
            pixel = decodeBitmap16(v, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
        } else if (bitmap.bpp == 8 && !bitmap.palette.empty()) {
            pixel = bitmap.palette[std::min<size_t>(row[x], bitmap.palette.size() - 1)];
        } else if (bitmap.bpp == 4 && !bitmap.palette.empty()) {
            const uint8_t packed = row[size_t(x) / 2];
            const uint8_t index = uint8_t((x & 1) ? (packed & 0x0f) : (packed >> 4));
            pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
        } else if (bitmap.bpp == 1 && !bitmap.palette.empty()) {
            const uint8_t packed = row[size_t(x) / 8];
            const uint8_t index = uint8_t((packed >> (7 - (x & 7))) & 1);
            pixel = bitmap.palette[std::min<size_t>(index, bitmap.palette.size() - 1)];
        } else {
            return false;
        }
        return true;
    };

    auto writeDestPixel = [&](int32_t x, int32_t y, uint32_t pixel) -> bool {
        if (x < 0 || x >= dstBitmap.width || y < 0 || y >= dstHeight) return false;
        const int32_t rowIndex = dstBitmap.heightRaw < 0 ? y : (dstHeight - 1 - y);
        uint8_t* row = dstBits.data() + size_t(rowIndex) * size_t(dstBitmap.stride);
        const uint8_t r = uint8_t((pixel >> 16) & 0xff);
        const uint8_t g = uint8_t((pixel >> 8) & 0xff);
        const uint8_t b = uint8_t(pixel & 0xff);
        if (dstBitmap.bpp == 32) {
            const size_t o = size_t(x) * 4;
            row[o + 0] = b;
            row[o + 1] = g;
            row[o + 2] = r;
            row[o + 3] = 0xff;
        } else if (dstBitmap.bpp == 24) {
            const size_t o = size_t(x) * 3;
            row[o + 0] = b;
            row[o + 1] = g;
            row[o + 2] = r;
        } else if (dstBitmap.bpp == 16) {
            const uint16_t v = encodeBitmap16(pixel, dstBitmap.redMask, dstBitmap.greenMask, dstBitmap.blueMask);
            const size_t o = size_t(x) * 2;
            row[o + 0] = uint8_t(v & 0xff);
            row[o + 1] = uint8_t(v >> 8);
        } else if (dstBitmap.bpp == 8 && !dstBitmap.palette.empty()) {
            uint32_t bestIndex = 0;
            uint32_t bestDistance = UINT32_MAX;
            for (uint32_t i = 0; i < dstBitmap.palette.size(); ++i) {
                const uint32_t p = dstBitmap.palette[i];
                const int32_t dr = int32_t((p >> 16) & 0xff) - int32_t(r);
                const int32_t dg = int32_t((p >> 8) & 0xff) - int32_t(g);
                const int32_t db = int32_t(p & 0xff) - int32_t(b);
                const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                    if (!distance) break;
                }
            }
            row[x] = uint8_t(bestIndex);
        } else if ((dstBitmap.bpp == 4 || dstBitmap.bpp == 1) && !dstBitmap.palette.empty()) {
            uint32_t bestIndex = 0;
            uint32_t bestDistance = UINT32_MAX;
            const uint32_t maxColors = dstBitmap.bpp == 4 ? 16u : 2u;
            const uint32_t limit = std::min<uint32_t>(maxColors, uint32_t(dstBitmap.palette.size()));
            for (uint32_t i = 0; i < limit; ++i) {
                const uint32_t p = dstBitmap.palette[i];
                const int32_t dr = int32_t((p >> 16) & 0xff) - int32_t(r);
                const int32_t dg = int32_t((p >> 8) & 0xff) - int32_t(g);
                const int32_t db = int32_t(p & 0xff) - int32_t(b);
                const uint32_t distance = uint32_t(dr * dr + dg * dg + db * db);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                    if (!distance) break;
                }
            }
            if (dstBitmap.bpp == 4) {
                uint8_t& packed = row[size_t(x) / 2];
                if (x & 1) {
                    packed = uint8_t((packed & 0xf0) | (bestIndex & 0x0f));
                } else {
                    packed = uint8_t((packed & 0x0f) | ((bestIndex & 0x0f) << 4));
                }
            } else {
                uint8_t& packed = row[size_t(x) / 8];
                const uint8_t mask = uint8_t(1u << (7 - (x & 7)));
                packed = bestIndex ? uint8_t(packed | mask) : uint8_t(packed & ~mask);
            }
        } else {
            return false;
        }
        return true;
    };

    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    const int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    const int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            uint32_t pixel = 0;
            uint32_t dstPixel = 0;
            const int32_t dx = outLeft + x;
            const int32_t dy = outTop + y;
            if (readBitmapPixel(srcBitmap, srcBits, srcHeight, sx, sy, pixel) &&
                readBitmapPixel(dstBitmap, dstBits, dstHeight, dx, dy, dstPixel)) {
                writeDestPixel(dx, dy, applySourceRasterOp(rop, pixel, dstPixel));
            }
        }
    }
    return uc_mem_write(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::transparentImageToFramebuffer(const GuestDc& dstDc,
                                                        const GuestBitmap& srcBitmap,
                                                        int32_t dstX,
                                                        int32_t dstY,
                                                        int32_t dstW,
                                                        int32_t dstH,
                                                        int32_t srcX,
                                                        int32_t srcY,
                                                        int32_t srcW,
                                                        int32_t srcH,
                                                        uint32_t transparentColor) {
    if (!framebuffer_ || !srcBitmap.bits || srcBitmap.width <= 0 || srcBitmap.heightRaw == 0 ||
        srcBitmap.stride == 0 || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
        return false;
    }
    const int32_t srcHeight = std::abs(srcBitmap.heightRaw);
    const uint64_t srcBytes = uint64_t(srcBitmap.stride) * uint64_t(srcHeight);
    if (!srcBytes || srcBytes > 0x2000000ull) return false;

    std::vector<uint8_t> srcBits(static_cast<size_t>(srcBytes));
    if (uc_mem_read(uc_, srcBitmap.bits, srcBits.data(), srcBits.size()) != UC_ERR_OK) return false;

    int32_t originX = 0;
    int32_t originY = 0;
    auto window = windows_.find(dstDc.hwnd);
    if (window != windows_.end()) {
        originX = window->second.x;
        originY = window->second.y;
    }

    const uint32_t transparentPixel = colorRefToPixel(transparentColor) & 0x00ffffffu;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    outLeft += originX;
    outTop += originY;

    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < 0 || dstPy >= framebufferHeight_) continue;
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < 0 || dstPx >= framebufferWidth_) continue;
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            uint32_t pixel = 0;
            if (readBitmapPixel(srcBitmap, srcBits, srcHeight, sx, sy, pixel) &&
                ((pixel & 0x00ffffffu) != transparentPixel)) {
                framebuffer_[size_t(dstPy) * size_t(framebufferWidth_) + size_t(dstPx)] = pixel;
            }
        }
    }
    invalidateHostWindows();
    return true;
}

bool SyntheticDllRuntime::transparentImageToBitmap(const GuestBitmap& dstBitmap,
                                                   const GuestBitmap& srcBitmap,
                                                   int32_t dstX,
                                                   int32_t dstY,
                                                   int32_t dstW,
                                                   int32_t dstH,
                                                   int32_t srcX,
                                                   int32_t srcY,
                                                   int32_t srcW,
                                                   int32_t srcH,
                                                   uint32_t transparentColor) {
    if (!dstBitmap.bits || !srcBitmap.bits || dstBitmap.width <= 0 || srcBitmap.width <= 0 ||
        dstBitmap.heightRaw == 0 || srcBitmap.heightRaw == 0 || dstBitmap.stride == 0 ||
        srcBitmap.stride == 0 || dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
        return false;
    }
    const int32_t dstHeight = std::abs(dstBitmap.heightRaw);
    const int32_t srcHeight = std::abs(srcBitmap.heightRaw);
    const uint64_t dstBytes = uint64_t(dstBitmap.stride) * uint64_t(dstHeight);
    const uint64_t srcBytes = uint64_t(srcBitmap.stride) * uint64_t(srcHeight);
    if (!dstBytes || !srcBytes || dstBytes > 0x2000000ull || srcBytes > 0x2000000ull) return false;

    std::vector<uint8_t> dstBits(static_cast<size_t>(dstBytes));
    std::vector<uint8_t> srcBits(static_cast<size_t>(srcBytes));
    if (uc_mem_read(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) != UC_ERR_OK ||
        uc_mem_read(uc_, srcBitmap.bits, srcBits.data(), srcBits.size()) != UC_ERR_OK) {
        return false;
    }

    const uint32_t transparentPixel = colorRefToPixel(transparentColor) & 0x00ffffffu;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    const int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    const int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            uint32_t pixel = 0;
            if (readBitmapPixel(srcBitmap, srcBits, srcHeight, sx, sy, pixel) &&
                ((pixel & 0x00ffffffu) != transparentPixel)) {
                writeBitmapPixel(dstBitmap, dstBits, dstHeight, outLeft + x, outTop + y, pixel);
            }
        }
    }
    return uc_mem_write(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) == UC_ERR_OK;
}

void SyntheticDllRuntime::loadMainResources(const std::filesystem::path& path) {
    mainResources_.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        spdlog::warn("resource parse skipped; cannot open {}", pathToUtf8(path));
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
    spdlog::info("parsed {} resources from {}", mainResources_.size(), pathToUtf8(path.filename()));
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

uint32_t SyntheticDllRuntime::handleWaveInGetID(uint32_t waveInHandle, uint32_t deviceIdPtr) {
#if defined(_WIN32)
    auto* handle = lookupGuestHandle(waveInHandle);
    auto& winmm = winmmBridge();
    if (!deviceIdPtr) return MMSYSERR_INVALPARAM;
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !winmm.waveInGetID) {
        return MMSYSERR_INVALHANDLE;
    }
    UINT id = 0;
    const MMRESULT result = winmm.waveInGetID(reinterpret_cast<HWAVEIN>(handle->hostValue), &id);
    if (result == MMSYSERR_NOERROR) writeU32(deviceIdPtr, id);
    return result;
#else
    return 2;
#endif
}

uint32_t SyntheticDllRuntime::handleWaveInBuffer(const std::string& name, uint32_t waveInHandle, uint32_t headerPtr) {
#if defined(_WIN32)
    auto* handle = lookupGuestHandle(waveInHandle);
    auto& winmm = winmmBridge();
    if (!handle || handle->kind != GuestHandle::Kind::HostWaveIn || !handle->hostValue || !headerPtr) {
        return MMSYSERR_INVALHANDLE;
    }

    uint32_t guestData = 0;
    uint32_t guestLength = 0;
    uint32_t guestBytesRecorded = 0;
    uint32_t guestUser = 0;
    uint32_t guestFlags = 0;
    uint32_t guestLoops = 0;
    uc_mem_read(uc_, headerPtr, &guestData, sizeof(guestData));
    uc_mem_read(uc_, headerPtr + 4, &guestLength, sizeof(guestLength));
    uc_mem_read(uc_, headerPtr + 8, &guestBytesRecorded, sizeof(guestBytesRecorded));
    uc_mem_read(uc_, headerPtr + 12, &guestUser, sizeof(guestUser));
    uc_mem_read(uc_, headerPtr + 16, &guestFlags, sizeof(guestFlags));
    uc_mem_read(uc_, headerPtr + 20, &guestLoops, sizeof(guestLoops));

    if (name == "waveInAddBuffer") {
        if (!winmm.waveInAddBuffer || !guestData || !guestLength || guestLength > 0x100000) {
            return MMSYSERR_INVALPARAM;
        }
        auto& stored = hostWaveBuffers_[headerPtr];
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
        const MMRESULT result = winmm.waveInAddBuffer(reinterpret_cast<HWAVEIN>(handle->hostValue), header, sizeof(*header));
        writeU32(headerPtr + 8, header->dwBytesRecorded);
        writeU32(headerPtr + 16, header->dwFlags);
        return result;
    }

    auto it = hostWaveBuffers_.find(headerPtr);
    if (it == hostWaveBuffers_.end() || !winmm.waveInUnprepareHeader) {
        return MMSYSERR_INVALPARAM;
    }
    auto* header = reinterpret_cast<WAVEHDR*>(it->second.header.data());
    const MMRESULT result = winmm.waveInUnprepareHeader(reinterpret_cast<HWAVEIN>(handle->hostValue), header, sizeof(*header));
    if (guestData && header->dwBytesRecorded) {
        uc_mem_write(uc_, guestData, it->second.data.data(),
                     std::min<size_t>(it->second.data.size(), header->dwBytesRecorded));
    }
    writeU32(headerPtr + 8, header->dwBytesRecorded);
    writeU32(headerPtr + 16, header->dwFlags);
    hostWaveBuffers_.erase(it);
    return result;
#else
    return 2;
#endif
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

uint32_t SyntheticDllRuntime::handleLoadCursorW(uint32_t, uint32_t cursorName) {
#if defined(_WIN32)
    HCURSOR cursor = nullptr;
    if (cursorName && cursorName < 0x10000) cursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(cursorName));
    else cursor = ::LoadCursorW(nullptr, IDC_ARROW);
    const uint32_t result = cursor
        ? makeGuestHandle({GuestHandle::Kind::HostCursor, reinterpret_cast<uintptr_t>(cursor), 0})
        : 0;
    lastError_ = result ? 0 : GetLastError();
    return result;
#else
    lastError_ = 0;
    return makeGuestHandle({GuestHandle::Kind::HostCursor, 0, 0});
#endif
}

uint32_t SyntheticDllRuntime::handleLoadImageApi(const std::string& name, uint32_t, uint32_t imageName,
                                                 uint32_t imageType, uint32_t desiredCx, uint32_t desiredCy,
                                                 uint32_t loadFlags) {
#if defined(_WIN32)
    if (name != "LoadImageW") {
        imageType = IMAGE_ICON;
        desiredCx = 0;
        desiredCy = 0;
        loadFlags = LR_DEFAULTCOLOR;
    }

    auto createIconFromMainResource = [&](uint32_t nameArg) -> HICON {
        const ResourceEntry* group = findResource(14, nameArg);
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
            const ResourceEntry* candidate = findResource(3, iconId);
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
        const ResourceEntry* bitmap = findResource(2, nameArg);
        if (!bitmap || bitmap->data.size() < 40) return nullptr;
        const uint32_t headerSize = readLe32(bitmap->data, 0);
        if (headerSize < 40 || headerSize > bitmap->data.size()) return nullptr;
        const uint16_t bitCount = readLe16(bitmap->data, 14);
        const uint32_t compression = readLe32(bitmap->data, 16);
        const uint32_t clrUsed = readLe32(bitmap->data, 32);
        uint32_t colorCount = clrUsed;
        if (!colorCount && bitCount <= 8) colorCount = 1u << bitCount;
        size_t bitsOffset = size_t(headerSize) + size_t(colorCount) * 4;
        if (compression == BI_BITFIELDS && !colorCount && (bitCount == 16 || bitCount == 32)) bitsOffset += 12;
        if (bitsOffset > bitmap->data.size()) return nullptr;
        HDC dc = GetDC(nullptr);
        if (!dc) return nullptr;
        HBITMAP result = CreateDIBitmap(dc,
                                        reinterpret_cast<const BITMAPINFOHEADER*>(bitmap->data.data()),
                                        CBM_INIT, bitmap->data.data() + bitsOffset,
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

    uint32_t result = 0;
    if (imageType == IMAGE_ICON) {
        HICON icon = createIconFromMainResource(imageName);
        if (icon) result = makeGuestHandle({GuestHandle::Kind::HostIcon, reinterpret_cast<uintptr_t>(icon), 0});
    } else if (imageType == IMAGE_BITMAP) {
        HBITMAP bitmap = createBitmapFromMainResource(imageName);
        if (bitmap) result = makeGuestHandle({GuestHandle::Kind::HostBitmap, reinterpret_cast<uintptr_t>(bitmap), 0});
    }
    lastError_ = result ? 0 : 1814;
    return result;
#else
    const uint32_t type = name == "LoadImageW" ? imageType : IMAGE_ICON;
    lastError_ = findResource(type == IMAGE_BITMAP ? 2 : 14, imageName) ? 120 : 1814;
    return 0;
#endif
}

uint32_t SyntheticDllRuntime::handleGetSysColorBrush(uint32_t colorIndex) {
    const int index = int(colorIndex & 0xffu);
#if defined(_WIN32)
    return makeGuestBrush(uint32_t(GetSysColor(index)), true);
#else
    return makeGuestBrush(0x00ffffffu, true);
#endif
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

uint32_t SyntheticDllRuntime::handleWideCharToMultiByte(uint32_t codePageArg, uint32_t flags,
                                                        uint32_t widePtr, uint32_t wideCharsArg) {
    const uint32_t multiOut = stackArg(4);
    const uint32_t multiCapacity = stackArg(5);
    const int32_t wideChars = int32_t(wideCharsArg);
    if (!widePtr) {
        lastError_ = 87;
        return 0;
    }
    std::vector<uint16_t> units;
    if (wideChars < 0) {
        for (uint32_t i = 0; i < 1024 * 1024; ++i) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, widePtr + i * 2, &ch, sizeof(ch)) != UC_ERR_OK) break;
            units.push_back(ch);
            if (!ch) break;
        }
    } else {
        units.resize(uint32_t(wideChars));
        if (!units.empty() &&
            uc_mem_read(uc_, widePtr, units.data(), units.size() * sizeof(uint16_t)) != UC_ERR_OK) {
            units.clear();
        }
    }
#if defined(_WIN32)
    const uint32_t codePage = guestAnsiCodePage(codePageArg);
    const int inputChars = wideChars < 0 ? -1 : int(units.size());
    const auto* wideData = reinterpret_cast<const wchar_t*>(units.data());
    const int needed = units.empty()
        ? 0
        : ::WideCharToMultiByte(codePage, flags, wideData, inputChars, nullptr, 0, nullptr, nullptr);
    if (!needed) {
        lastError_ = GetLastError();
        return 0;
    }
    if (!multiOut || !multiCapacity) {
        lastError_ = 0;
        return uint32_t(needed);
    }
    std::vector<char> bytes(static_cast<size_t>(multiCapacity));
    const int written = ::WideCharToMultiByte(codePage, flags, wideData, inputChars,
                                             bytes.data(), int(bytes.size()), nullptr, nullptr);
    if (!written) {
        lastError_ = GetLastError();
        return 0;
    }
    uc_mem_write(uc_, multiOut, bytes.data(), size_t(written));
    lastError_ = 0;
    return uint32_t(written);
#else
    const std::string text = utf16ToUtf8(units);
    if (multiOut && multiCapacity) {
        const uint32_t count = std::min<uint32_t>(uint32_t(text.size()), multiCapacity);
        if (count) uc_mem_write(uc_, multiOut, text.data(), count);
        return count;
    }
    return uint32_t(text.size());
#endif
}

uint32_t SyntheticDllRuntime::handleCreateFileMappingW(uint32_t fileHandle, uint32_t, uint32_t protect,
                                                       uint32_t sizeHigh) {
    const uint32_t sizeLow = stackArg(4);
    const uint32_t namePtr = stackArg(5);
    uint64_t mappingSize = (uint64_t(sizeHigh) << 32) | sizeLow;
    if (fileHandle != 0xffffffffu) {
        auto* file = lookupGuestHandle(fileHandle);
        if (!file || file->kind != GuestHandle::Kind::HostFile || !file->hostValue) {
            lastError_ = 6;
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
        return 0;
    }
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestFileMapping, 0, 0});
    fileMappings_[handle] = GuestFileMapping{fileHandle, mappingSize, protect, readUtf16(namePtr, 260)};
    lastError_ = 0;
    return handle;
}

uint32_t SyntheticDllRuntime::handleMapViewOfFile(uint32_t mappingHandle, uint32_t, uint32_t offsetHigh,
                                                  uint32_t offsetLow) {
    const uint32_t bytesToMap = stackArg(4);
    auto mapping = fileMappings_.find(mappingHandle);
    if (mapping == fileMappings_.end()) {
        lastError_ = 6;
        return 0;
    }
    const uint64_t offset = (uint64_t(offsetHigh) << 32) | offsetLow;
    if (offset >= mapping->second.size) {
        lastError_ = 87;
        return 0;
    }
    const uint64_t viewSize64 = bytesToMap ? bytesToMap : mapping->second.size - offset;
    if (!viewSize64 || viewSize64 > 0x02000000u) {
        lastError_ = 87;
        return 0;
    }
    const uint32_t viewSize = uint32_t(viewSize64);
    const uint32_t base = allocate(viewSize, true);
    if (!base) return 0;
    mappedViews_[base] = GuestMappedView{mappingHandle, offset, viewSize};
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
    lastError_ = 0;
    return base;
}

uint32_t SyntheticDllRuntime::handleUnmapViewOfFile(uint32_t baseAddress) {
    auto view = mappedViews_.find(baseAddress);
    if (view == mappedViews_.end()) {
        lastError_ = 487;
        return 0;
    }
    mappedViews_.erase(view);
    lastError_ = 0;
    return 1;
}

uint32_t SyntheticDllRuntime::handleFlushViewOfFile(uint32_t baseAddress, uint32_t bytesToFlush) {
    auto view = mappedViews_.find(baseAddress);
    if (view == mappedViews_.end()) {
        lastError_ = 487;
        return 0;
    }
    const auto mapping = fileMappings_.find(view->second.mappingHandle);
    const uint32_t bytes = bytesToFlush ? std::min(bytesToFlush, view->second.size) : view->second.size;
    if (mapping == fileMappings_.end() || !bytes) {
        lastError_ = 0;
        return 1;
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
    return 1;
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
    const uint32_t capacity = (size + 0x0fu) & ~0x0fu;
    uint32_t address = 0;
    uint32_t blockCapacity = capacity;
    auto freeIt = freeBlocksBySize_.lower_bound(capacity);
    if (freeIt != freeBlocksBySize_.end()) {
        blockCapacity = freeIt->first;
        address = freeIt->second;
        freeBlocksBySize_.erase(freeIt);
        const uint32_t remainder = blockCapacity - capacity;
        blockCapacity = capacity;
        if (remainder >= 0x20u) {
            freeBlocksBySize_.emplace(remainder, address + capacity);
        } else {
            blockCapacity += remainder;
        }
    } else {
        if (nextHeap_ + capacity > heapLimit_) {
            spdlog::warn("guest heap exhausted requested={} capacity={} next=0x{:08x} limit=0x{:08x} freeBlocks={}",
                         size, capacity, nextHeap_, heapLimit_, freeBlocksBySize_.size());
            lastError_ = 14; // ERROR_OUTOFMEMORY
            return 0;
        }
        address = nextHeap_;
        nextHeap_ += capacity;
    }
    allocationSizes_[address] = size;
    allocationCapacities_[address] = blockCapacity;
    if (zeroFill) {
        std::vector<uint8_t> zeros(blockCapacity);
        uc_mem_write(uc_, address, zeros.data(), zeros.size());
    }
    lastError_ = 0;
    return address;
}

void SyntheticDllRuntime::releaseAllocation(uint32_t address) {
    if (!address) return;
    auto sizeIt = allocationSizes_.find(address);
    auto capacityIt = allocationCapacities_.find(address);
    if (sizeIt == allocationSizes_.end() || capacityIt == allocationCapacities_.end()) return;
    const uint32_t capacity = capacityIt->second;
    allocationSizes_.erase(sizeIt);
    allocationCapacities_.erase(capacityIt);
    if (capacity) freeBlocksBySize_.emplace(capacity, address);
}

uint32_t SyntheticDllRuntime::allocationSize(uint32_t address) const {
    auto it = allocationSizes_.find(address);
    return it == allocationSizes_.end() ? 0 : it->second;
}

uint32_t SyntheticDllRuntime::readU32(uint32_t address) const {
    uint32_t value = 0;
    if (address) uc_mem_read(uc_, address, &value, sizeof(value));
    return value;
}

void SyntheticDllRuntime::writeU32(uint32_t address, uint32_t value) const {
    if (address) uc_mem_write(uc_, address, &value, sizeof(value));
}

bool SyntheticDllRuntime::isGuestRangeReadable(uint32_t address, uint32_t size) const {
    if (!address) return false;
    if (!size) return true;
    uint8_t byte = 0;
    if (uc_mem_read(uc_, address, &byte, sizeof(byte)) != UC_ERR_OK) return false;
    const uint32_t last = address + size - 1;
    if (last < address) return false;
    return uc_mem_read(uc_, last, &byte, sizeof(byte)) == UC_ERR_OK;
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
    std::vector<uint16_t> units;
    for (size_t i = 0; address && i < maxChars; ++i) {
        uint16_t ch = 0;
        if (uc_mem_read(uc_, address + uint32_t(i * 2), &ch, sizeof(ch)) != UC_ERR_OK) break;
        if (!ch) break;
        units.push_back(ch);
    }
    return utf16ToUtf8(units);
}

uint32_t SyntheticDllRuntime::writeUtf16(uint32_t address, const std::string& value, uint32_t maxChars) const {
    if (!address || !maxChars) return 0;
    const std::u16string wide = utf8ToUtf16(value);
    const uint32_t charsToWrite = std::min<uint32_t>(uint32_t(wide.size()), maxChars - 1);
    for (uint32_t i = 0; i < charsToWrite; ++i) {
        const uint16_t ch = uint16_t(wide[i]);
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
        return pathFromUtf8(normalized);
    }
    const bool rootRelative = !normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/');

    while (!normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/')) {
        normalized.erase(normalized.begin());
    }

    std::filesystem::path relative = pathFromUtf8(normalized);
    std::filesystem::path missingLeafCandidate;
    auto rememberMissingLeafCandidate = [&](const std::filesystem::path& candidate) {
        if (missingLeafCandidate.empty() && parentExistsForLookup(candidate)) {
            missingLeafCandidate = candidate;
        }
    };
    if (rootRelative && !fileSystemRoots_.empty()) {
        for (const auto& root : fileSystemRoots_) {
            const std::filesystem::path candidate = root / relative;
            if (pathExistsForLookup(candidate)) return candidate;
            rememberMissingLeafCandidate(candidate);
        }
        if (!missingLeafCandidate.empty()) return missingLeafCandidate;
        return fileSystemRoots_.front() / relative;
    }
    if (!fileSystemRoots_.empty()) {
        for (const auto& root : fileSystemRoots_) {
            const std::filesystem::path candidate = root / relative;
            if (pathExistsForLookup(candidate)) return candidate;
            rememberMissingLeafCandidate(candidate);
        }
        if (!missingLeafCandidate.empty()) return missingLeafCandidate;
        return fileSystemRoots_.front() / relative;
    }
    if (!hostBaseDir_.empty()) {
        auto first = relative.begin();
        if (first != relative.end() &&
            lowerAscii(pathToUtf8(*first)) == lowerAscii(pathToUtf8(hostBaseDir_.filename()))) {
            return hostBaseDir_.parent_path() / relative;
        }
        return hostBaseDir_ / relative;
    }
    return relative;
}

bool SyntheticDllRuntime::isUnderFileSystemRoot(const std::filesystem::path& path) const {
    const std::string pathKey = normalizedPathKey(path);
    for (const auto& root : fileSystemRoots_) {
        const std::string rootKey = normalizedPathKey(root);
        if (rootKey.empty()) continue;
        if (pathKey == rootKey) return true;
        if (pathKey.size() > rootKey.size() && pathKey.compare(0, rootKey.size(), rootKey) == 0 &&
            pathKey[rootKey.size()] == '\\') {
            return true;
        }
    }
    return false;
}

uint32_t SyntheticDllRuntime::normalizeVirtualFileMiss(const std::filesystem::path& hostPath, uint32_t error) const {
    if (error == kErrorPathNotFound && !hostPath.empty() && isUnderFileSystemRoot(hostPath)) {
        return kErrorFileNotFound;
    }
    return error;
}

bool SyntheticDllRuntime::dispatchGuestMemoryApi(const std::string& name,
                                                 const GuestCallArgs& args,
                                                 uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;

    if (name == "CreateBitmap") return handleCreateBitmap(args, ret);
    if (name == "GetObjectW") return handleGetObjectW(args, ret);
    if (name == "SetDIBColorTable") return handleSetDIBColorTable(args, ret);
    if (name == "SetBitmapBits") return handleSetBitmapBits(args, ret);
    if (name == "SetDIBitsToDevice") return handleSetDIBitsToDevice(args, ret);

    if (name == "CharUpperW" || name == "CharLowerW") {
        const bool makeUpper = name == "CharUpperW";
        if (a0 <= 0xffffu) {
            ret = uint32_t(makeUpper ? std::towupper(wint_t(a0))
                                     : std::towlower(wint_t(a0)));
        } else {
            for (uint32_t offset = 0; offset < 4096; offset += 2) {
                uint16_t ch = 0;
                if (uc_mem_read(uc_, a0 + offset, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
                const uint16_t mapped = uint16_t(makeUpper ? std::towupper(wint_t(ch))
                                                           : std::towlower(wint_t(ch)));
                if (mapped != ch) uc_mem_write(uc_, a0 + offset, &mapped, sizeof(mapped));
            }
            ret = a0;
        }
    } else if (name == "CreatePenIndirect") {
        if (!a0) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestPen(readU32(a0), readU32(a0 + 4), readU32(a0 + 12));
            lastError_ = 0;
        }
    } else if (name == "CreateDIBSection") {
        if (!a1 || !a3) {
            lastError_ = 87;
            ret = 0;
        } else {
            const uint32_t headerSize = readU32(a1);
            const int32_t width = int32_t(readU32(a1 + 4));
            const int32_t heightRaw = int32_t(readU32(a1 + 8));
            uint16_t bpp = 0;
            uc_mem_read(uc_, a1 + 14, &bpp, sizeof(bpp));
            const uint32_t compression = headerSize >= 40 ? readU32(a1 + 16) : 0;
            const uint32_t clrUsed = headerSize >= 40 ? readU32(a1 + 32) : 0;
            const uint32_t absHeight = uint32_t(heightRaw < 0 ? -heightRaw : heightRaw);
            const uint32_t bitsPerPixel = bpp ? bpp : 32;
            const uint32_t stride = ((uint32_t(std::max<int32_t>(width, 0)) * bitsPerPixel + 31) / 32) * 4;
            const uint32_t bits = allocate(std::max<uint32_t>(stride * absHeight, 4), true);
            writeU32(a3, bits);
            ret = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, bits});
            lastError_ = ret ? 0 : 8;
            if (ret) {
                GuestBitmap bitmap{};
                bitmap.width = width;
                bitmap.heightRaw = heightRaw;
                bitmap.bpp = uint16_t(bitsPerPixel);
                bitmap.stride = stride;
                bitmap.bits = bits;
                if (bitmap.bpp == 16) {
                    ceDefault16BitMasks(bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
                    if (compression == 3) {
                        const uint32_t maskOffset = headerSize >= 52 ? 40 : headerSize;
                        bitmap.redMask = readU32(a1 + maskOffset);
                        bitmap.greenMask = readU32(a1 + maskOffset + 4);
                        bitmap.blueMask = readU32(a1 + maskOffset + 8);
                    }
                }
                const uint32_t paletteEntries = bitsPerPixel <= 8 ? (clrUsed ? clrUsed : (1u << bitsPerPixel)) : 0;
                if (headerSize >= 40 && headerSize < 0x1000 && paletteEntries && paletteEntries <= 256) {
                    std::vector<uint8_t> rawPalette(size_t(paletteEntries) * 4);
                    if (uc_mem_read(uc_, a1 + headerSize, rawPalette.data(), rawPalette.size()) == UC_ERR_OK) {
                        bitmap.palette.resize(paletteEntries);
                        for (uint32_t i = 0; i < paletteEntries; ++i) {
                            const uint8_t b = rawPalette[size_t(i) * 4 + 0];
                            const uint8_t g = rawPalette[size_t(i) * 4 + 1];
                            const uint8_t r = rawPalette[size_t(i) * 4 + 2];
                            bitmap.palette[i] = 0xff000000u | (uint32_t(r) << 16) |
                                                (uint32_t(g) << 8) | uint32_t(b);
                        }
                    }
                }
                if (bitmap.palette.empty() && (bitsPerPixel == 1 || bitsPerPixel == 4 || bitsPerPixel == 8)) {
                    bitmap.palette = defaultIndexedPalette(uint16_t(bitsPerPixel));
                }
                bitmaps_[ret] = std::move(bitmap);
            }
            spdlog::info("CreateDIBSection {}x{} bpp={} compression={} masks={:08x}/{:08x}/{:08x} stride={} bits=0x{:08x} bitmap=0x{:08x}",
                         width, heightRaw, bitsPerPixel, compression,
                         ret && bitmaps_.count(ret) ? bitmaps_[ret].redMask : 0,
                         ret && bitmaps_.count(ret) ? bitmaps_[ret].greenMask : 0,
                         ret && bitmaps_.count(ret) ? bitmaps_[ret].blueMask : 0,
                         stride, bits, ret);
        }
    } else if (name == "CreateCompatibleBitmap") {
        const uint32_t width = std::max<uint32_t>(a1, 1);
        const uint32_t height = std::max<uint32_t>(a2, 1);
        const uint32_t bpp = 32;
        const uint32_t stride = ((width * bpp + 31) / 32) * 4;
        const uint32_t bits = allocate(std::max<uint32_t>(stride * height, 4), true);
        ret = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, bits});
        if (ret) {
            bitmaps_[ret] = GuestBitmap{int32_t(width), -int32_t(height), uint16_t(bpp), stride, bits,
                                        0, 0, 0, {}};
        }
        lastError_ = ret ? 0 : 8;
        spdlog::info("CreateCompatibleBitmap {}x{} bits=0x{:08x} bitmap=0x{:08x}",
                     width, height, bits, ret);
    } else if (name == "Polyline") {
        GuestDc* dc = lookupGuestDc(a0);
        auto pen = dc ? pens_.find(dc->selectedPen) : pens_.end();
        if (!dc || !a1 || a2 < 2 || pen == pens_.end()) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            const uint32_t pixel = colorRefToPixel(pen->second.colorRef);
            int32_t prevX = int32_t(readU32(a1));
            int32_t prevY = int32_t(readU32(a1 + 4));
            for (uint32_t index = 1; index < a2 && index < 0x10000; ++index) {
                const uint32_t point = a1 + index * 8;
                const int32_t x = int32_t(readU32(point));
                const int32_t y = int32_t(readU32(point + 4));
                drawFramebufferLine(*dc, prevX, prevY, x, y, pixel);
                prevX = x;
                prevY = y;
            }
            dc->x = prevX;
            dc->y = prevY;
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "fopen" || name == "_wfopen") {
        const std::string guestPath = name == "_wfopen" ? readUtf16(a0, 2048) : readAscii(a0, 2048);
        const std::string mode = name == "_wfopen" ? readUtf16(a1, 64) : readAscii(a1, 64);
        const std::filesystem::path hostPath = resolveGuestPath(guestPath);
        FILE* host = nullptr;
        if (!hostPath.empty()) {
#if defined(_WIN32)
            _wfopen_s(&host, hostPath.wstring().c_str(), widenLossy(mode).c_str());
#else
            host = std::fopen(hostPath.string().c_str(), mode.c_str());
#endif
        }
        if (!host) {
            ret = 0;
            spdlog::warn("{} miss guest=\"{}\" host=\"{}\" mode=\"{}\"",
                         name, guestPath, pathToUtf8(hostPath), mode);
        } else {
            ret = makeGuestHandle({GuestHandle::Kind::HostCrtFile, reinterpret_cast<uintptr_t>(host), 0});
            fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
            spdlog::info("{} hit guest=\"{}\" host=\"{}\" mode=\"{}\" guestHandle=0x{:08x}",
                         name, guestPath, pathToUtf8(hostPath), mode, ret);
        }
    } else if (name == "fclose") {
        auto it = guestHandles_.find(a0);
        if (it == guestHandles_.end() || it->second.kind != GuestHandle::Kind::HostCrtFile || !it->second.hostValue) {
            ret = 0xffffffffu;
        } else {
            FILE* host = reinterpret_cast<FILE*>(it->second.hostValue);
            ret = uint32_t(std::fclose(host));
            fileHandleDebugNames_.erase(a0);
            fileReadCounts_.erase(a0);
            guestHandles_.erase(it);
        }
    } else if (name == "fread") {
        auto* handle = lookupGuestHandle(a3);
        if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !a0 || !a1) {
            ret = 0;
        } else {
            const uint64_t total = uint64_t(a1) * uint64_t(a2);
            std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(total, 0x1000000u)));
            const size_t readBytes = bytes.empty()
                ? 0
                : std::fread(bytes.data(), 1, bytes.size(), reinterpret_cast<FILE*>(handle->hostValue));
            if (readBytes) uc_mem_write(uc_, a0, bytes.data(), readBytes);
            ret = uint32_t(readBytes / a1);
            const uint32_t readCount = ++fileReadCounts_[a3];
            if (readCount <= 32 || readBytes != bytes.size()) {
                auto debugName = fileHandleDebugNames_.find(a3);
                const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
                spdlog::info("fread handle=0x{:08x} path=\"{}\" size={} count={} bytes={} elements={} read#={}",
                             a3, debugPath, a1, a2, readBytes, ret, readCount);
            }
        }
    } else if (name == "fwrite") {
        auto* handle = lookupGuestHandle(a3);
        if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !a0 || !a1) {
            ret = 0;
        } else {
            const uint64_t total = uint64_t(a1) * uint64_t(a2);
            std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(total, 0x1000000u)));
            if (!bytes.empty()) uc_mem_read(uc_, a0, bytes.data(), bytes.size());
            const size_t writtenBytes = bytes.empty()
                ? 0
                : std::fwrite(bytes.data(), 1, bytes.size(), reinterpret_cast<FILE*>(handle->hostValue));
            ret = uint32_t(writtenBytes / a1);
        }
    } else if (name == "fseek") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
            ? uint32_t(std::fseek(reinterpret_cast<FILE*>(handle->hostValue), int32_t(a1), int(a2)))
            : 0xffffffffu;
    } else if (name == "ftell") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
            ? uint32_t(std::ftell(reinterpret_cast<FILE*>(handle->hostValue)))
            : 0xffffffffu;
    } else if (name == "fflush") {
        auto* handle = lookupGuestHandle(a0);
        ret = (!a0 || (handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue))
            ? uint32_t(std::fflush(a0 ? reinterpret_cast<FILE*>(handle->hostValue) : nullptr))
            : 0xffffffffu;
    } else if (name == "feof" || name == "ferror") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue) {
            ret = 0;
        } else if (name == "feof") {
            ret = uint32_t(std::feof(reinterpret_cast<FILE*>(handle->hostValue)));
        } else {
            ret = uint32_t(std::ferror(reinterpret_cast<FILE*>(handle->hostValue)));
        }
    } else if (name == "fgetc") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
            ? uint32_t(std::fgetc(reinterpret_cast<FILE*>(handle->hostValue)))
            : 0xffffffffu;
    } else if (name == "fgets") {
        auto* handle = lookupGuestHandle(a2);
        if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !a0 || !a1) {
            ret = 0;
        } else {
            std::vector<char> bytes(a1);
            char* result = std::fgets(bytes.data(), int(a1), reinterpret_cast<FILE*>(handle->hostValue));
            if (result) uc_mem_write(uc_, a0, bytes.data(), std::strlen(bytes.data()) + 1);
            ret = result ? a0 : 0;
        }
    } else if (name == "FindNextFileW") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
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
                writeGuestFindData(uc_, a1, data);
                auto debugName = fileHandleDebugNames_.find(a0);
                const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
                spdlog::info("FindNextFileW hit handle=0x{:08x} path=\"{}\" file=\"{}\" attr=0x{:08x} size={}",
                             a0, debugPath,
                             wideZToUtf8(data.cFileName, 260),
                             data.dwFileAttributes,
                             (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
            } else {
                spdlog::info("FindNextFileW miss handle=0x{:08x} lastError={}", a0, lastError_);
            }
        }
#else
        lastError_ = 6;
        ret = 0;
#endif
    } else if (name == "memcpy" || name == "memmove") {
        copyGuest(a0, a1, a2);
        ret = a0;
    } else if (name == "memset") {
        fillGuest(a0, uint8_t(a1 & 0xffu), a2);
        ret = a0;
    } else if (name == "swprintf" || name == "wsprintfW" || name == "vswprintf" ||
               name == "_snwprintf" || name == "_vsnwprintf") {
        std::vector<uint32_t> values;
        if (name == "vswprintf") {
            values.reserve(16);
            for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(a2 + i * 4));
        } else if (name == "_vsnwprintf") {
            values.reserve(16);
            for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(a3 + i * 4));
        } else if (name == "_snwprintf") {
            values = {a3};
            for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
        } else {
            values = {a2, a3};
            for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
        }
        size_t argIndex = 0;
        auto nextArg = [&]() -> uint32_t {
            return argIndex < values.size() ? values[argIndex++] : 0;
        };
        const std::string format = readUtf16((name == "_snwprintf" || name == "_vsnwprintf") ? a2 : a1, 2048);
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
            char lengthModifier = 0;
            if (i < format.size() && (format[i] == 'l' || format[i] == 'h')) {
                lengthModifier = format[i++];
            }
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
                if (spec == 'S' || lengthModifier == 'h') {
                    out += readAscii(value, 2048);
                } else {
                    std::string text = readUtf16(value, 2048);
                    if (text.empty()) text = readAscii(value, 2048);
                    out += text;
                }
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
        ret = writeUtf16(a0, out, (name == "_snwprintf" || name == "_vsnwprintf") ? a1 : uint32_t(out.size() + 1));
        if (out.find(".db") != std::string::npos || out.find(".bin") != std::string::npos ||
            out.find("\\") != std::string::npos || out.find("/") != std::string::npos) {
            spdlog::info("synthetic coredll.dll!{} formatted \"{}\" -> 0x{:08x}",
                         name, out, a0);
        } else if (out.empty() && (name == "wsprintfW" || name == "swprintf")) {
            spdlog::info("synthetic coredll.dll!{} formatted empty format=\"{}\" a2w=\"{}\" a2a=\"{}\" a3=0x{:08x}",
                         name, format, readUtf16(a2, 128), readAscii(a2, 128), a3);
        }
    } else if (name == "printf" || name == "sprintf" || name == "_snprintf") {
        std::vector<uint32_t> values = name == "sprintf"
            ? std::vector<uint32_t>{a2, a3}
            : (name == "_snprintf" ? std::vector<uint32_t>{a3} : std::vector<uint32_t>{a1, a2, a3});
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
        size_t argIndex = 0;
        auto nextArg = [&]() -> uint32_t {
            return argIndex < values.size() ? values[argIndex++] : 0;
        };
        const std::string format = readAscii(name == "sprintf" ? a1 : (name == "_snprintf" ? a2 : a0), 2048);
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
                                   spec == 'X' || spec == 'p' || spec == 's' ||
                                   spec == 'S'
                ? nextArg()
                : 0;
            char buffer[64]{};
            switch (spec) {
            case 's':
                out += readAscii(value, 2048);
                break;
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
            case 'p':
                std::snprintf(buffer, sizeof(buffer), "0x%08x", value);
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
        if (name == "sprintf" || name == "_snprintf") {
            if (name == "_snprintf") {
                const uint32_t capacity = a1;
                if (a0 && capacity) {
                    const uint32_t copy = std::min<uint32_t>(capacity - 1, uint32_t(out.size()));
                    if (copy) uc_mem_write(uc_, a0, out.data(), copy);
                    const char nul = 0;
                    uc_mem_write(uc_, a0 + copy, &nul, sizeof(nul));
                }
            } else {
                writeAscii(a0, out);
            }
            if (out.find(".db") != std::string::npos || out.find(".bin") != std::string::npos ||
                out.find("\\") != std::string::npos || out.find("/") != std::string::npos) {
                spdlog::info("synthetic coredll.dll!{} formatted \"{}\" -> 0x{:08x}",
                             name, out, a0);
            }
        } else if (!out.empty()) {
            spdlog::info("printf: {}", out);
        }
        ret = uint32_t(out.size());
    } else if (name == "_wtol") {
        const std::string value = readUtf16(a0, 128);
        char* end = nullptr;
        ret = uint32_t(std::strtol(value.c_str(), &end, 10));
    } else if (name == "_ultow") {
        const uint32_t radix = (a2 >= 2 && a2 <= 36) ? a2 : 10;
        uint32_t value = a0;
        std::string digits;
        do {
            const uint32_t digit = value % radix;
            digits.push_back(char(digit < 10 ? ('0' + digit) : ('a' + digit - 10)));
            value /= radix;
        } while (value);
        std::reverse(digits.begin(), digits.end());
        writeUtf16(a1, digits, uint32_t(digits.size() + 1));
        ret = a1;
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
    } else if (name == "InterlockedTestExchange") {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        if (old == a1) uc_mem_write(uc_, a0, &a2, sizeof(a2));
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
        releaseAllocation(a0);
        ret = 0;
    } else if (name == "LocalSize" || name == "HeapSize") {
        ret = allocationSize(name == "HeapSize" ? a2 : a0);
    } else if (name == "LocalReAlloc" || name == "RemoteLocalReAlloc" || name == "realloc") {
        if (!a1) {
            releaseAllocation(a0);
            ret = 0;
        } else {
            const uint32_t oldSize = allocationSize(a0);
            ret = allocate(a1, name == "realloc" ? false : (a2 & 0x0040u) != 0);
            if (a0 && ret && oldSize) {
                std::vector<uint8_t> bytes(std::min(oldSize, a1));
                uc_mem_read(uc_, a0, bytes.data(), bytes.size());
                uc_mem_write(uc_, ret, bytes.data(), bytes.size());
                releaseAllocation(a0);
            }
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
            const uint32_t oldSize = allocationSize(a2);
            ret = allocate(a3, (a1 & 0x00000008u) != 0);
            if (ret && oldSize) {
                std::vector<uint8_t> bytes(std::min(oldSize, a3));
                uc_mem_read(uc_, a2, bytes.data(), bytes.size());
                uc_mem_write(uc_, ret, bytes.data(), bytes.size());
                releaseAllocation(a2);
            }
            lastError_ = ret ? 0 : 8;
        }
    } else if (name == "HeapFree") {
        auto* heap = lookupGuestHandle(a0);
        if (!heap || heap->kind != GuestHandle::Kind::GuestHeap || !a2) {
            lastError_ = 6;
            ret = 0;
        } else {
            releaseAllocation(a2);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "RemoteHeapFree") {
        ret = copyGuest(a0, a2, 0x38) ? a0 : 1;
    } else if (name == "VirtualAlloc") {
        ret = allocate(a1, true);
    } else if (name == "VirtualFree") {
        releaseAllocation(a0);
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
        releaseAllocation(a0);
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
    } else if (name == "wcsstr") {
        ret = 0;
        uint16_t needleFirst = 0;
        if (a0 && a1 && uc_mem_read(uc_, a1, &needleFirst, sizeof(needleFirst)) == UC_ERR_OK) {
            if (!needleFirst) {
                ret = a0;
            } else {
                for (uint32_t hayOffset = 0; hayOffset < 0x200000; hayOffset += 2) {
                    uint16_t hay = 0;
                    if (uc_mem_read(uc_, a0 + hayOffset, &hay, sizeof(hay)) != UC_ERR_OK || !hay) break;
                    if (hay != needleFirst) continue;
                    bool match = true;
                    for (uint32_t needleOffset = 2; needleOffset < 0x200000; needleOffset += 2) {
                        uint16_t needle = 0;
                        uint16_t candidate = 0;
                        if (uc_mem_read(uc_, a1 + needleOffset, &needle, sizeof(needle)) != UC_ERR_OK) {
                            match = false;
                            break;
                        }
                        if (!needle) break;
                        if (uc_mem_read(uc_, a0 + hayOffset + needleOffset, &candidate, sizeof(candidate)) != UC_ERR_OK ||
                            candidate != needle) {
                            match = false;
                            break;
                        }
                    }
                    if (match) {
                        ret = a0 + hayOffset;
                        break;
                    }
                }
            }
        }
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
    } else if (name == "_wcsicmp") {
        ret = 0;
        for (uint32_t i = 0; i < 0x100000; ++i) {
            uint16_t left = 0;
            uint16_t right = 0;
            uc_mem_read(uc_, a0 + i * 2, &left, sizeof(left));
            uc_mem_read(uc_, a1 + i * 2, &right, sizeof(right));
            if (left >= 'A' && left <= 'Z') left = uint16_t(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = uint16_t(right - 'A' + 'a');
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
    } else if (name == "strcat") {
        uint32_t dstLen = 0;
        for (; dstLen < 0x100000; ++dstLen) {
            char ch = 0;
            if (uc_mem_read(uc_, a0 + dstLen, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
        }
        uint32_t offset = 0;
        for (;; ++offset) {
            char ch = 0;
            uc_mem_read(uc_, a1 + offset, &ch, sizeof(ch));
            uc_mem_write(uc_, a0 + dstLen + offset, &ch, sizeof(ch));
            if (!ch) break;
        }
        ret = a0;
    } else if (name == "strtok") {
        const std::string delimiters = readAscii(a1, 256);
        uint32_t cursor = a0 ? a0 : strtokNext_;
        auto isDelimiter = [&](char ch) {
            return delimiters.find(ch) != std::string::npos;
        };
        char ch = 0;
        while (cursor && uc_mem_read(uc_, cursor, &ch, sizeof(ch)) == UC_ERR_OK && ch && isDelimiter(ch)) {
            ++cursor;
        }
        if (!cursor || uc_mem_read(uc_, cursor, &ch, sizeof(ch)) != UC_ERR_OK || !ch) {
            strtokNext_ = 0;
            ret = 0;
        } else {
            ret = cursor;
            for (;; ++cursor) {
                if (uc_mem_read(uc_, cursor, &ch, sizeof(ch)) != UC_ERR_OK || !ch) {
                    strtokNext_ = 0;
                    break;
                }
                if (isDelimiter(ch)) {
                    const char nul = 0;
                    uc_mem_write(uc_, cursor, &nul, sizeof(nul));
                    strtokNext_ = cursor + 1;
                    break;
                }
            }
        }
    } else if (name == "memcmp") {
        ret = 0;
        for (uint32_t offset = 0; offset < a2; ++offset) {
            unsigned char left = 0;
            unsigned char right = 0;
            if (uc_mem_read(uc_, a0 + offset, &left, sizeof(left)) != UC_ERR_OK ||
                uc_mem_read(uc_, a1 + offset, &right, sizeof(right)) != UC_ERR_OK) {
                ret = 0xffffffffu;
                break;
            }
            if (left != right) {
                ret = uint32_t(int(left) - int(right));
                break;
            }
        }
    } else if (name == "rand") {
        ret = uint32_t(std::rand());
    } else if (name == "srand") {
        std::srand(a0);
        ret = 0;
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
    } else if (name == "strtoul") {
        const std::string source = readAscii(a0);
        char* end = nullptr;
        const unsigned long value = std::strtoul(source.c_str(), &end, int(a2));
        if (a1) writeU32(a1, a0 + uint32_t(end ? end - source.c_str() : 0));
        ret = uint32_t(value);
        spdlog::info("strtoul source=\"{}\" base={} -> 0x{:08x}", source, a2, ret);
    } else if (name == "atoi") {
        ret = uint32_t(std::atoi(readAscii(a0, 256).c_str()));
    } else if (name == "atof") {
        const std::string source = readAscii(a0, 256);
        char* end = nullptr;
        const double value = std::strtod(source.c_str(), &end);
        setGuestDoubleReturn(uc_, value, ret);
    } else if (name == "CopyRect") {
        ret = copyGuest(a0, a1, 16) ? 1 : 0;
    } else if (name == "EqualRect") {
        int32_t left[4]{};
        int32_t right[4]{};
        ret = a0 && a1 &&
              uc_mem_read(uc_, a0, left, sizeof(left)) == UC_ERR_OK &&
              uc_mem_read(uc_, a1, right, sizeof(right)) == UC_ERR_OK &&
              std::memcmp(left, right, sizeof(left)) == 0 ? 1 : 0;
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
    } else if (name == "SetRect") {
        if (a0) {
            writeGuestRect(a0, int32_t(a1), int32_t(a2), int32_t(a3), int32_t(stackArg(4)));
            ret = 1;
        } else {
            lastError_ = 87;
            ret = 0;
        }
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
    } else if (name == "IsDBCSLeadByteEx") {
        const uint32_t codePage = guestAnsiCodePage(a0);
        const uint8_t ch = uint8_t(a1);
        ret = (codePage == 949 && ch >= 0x81 && ch <= 0xfe) ? 1 : 0;
    } else if (name == "iswctype") {
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
            const uint32_t codePage = guestAnsiCodePage(a0);
            const int inputChars = byteCount < 0 ? -1 : int(bytes.size());
            const int needed = bytes.empty()
                ? 0
                : ::MultiByteToWideChar(codePage, a1, bytes.data(), inputChars, nullptr, 0);
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
                const int written = ::MultiByteToWideChar(codePage, a1, bytes.data(), inputChars, wideData, wideLength);
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
    } else if (name == "__litodp") {
        setGuestDoubleReturn(uc_, static_cast<double>(int32_t(a0)), ret);
    } else if (name == "__ultodp") {
        setGuestDoubleReturn(uc_, static_cast<double>(a0), ret);
    } else if (name == "__fpadd") {
        float left = 0.0f;
        float right = 0.0f;
        std::memcpy(&left, &a0, sizeof(left));
        std::memcpy(&right, &a1, sizeof(right));
        const float value = left + right;
        std::memcpy(&ret, &value, sizeof(ret));
    } else if (name == "__dpadd" || name == "__dpsub" || name == "__dpmul" || name == "__dpdiv") {
        const double left = doubleFromGuestPair(a0, a1);
        const double right = doubleFromGuestPair(a2, a3);
        const double result = name == "__dpadd"
            ? left + right
            : (name == "__dpsub" ? left - right : (name == "__dpmul" ? left * right : (right == 0.0 ? 0.0 : left / right)));
        setGuestDoubleReturn(uc_, result, ret);
    } else if (name == "__dptoli") {
        ret = uint32_t(int32_t(doubleFromGuestPair(a0, a1)));
    } else if (name == "__dptoul") {
        ret = uint32_t(doubleFromGuestPair(a0, a1));
    } else if (name == "__dptofp") {
        const float value = static_cast<float>(doubleFromGuestPair(a0, a1));
        std::memcpy(&ret, &value, sizeof(ret));
    } else if (name == "__fptoli") {
        float value = 0.0f;
        std::memcpy(&value, &a0, sizeof(value));
        ret = uint32_t(int32_t(value));
    } else if (name == "__fptoul") {
        float value = 0.0f;
        std::memcpy(&value, &a0, sizeof(value));
        ret = uint32_t(value);
    } else if (name == "__fptodp") {
        float value = 0.0f;
        std::memcpy(&value, &a0, sizeof(value));
        setGuestDoubleReturn(uc_, static_cast<double>(value), ret);
    } else if (name == "__fpmul") {
        float left = 0.0f;
        float right = 0.0f;
        std::memcpy(&left, &a0, sizeof(left));
        std::memcpy(&right, &a1, sizeof(right));
        const float value = left * right;
        std::memcpy(&ret, &value, sizeof(ret));
    } else if (name == "fmodf") {
        float left = 0.0f;
        float right = 0.0f;
        std::memcpy(&left, &a0, sizeof(left));
        std::memcpy(&right, &a1, sizeof(right));
        const float value = right == 0.0f ? 0.0f : std::fmod(left, right);
        std::memcpy(&ret, &value, sizeof(ret));
    } else if (name == "__litofp") {
        const float value = static_cast<float>(int32_t(a0));
        std::memcpy(&ret, &value, sizeof(ret));
    } else if (name == "__eqs" || name == "__nes" || name == "__lts" || name == "__les" || name == "__ges") {
        float left = 0.0f;
        float right = 0.0f;
        std::memcpy(&left, &a0, sizeof(left));
        std::memcpy(&right, &a1, sizeof(right));
        const bool equal = left == right;
        if (name == "__lts") ret = left < right ? 1 : 0;
        else if (name == "__les") ret = left <= right ? 1 : 0;
        else if (name == "__ges") ret = left >= right ? 1 : 0;
        else ret = (name == "__eqs" ? equal : !equal) ? 1 : 0;
    } else if (name == "sqrt") {
        setGuestDoubleReturn(uc_, std::sqrt(doubleFromGuestPair(a0, a1)), ret);
    } else if (name == "toupper") {
        ret = uint32_t(std::toupper(int(a0)));
    } else if (name == "__ltd" || name == "__led" || name == "__eqd" || name == "__ged") {
        const double left = doubleFromGuestPair(a0, a1);
        const double right = doubleFromGuestPair(a2, a3);
        if (name == "__ltd") ret = left < right ? 1 : 0;
        else if (name == "__led") ret = left <= right ? 1 : 0;
        else if (name == "__eqd") ret = left == right ? 1 : 0;
        else ret = left >= right ? 1 : 0;
    } else if (name == "__ehvec_ctor") {
        // Full vector construction needs repeated guest callback transfers. The
        // current callers survive a fail-closed no-construction result.
        ret = 0;
    } else if (name == "GetCRTFlags") {
        ret = 0;
    } else if (name == "GetCRTStorageEx") {
        ret = a1 && a2 == 0x38 ? 0 : allocate(0x100, true);
    } else if (name == "_setjmp") {
        constexpr uint32_t kJmpBufMagic = 0x4a4d5032u; // "JMP2"
        ret = 0;
        if (a0 && isGuestRangeReadable(a0, 56)) {
            writeU32(a0 + 0, kJmpBufMagic);
            writeU32(a0 + 4, args.ra);
            writeU32(a0 + 8, reg(UC_MIPS_REG_SP));
            writeU32(a0 + 12, reg(UC_MIPS_REG_GP));
            writeU32(a0 + 16, reg(UC_MIPS_REG_S0));
            writeU32(a0 + 20, reg(UC_MIPS_REG_S1));
            writeU32(a0 + 24, reg(UC_MIPS_REG_S2));
            writeU32(a0 + 28, reg(UC_MIPS_REG_S3));
            writeU32(a0 + 32, reg(UC_MIPS_REG_S4));
            writeU32(a0 + 36, reg(UC_MIPS_REG_S5));
            writeU32(a0 + 40, reg(UC_MIPS_REG_S6));
            writeU32(a0 + 44, reg(UC_MIPS_REG_S7));
            writeU32(a0 + 48, reg(UC_MIPS_REG_FP));
            writeU32(a0 + 52, 0);
        }
    } else if (name == "longjmp") {
        constexpr uint32_t kJmpBufMagic = 0x4a4d5032u; // "JMP2"
        ret = a1 ? a1 : 1;
        if (a0 && isGuestRangeReadable(a0, 56) && readU32(a0) == kJmpBufMagic) {
            const uint32_t savedRa = readU32(a0 + 4);
            setReg(UC_MIPS_REG_RA, savedRa);
            setReg(UC_MIPS_REG_SP, readU32(a0 + 8));
            setReg(UC_MIPS_REG_GP, readU32(a0 + 12));
            setReg(UC_MIPS_REG_S0, readU32(a0 + 16));
            setReg(UC_MIPS_REG_S1, readU32(a0 + 20));
            setReg(UC_MIPS_REG_S2, readU32(a0 + 24));
            setReg(UC_MIPS_REG_S3, readU32(a0 + 28));
            setReg(UC_MIPS_REG_S4, readU32(a0 + 32));
            setReg(UC_MIPS_REG_S5, readU32(a0 + 36));
            setReg(UC_MIPS_REG_S6, readU32(a0 + 40));
            setReg(UC_MIPS_REG_S7, readU32(a0 + 44));
            setReg(UC_MIPS_REG_FP, readU32(a0 + 48));
            setReg(UC_MIPS_REG_PC, savedRa);
            spdlog::info("longjmp restored guest context jmpbuf=0x{:08x} pc=0x{:08x} value={}",
                         a0, savedRa, ret);
        } else {
            spdlog::warn("longjmp received invalid or uninitialized jmp_buf=0x{:08x}", a0);
        }
    } else {
        return false;
    }

    return true;
}

bool SyntheticDllRuntime::dispatchRegistryApi(const std::string& name,
                                              const GuestCallArgs& args,
                                              uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;

    if (name == "RegCreateKeyExW") {
        const std::string subKey = readUtf16(a1, 1024);
        const auto path = registryPathFromHandle(a0, subKey);
        const uint32_t resultPtr = stackArg(7);
        const uint32_t dispositionPtr = stackArg(8);
        if (!path || !resultPtr) {
            ret = 87;
            spdlog::info("RegCreateKeyExW invalid hkey=0x{:08x} subkey=\"{}\" resultPtr=0x{:08x}", a0, subKey, resultPtr);
        } else {
            const bool existed = registryKeyExists(*path);
            registryEnsureKey(*path);
            writeU32(resultPtr, makeRegistryHandle(*path));
            if (dispositionPtr) writeU32(dispositionPtr, existed ? 2 : 1);
            ret = 0;
            spdlog::info("RegCreateKeyExW path=\"{}\" existed={} -> {}", *path, existed, ret);
        }
    } else if (name == "RegOpenKeyExW") {
        const std::string subKey = readUtf16(a1, 1024);
        const auto path = registryPathFromHandle(a0, subKey);
        const uint32_t resultPtr = stackArg(4);
        if (!path || !resultPtr) {
            ret = 87;
            spdlog::info("RegOpenKeyExW invalid hkey=0x{:08x} subkey=\"{}\" resultPtr=0x{:08x}", a0, subKey, resultPtr);
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
    } else if (name == "RegQueryValueExW") {
        const auto path = registryPathFromHandle(a0, {});
        const std::string valueName = readUtf16(a1, 1024);
        const nlohmann::json* value = path ? registryValue(*path, valueName) : nullptr;
        const uint32_t typePtr = a3;
        const uint32_t dataPtr = stackArg(4);
        const uint32_t sizePtr = stackArg(5);
        if (!path || !sizePtr) {
            ret = 87;
            spdlog::info("RegQueryValueExW invalid hkey=0x{:08x} value=\"{}\" sizePtr=0x{:08x}", a0, valueName, sizePtr);
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
    } else if (name == "RegSetValueExW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path)) {
            ret = 6;
            spdlog::info("RegSetValueExW invalid hkey=0x{:08x} value=\"{}\" -> {}", a0, readUtf16(a1, 1024), ret);
        } else {
            const std::string valueName = normalizeRegistryValueName(readUtf16(a1, 1024));
            registry_["keys"][*path]["values"][valueName] = registryJsonFromBytes(a3, stackArg(4), stackArg(5));
            registryDirty_ = true;
            flushRegistry();
            ret = 0;
            spdlog::info("RegSetValueExW path=\"{}\" value=\"{}\" type={} size={} -> {}",
                         *path, valueName, registryTypeName(a3), stackArg(5), ret);
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
        if (!path || !a2 || !a3) ret = 87;
        else if (a1 >= children.size()) ret = 259;
        else {
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
        ret = handleRegEnumValueW(a0, a1, a2, a3);
    } else if (name == "RegQueryInfoKeyW") {
        const auto path = registryPathFromHandle(a0, {});
        if (!path || !registryKeyExists(*path)) ret = 6;
        else {
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
    } else {
        return false;
    }

    return true;
}

bool SyntheticDllRuntime::dispatchSimpleHostWin32(const std::string& name,
                                                  const GuestCallArgs& args,
                                                  uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;

    if (name == "ResumeThread") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::GuestThread ? 1 : 0xffffffffu;
        lastError_ = ret == 0xffffffffu ? 6 : 0;
    } else if (name == "GetCommState" || name == "SetCommState" || name == "SetCommTimeouts" ||
               name == "SetCommMask" || name == "SetupComm" || name == "PurgeComm" ||
               name == "ClearCommError") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || (handle->kind != GuestHandle::Kind::GuestSerialDevice &&
                        handle->kind != GuestHandle::Kind::HostSerialDevice)) {
            lastError_ = 6;
            ret = 0;
        } else if (handle->kind == GuestHandle::Kind::GuestSerialDevice) {
            auto debugName = fileHandleDebugNames_.find(a0);
            const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
            lastError_ = 120;
            ret = 0;
            spdlog::info("{} guest device handle=0x{:08x} name=\"{}\" has no host serial bridge yet -> 0",
                         name, a0, debugPath);
        } else {
#if defined(_WIN32)
            HANDLE host = reinterpret_cast<HANDLE>(handle->hostValue);
            BOOL ok = FALSE;
            if (name == "GetCommState") {
                DCB dcb{};
                dcb.DCBlength = sizeof(dcb);
                ok = GetCommState(host, &dcb);
                if (ok && a1) {
                    uint32_t guestLength = readU32(a1);
                    if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
                    uc_mem_write(uc_, a1, &dcb, guestLength);
                    writeU32(a1, guestLength);
                }
            } else if (name == "SetCommState") {
                DCB dcb{};
                dcb.DCBlength = sizeof(dcb);
                GetCommState(host, &dcb);
                if (a1) {
                    uint32_t guestLength = readU32(a1);
                    if (!guestLength || guestLength > sizeof(dcb)) guestLength = sizeof(dcb);
                    uc_mem_read(uc_, a1, &dcb, guestLength);
                    dcb.DCBlength = sizeof(dcb);
                    ok = SetCommState(host, &dcb);
                }
            } else if (name == "SetCommTimeouts") {
                COMMTIMEOUTS timeouts{};
                if (a1 && uc_mem_read(uc_, a1, &timeouts, sizeof(timeouts)) == UC_ERR_OK) {
                    ok = SetCommTimeouts(host, &timeouts);
                }
            } else if (name == "SetCommMask") {
                ok = SetCommMask(host, a1);
            } else if (name == "SetupComm") {
                ok = SetupComm(host, a1, a2);
            } else if (name == "PurgeComm") {
                ok = PurgeComm(host, a1);
            } else if (name == "ClearCommError") {
                DWORD errors = 0;
                COMSTAT stat{};
                ok = ClearCommError(host, &errors, &stat);
                if (ok && a1) writeU32(a1, errors);
                if (ok && a2) uc_mem_write(uc_, a2, &stat, sizeof(stat));
            }
            ret = ok ? 1 : 0;
            lastError_ = ok ? 0 : GetLastError();
#else
            ret = 0;
            lastError_ = 6;
#endif
        }
    } else if (name == "SetTimer") {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const uint32_t timerId = a1 ? a1 : uint32_t(++tick_);
            const uint32_t interval = std::max<uint32_t>(1, a2);
            timers_[guestTimerKey(a0, timerId)] = GuestTimer{
                a0, timerId, interval, args.a3, hostTickMilliseconds() + interval,
            };
            lastError_ = 0;
            ret = timerId;
        }
    } else if (name == "GetWindowTextLengthW") {
        auto it = windows_.find(a0);
        ret = it == windows_.end() ? 0 : uint32_t(it->second.title.size());
        lastError_ = it == windows_.end() ? 1400 : 0;
    } else if (name == "GetWindowTextW") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else if (!a1 || !a2) {
            lastError_ = 0;
            ret = 0;
        } else {
            ret = writeUtf16(a1, it->second.title, a2);
            lastError_ = 0;
        }
    } else if (name == "SetFocus") {
        auto target = windows_.find(a0);
        if (!a0 || (target != windows_.end() && !target->second.destroyed)) {
            ret = focusedWindow_;
            focusedWindow_ = a0;
            if (a0) guestMessages_.push_back({a0, 0x0007, 0, 0, uint32_t(++tick_ * 16), 0, 0});
            lastError_ = 0;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (name == "GetFocus") {
        ret = focusedWindow_;
        lastError_ = 0;
    } else if (name == "SetCapture") {
        auto target = windows_.find(a0);
        if (!a0 || (target != windows_.end() && !target->second.destroyed)) {
            ret = capturedWindow_;
            capturedWindow_ = a0;
            lastError_ = 0;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (name == "GetCapture") {
        ret = capturedWindow_;
        lastError_ = 0;
    } else if (name == "ReleaseCapture") {
        ret = capturedWindow_ ? 1 : 0;
        capturedWindow_ = 0;
        lastError_ = 0;
    } else if (name == "EnableWindow") {
        if (windows_.count(a0)) {
            lastError_ = 0;
            ret = 1;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else {
        return false;
    }
    return true;
}

bool SyntheticDllRuntime::dispatchHostWin32(const std::string& name,
                                            const GuestCallArgs& args,
                                            uint32_t& ret) {
    if (dispatchSimpleHostWin32(name, args, ret)) return true;

    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    const uint32_t ra = args.ra;
    if (name == "SystemParametersInfoW") {
        ret = handleSystemParametersInfoW(a0, a1, a2, a3);
        return true;
    }
    if (name == "LoadCursorW") {
        ret = handleLoadCursorW(a0, a1);
        return true;
    }
    if (name == "GetSysColorBrush") {
        ret = handleGetSysColorBrush(a0);
        return true;
    }
    if (name == "GetDeviceCaps") {
        ret = handleGetDeviceCaps(a0, a1);
        return true;
    }
    if (name == "DeviceIoControl") {
        ret = dispatchDeviceIoControl(a0, a1, a2, a3);
        return true;
    }
    if (name == "FlushFileBuffers") {
        auto* handle = lookupGuestHandle(a0);
        if (!handle || (handle->kind != GuestHandle::Kind::HostFile &&
                        handle->kind != GuestHandle::Kind::HostSerialDevice) ||
            !handle->hostValue) {
            lastError_ = 6;
            ret = 0;
        } else {
#if defined(_WIN32)
            const BOOL ok = FlushFileBuffers(reinterpret_cast<HANDLE>(handle->hostValue));
            ret = ok ? 1 : 0;
            lastError_ = ret ? 0 : GetLastError();
            auto debugName = fileHandleDebugNames_.find(a0);
            spdlog::info("FlushFileBuffers handle=0x{:08x} path=\"{}\" -> {} lastError={}",
                         a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
                         ret, lastError_);
#else
            lastError_ = 6;
            ret = 0;
#endif
        }
        return true;
    }
    if (name == "WideCharToMultiByte") {
        ret = handleWideCharToMultiByte(a0, a1, a2, a3);
        return true;
    }
    if (name == "CreateFileMappingW") {
        ret = handleCreateFileMappingW(a0, a1, a2, a3);
        return true;
    }
    if (name == "MapViewOfFile") {
        ret = handleMapViewOfFile(a0, a1, a2, a3);
        return true;
    }
    if (name == "UnmapViewOfFile") {
        ret = handleUnmapViewOfFile(a0);
        return true;
    }
    if (name == "FlushViewOfFile") {
        ret = handleFlushViewOfFile(a0, a1);
        return true;
    }
    if (name == "GetProcessIndexFromID") {
        // Windows CE process IDs are process handles; runtime evidence and CE
        // docs show the low six bits identify the process slot.
        ret = a0 & 0x3fu;
        return true;
    }
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
    auto windowOrigin = [&](uint32_t hwnd) {
        int32_t x = 0;
        int32_t y = 0;
        for (uint32_t current = hwnd; current;) {
            auto it = windows_.find(current);
            if (it == windows_.end()) break;
            x += it->second.x;
            y += it->second.y;
            current = it->second.parent;
        }
        return std::pair<int32_t, int32_t>{x, y};
    };
    auto firstWindow = [&]() -> uint32_t {
        for (const auto& [hwnd, window] : windows_) {
            if (!window.destroyed && !window.parent) return hwnd;
        }
        for (const auto& [hwnd, window] : windows_) {
            if (!window.destroyed) return hwnd;
        }
        return 0;
    };

    if (name == "IsProcessDying") {
        ret = 0;
    } else if (name == "GetLastError") {
        ret = lastError_;
    } else if (name == "SetLastError") {
        lastError_ = a0;
        ret = 0;
    } else if (name == "IsBadReadPtr" || name == "IsBadWritePtr") {
        ret = isGuestRangeReadable(a0, a1) ? 0 : 1;
        lastError_ = 0;
    } else if (name == "KernelIoControl") {
        const uint32_t outPtr = a3;
        const uint32_t outSize = stackArg(4);
        const uint32_t bytesReturnedPtr = stackArg(5);
        const uint32_t function = (a0 >> 2) & 0x0fffu;
        const uint32_t method = a0 & 0x3u;
        const uint32_t device = (a0 >> 16) & 0xffffu;
        const uint32_t access = (a0 >> 14) & 0x3u;
        auto registryData = [](const nlohmann::json* value) -> const nlohmann::json* {
            if (!value) return nullptr;
            const auto data = value->find("data");
            return data != value->end() ? &*data : nullptr;
        };
        auto parseIoctlCmd = [&](const nlohmann::json* value, uint32_t& command) -> bool {
            const nlohmann::json* data = registryData(value);
            if (!data) return false;
            if (data->is_number_unsigned()) {
                const uint64_t raw = data->get<uint64_t>();
                if (raw > 0xffffffffull) return false;
                command = uint32_t(raw);
                return true;
            }
            if (data->is_number_integer()) {
                const int64_t raw = data->get<int64_t>();
                if (raw < 0 || raw > 0xffffffffll) return false;
                command = uint32_t(raw);
                return true;
            }
            if (!data->is_string()) return false;
            std::string text = lowerAscii(data->get<std::string>());
            if (text.rfind("0x", 0) == 0) text.erase(0, 2);
            if (text.empty()) return false;
            char* end = nullptr;
            const unsigned long raw = std::strtoul(text.c_str(), &end, 16);
            if (!end || *end || raw > 0xfffffffful) return false;
            command = uint32_t(raw);
            return true;
        };
        auto configuredIoctlReturn = [&]() -> const nlohmann::json* {
            const std::string base = "hklm\\system\\emulator\\kernelioctl";
            std::vector<std::string> paths{base};
            for (const std::string& child : registryChildNames(base)) {
                paths.push_back(base + "\\" + child);
            }
            for (const std::string& path : paths) {
                uint32_t command = 0;
                if (!parseIoctlCmd(registryValue(path, "ioctlcmd"), command) || command != a0) continue;
                if (const nlohmann::json* value = registryValue(path, "return")) return value;
            }
            return nullptr;
        };
        auto writeConfiguredIoctlReturn = [&](const nlohmann::json& configured) -> bool {
            const nlohmann::json* data = registryData(&configured);
            if (!data || !outPtr) return false;
            if ((data->is_number_unsigned() || data->is_number_integer()) && outSize >= 4) {
                uint32_t value = 0;
                if (data->is_number_unsigned()) {
                    const uint64_t raw = data->get<uint64_t>();
                    if (raw > 0xffffffffull) return false;
                    value = uint32_t(raw);
                } else {
                    const int64_t raw = data->get<int64_t>();
                    if (raw < 0 || raw > 0xffffffffll) return false;
                    value = uint32_t(raw);
                }
                writeU32(outPtr, value);
                if (bytesReturnedPtr) writeU32(bytesReturnedPtr, 4);
                spdlog::info("KernelIoControl registry return cmd=0x{:08x} dword=0x{:08x}", a0, value);
                return true;
            }
            if (data->is_string()) {
                const std::string value = data->get<std::string>();
                const uint32_t capacityBytes = outSize ? outSize : uint32_t(value.size() + 1);
                if (!capacityBytes) return false;
                const uint32_t bytesToWrite = std::min<uint32_t>(uint32_t(value.size()), capacityBytes - 1);
                if (bytesToWrite) uc_mem_write(uc_, outPtr, value.data(), bytesToWrite);
                const char nul = 0;
                uc_mem_write(uc_, outPtr + bytesToWrite, &nul, sizeof(nul));
                if (bytesReturnedPtr) writeU32(bytesReturnedPtr, bytesToWrite + 1);
                spdlog::info("KernelIoControl registry return cmd=0x{:08x} string=\"{}\" bytes={} capacityBytes={}",
                             a0, value, bytesToWrite, capacityBytes);
                return true;
            }
            return false;
        };
        ret = 0;
        lastError_ = 120;
        if (const nlohmann::json* configured = configuredIoctlReturn()) {
            if (writeConfiguredIoctlReturn(*configured)) {
                lastError_ = 0;
                ret = 1;
            }
        }
        spdlog::info("KernelIoControl code=0x{:08x} device=0x{:04x} function=0x{:03x} method={} access={} out=0x{:08x} outSize={} bytesReturned=0x{:08x} -> {} lastError={}",
                     a0, device, function, method, access, outPtr, outSize, bytesReturnedPtr, ret, lastError_);
    } else if (name == "KernelLibIoControl") {
        lastError_ = 120;
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
    } else if (name == "FileTimeToSystemTime") {
#if defined(_WIN32)
        FILETIME ft{};
        SYSTEMTIME st{};
        if (a0) uc_mem_read(uc_, a0, &ft, sizeof(ft));
        const BOOL ok = a0 && a1 && ::FileTimeToSystemTime(&ft, &st);
        if (ok) uc_mem_write(uc_, a1, &st, sizeof(st));
        ret = ok ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
#else
        if (a1) fillGuest(a1, 0, 16);
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
        auto debugName = fileHandleDebugNames_.find(a0);
        const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
        const std::string lowerPath = lowerAscii(debugPath);
        if (ra == 0x0006bea4u && lowerPath.find("values.dat") != std::string::npos) {
            uint16_t recordCount = 0;
            const uint32_t sp = reg(UC_MIPS_REG_SP);
            uc_mem_read(uc_, sp + 0x20, &recordCount, sizeof(recordCount));
            const uint32_t requestedId = reg(UC_MIPS_REG_S3);
            const uint32_t scannedRecords = reg(UC_MIPS_REG_S5);
            spdlog::warn("values.dat lookup miss handle=0x{:08x} path=\"{}\" requestedId={} (0x{:04x}) scanned={} recordCount={} ra=0x{:08x}",
                         a0, debugPath, int16_t(requestedId & 0xffffu), requestedId & 0xffffu,
                         scannedRecords, recordCount, ra);
        }
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
    } else if (name == "CreateThread") {
        const uint32_t startAddress = a2;
        const uint32_t parameter = a3;
        const uint32_t flags = stackArg(4);
        const uint32_t threadIdPtr = stackArg(5);
        ret = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
        if (threadIdPtr) writeU32(threadIdPtr, ret);
        lastError_ = 0;
        spdlog::info("CreateThread guestHandle=0x{:08x} start=0x{:08x} param=0x{:08x} flags=0x{:08x} idPtr=0x{:08x}",
                     ret, startAddress, parameter, flags, threadIdPtr);
    } else if (name == "SetThreadPriority" || name == "CeSetThreadPriority") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::GuestThread ? 1 : 0;
        lastError_ = ret ? 0 : 6;
    } else if (name == "GetThreadPriority") {
        auto* handle = lookupGuestHandle(a0);
        ret = handle && handle->kind == GuestHandle::Kind::GuestThread ? 251 : 0xffffffffu;
        lastError_ = ret == 0xffffffffu ? 6 : 0;
    } else if (name == "CreateFileW") {
#if defined(_WIN32)
        const std::string guestPath = readUtf16(a0);
        if (isGuestDevicePath(guestPath)) {
            ret = openGuestSerialDevice(guestPath, a1, a2);
        } else {
            const std::filesystem::path hostPath = resolveGuestPath(guestPath);
            HANDLE host = INVALID_HANDLE_VALUE;
            if (!hostPath.empty()) {
                host = CreateFileW(hostPath.wstring().c_str(), a1, a2, nullptr, stackArg(4), stackArg(5), nullptr);
            }
            if (host == INVALID_HANDLE_VALUE) {
                lastError_ = normalizeVirtualFileMiss(hostPath, GetLastError());
                ret = 0xffffffffu;
                spdlog::warn("CreateFileW miss guest=\"{}\" host=\"{}\" access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x} lastError={}",
                             guestPath, pathToUtf8(hostPath), a1, a2, stackArg(4), stackArg(5), lastError_);
            } else {
                ret = makeGuestHandle({GuestHandle::Kind::HostFile, reinterpret_cast<uintptr_t>(host), 0});
                fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
                lastError_ = 0;
                spdlog::info("CreateFileW hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} creation=0x{:08x} flags=0x{:08x}",
                             guestPath, pathToUtf8(hostPath), ret, a1, a2, stackArg(4), stackArg(5));
            }
        }
#else
        lastError_ = 2;
        ret = 0xffffffffu;
#endif
    } else if (name == "CreateDirectoryW") {
#if defined(_WIN32)
        const std::string guestPath = readUtf16(a0);
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
#else
        lastError_ = 2;
        ret = 0;
#endif
    } else if (name == "GetFileAttributesW") {
#if defined(_WIN32)
        const std::string guestPath = readUtf16(a0);
        const std::filesystem::path hostPath = resolveGuestPath(guestPath);
        ret = hostPath.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(hostPath.wstring().c_str());
        lastError_ = ret == INVALID_FILE_ATTRIBUTES
            ? normalizeVirtualFileMiss(hostPath, hostPath.empty() ? ERROR_PATH_NOT_FOUND : GetLastError())
            : 0;
        spdlog::info("GetFileAttributesW guest=\"{}\" host=\"{}\" -> 0x{:08x} lastError={}",
                     guestPath, pathToUtf8(hostPath), ret, lastError_);
#else
        lastError_ = 2;
        ret = 0xffffffffu;
#endif
    } else if (name == "GetFileAttributesExW") {
#if defined(_WIN32)
        const std::string guestPath = readUtf16(a0);
        const std::filesystem::path hostPath = resolveGuestPath(guestPath);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const BOOL ok = !hostPath.empty() && a1 == GetFileExInfoStandard &&
                        GetFileAttributesExW(hostPath.wstring().c_str(), GetFileExInfoStandard, &data);
        if (ok) {
            writeGuestFileAttributeData(uc_, a2, data);
            ret = 1;
            lastError_ = 0;
        } else {
            ret = 0;
            const uint32_t error = hostPath.empty()
                ? ERROR_PATH_NOT_FOUND
                : (a1 == GetFileExInfoStandard ? GetLastError() : ERROR_INVALID_PARAMETER);
            lastError_ = normalizeVirtualFileMiss(hostPath, error);
        }
        spdlog::info("GetFileAttributesExW guest=\"{}\" host=\"{}\" level={} out=0x{:08x} -> {} lastError={}",
                     guestPath, pathToUtf8(hostPath), a1, a2, ret, lastError_);
#else
        lastError_ = 2;
        ret = 0;
#endif
    } else if (name == "CreateProcessW") {
        const std::string application = readUtf16(a0, 2048);
        const std::string commandLine = readUtf16(a1, 4096);
        const uint32_t processInfo = stackArg(9);
        const std::filesystem::path hostApplication = application.empty()
            ? std::filesystem::path{}
            : resolveGuestPath(application);
        if (!application.empty() && (hostApplication.empty() || !std::filesystem::exists(hostApplication))) {
            lastError_ = 2;
            ret = 0;
        } else {
            const uint32_t processHandle = makeGuestHandle({GuestHandle::Kind::GuestProcess, 0, 0});
            const uint32_t threadHandle = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
            if (processInfo) {
                writeU32(processInfo, processHandle);
                writeU32(processInfo + 4, threadHandle);
                writeU32(processInfo + 8, processHandle);
                writeU32(processInfo + 12, threadHandle);
            }
            lastError_ = 0;
            ret = 1;
        }
        spdlog::info("CreateProcessW app=\"{}\" host=\"{}\" cmd=\"{}\" pi=0x{:08x} -> {} lastError={}",
                     application, pathToUtf8(hostApplication), commandLine, processInfo, ret, lastError_);
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
        const std::string guestPath = readUtf16(a0);
        const std::filesystem::path hostPath = resolveGuestPath(guestPath);
        WIN32_FIND_DATAW data{};
        HANDLE host = INVALID_HANDLE_VALUE;
        if (!hostPath.empty()) {
            host = FindFirstFileW(hostPath.wstring().c_str(), &data);
        }
        if (host == INVALID_HANDLE_VALUE) {
            lastError_ = normalizeVirtualFileMiss(hostPath, GetLastError());
            ret = 0xffffffffu;
            spdlog::warn("FindFirstFileW miss guest=\"{}\" host=\"{}\" lastError={}",
                         guestPath, pathToUtf8(hostPath), lastError_);
        } else {
            data = translateGuestFindData(data, isRootWildcardPattern(guestPath));
            writeGuestFindData(uc_, a1, data);
            ret = makeGuestHandle({GuestHandle::Kind::HostFind, reinterpret_cast<uintptr_t>(host),
                                   isRootWildcardPattern(guestPath) ? 1u : 0u});
            fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
            lastError_ = 0;
            spdlog::info("FindFirstFileW hit guest=\"{}\" host=\"{}\" guestHandle=0x{:08x} file=\"{}\" attr=0x{:08x} size={}",
                         guestPath, pathToUtf8(hostPath), ret,
                         wideZToUtf8(data.cFileName, 260),
                         data.dwFileAttributes,
                         (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow);
        }
#else
        lastError_ = 2;
        ret = 0xffffffffu;
#endif
    } else if (name == "ReadFile" || name == "WriteFile") {
        auto* handle = lookupGuestHandle(a0);
        writeU32(a3, 0);
        if (handle && handle->kind == GuestHandle::Kind::GuestSerialDevice) {
            ret = 1;
            lastError_ = 0;
            auto debugName = fileHandleDebugNames_.find(a0);
            const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
            spdlog::info("{} guest device handle=0x{:08x} name=\"{}\" requested={} transferred=0",
                         name, a0, debugPath, a2);
        } else if (!handle || (handle->kind != GuestHandle::Kind::HostFile &&
                               handle->kind != GuestHandle::Kind::HostSerialDevice) ||
                   !handle->hostValue) {
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
                const uint32_t readCount = ++fileReadCounts_[a0];
                auto debugName = fileHandleDebugNames_.find(a0);
                const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
                if (readCount <= 32 || !ok || transferred != a2 || transferred == 0) {
                    spdlog::info("ReadFile handle=0x{:08x} path=\"{}\" requested={} transferred={} ok={} read#={}",
                                 a0, debugPath,
                                 a2, transferred, ok ? 1 : 0, readCount);
                }
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
            auto debugName = fileHandleDebugNames_.find(a0);
            spdlog::info("GetFileSize handle=0x{:08x} path=\"{}\" size={} high={} lastError={}",
                         a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
                         ret, high, lastError_);
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
            const uint32_t seekCount = ++fileSeekCounts_[a0];
            if (seekCount <= 32 || ret == 0xffffffffu) {
                auto debugName = fileHandleDebugNames_.find(a0);
                spdlog::info("SetFilePointer handle=0x{:08x} path=\"{}\" distance={} method={} -> low={} high={} lastError={} seek#={}",
                             a0, debugName == fileHandleDebugNames_.end() ? "" : debugName->second,
                             int32_t(a1), a3, ret, uint32_t(high), lastError_, seekCount);
            }
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
    } else if (name == "FormatMessageW") {
        const uint32_t bufferPtr = stackArg(4);
        const uint32_t capacity = stackArg(5);
        std::string message = "Error " + std::to_string(a2);
#if defined(_WIN32)
        wchar_t hostBuffer[512]{};
        const DWORD flags = a0 & ~0x00000100u;
        const DWORD written = ::FormatMessageW(flags, nullptr, a2, a3, hostBuffer,
                                               DWORD(std::size(hostBuffer)), nullptr);
        if (written) {
            message.clear();
            for (const wchar_t* p = hostBuffer; *p; ++p) message.push_back(*p < 0x80 ? char(*p) : '?');
        }
#endif
        if (a0 & 0x00000100u) {
            const uint32_t guestText = allocate(uint32_t((message.size() + 1) * 2), true);
            writeUtf16(guestText, message, uint32_t(message.size() + 1));
            if (bufferPtr) writeU32(bufferPtr, guestText);
            ret = uint32_t(message.size());
        } else {
            ret = bufferPtr && capacity ? writeUtf16(bufferPtr, message, capacity) : 0;
        }
        lastError_ = ret ? 0 : 122;
    } else if (name == "LoadLibraryW" || name == "GetModuleHandleW") {
        const std::string requested = readUtf16(a0);
        const std::string pathKey = lowerAscii(requested);
        const std::string nameKey = lowerAscii(pathToUtf8(pathFromUtf8(requested).filename()));
        if (requested.empty()) {
            ret = mainModuleBase_;
        } else if (auto it = loadedModulesByPath_.find(pathKey); it != loadedModulesByPath_.end()) {
            ret = it->second.base;
        } else if (auto it = loadedModulesByName_.find(nameKey); it != loadedModulesByName_.end()) {
            ret = it->second.base;
        } else if (name == "LoadLibraryW") {
            if (auto syntheticModule = createModule(nameKey)) {
                registerLoadedModule(syntheticModule->moduleName,
                                     std::filesystem::path("[synthetic]") / syntheticModule->moduleName,
                                     syntheticModule->imageBase,
                                     syntheticModule->exportsByName,
                                     syntheticModule->exportsByOrdinal);
                ret = syntheticModule->imageBase;
            } else {
                ret = 0;
            }
        } else {
            ret = 0;
        }
        spdlog::info("{} requested=\"{}\" -> 0x{:08x}", name, requested, ret);
        lastError_ = ret ? 0 : 126;
    } else if (name == "GetProcAddressA" || name == "GetProcAddressW") {
        ret = 0;
        auto module = loadedModulesByBase_.find(a0);
        if (module != loadedModulesByBase_.end()) {
            if (a1 < 0x10000) {
                auto ordinal = module->second.exportsByOrdinal.find(uint16_t(a1));
                if (ordinal != module->second.exportsByOrdinal.end()) ret = module->second.base + ordinal->second;
                spdlog::info("{} module=0x{:08x} ordinal={} -> 0x{:08x}",
                             name, a0, a1, ret);
            } else {
                const std::string proc = name == "GetProcAddressW" ? readUtf16(a1) : readAscii(a1, 256);
                auto exported = module->second.exportsByName.find(lowerAscii(proc));
                if (exported != module->second.exportsByName.end()) ret = module->second.base + exported->second;
                spdlog::info("{} module=0x{:08x} proc=\"{}\" -> 0x{:08x}",
                             name, a0, proc, ret);
            }
        }
        lastError_ = ret ? 0 : 127;
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
    } else if (name == "SetCursor") {
        ret = currentCursor_;
        currentCursor_ = a0;
#if defined(_WIN32)
        auto handle = guestHandles_.find(a0);
        HCURSOR hostCursor = nullptr;
        if (handle != guestHandles_.end() && handle->second.hostValue) {
            hostCursor = reinterpret_cast<HCURSOR>(handle->second.hostValue);
        }
        ::SetCursor(hostCursor);
#endif
        lastError_ = 0;
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
        const int colorIndex = int(a0 & 0xffu);
#if defined(_WIN32)
        ret = uint32_t(GetSysColor(colorIndex));
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
            spdlog::info("RegisterClassW class=\"{}\" atom=0x{:04x}", className, ret);
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
        spdlog::info("CreateWindowExW guest=0x{:08x} class=\"{}\" title=\"{}\" parent=0x{:08x} id/menu=0x{:08x} "
                     "style=0x{:08x} ex=0x{:08x} wndproc=0x{:08x} param=0x{:08x} rect={},{} {}x{}",
                     ret, className, window.title, parent, menu, window.style, window.exStyle,
                     wndProc, param, window.x, window.y, window.width, window.height);
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
            window.createStruct = createStruct;
        }
        windows_[ret] = window;
        ensureHostWindow(ret, windows_[ret]);
        GuestMessage size{};
        size.hwnd = ret;
        size.message = 0x0005; // WM_SIZE
        size.lParam = (uint32_t(uint16_t(window.height)) << 16) | uint16_t(window.width);
        size.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(size);
        lastError_ = 0;
    } else if (name == "GetWindowRect") {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            const auto [x, y] = windowOrigin(a0);
            writeGuestRect(a1, x, y, x + it->second.width, y + it->second.height);
            lastError_ = 0;
            ret = 1;
        }
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
    } else if (name == "AdjustWindowRectEx") {
        int32_t rect[4]{};
        if (!a0 || uc_mem_read(uc_, a0, rect, sizeof(rect)) != UC_ERR_OK) {
            lastError_ = 87;
            ret = 0;
        } else {
#if defined(_WIN32)
            RECT hostRect{rect[0], rect[1], rect[2], rect[3]};
            ret = ::AdjustWindowRectEx(&hostRect, a1, a2 != 0, a3) ? 1 : 0;
            if (ret) {
                writeGuestRect(a0, hostRect.left, hostRect.top, hostRect.right, hostRect.bottom);
                lastError_ = 0;
            } else {
                lastError_ = ::GetLastError();
            }
#else
            lastError_ = 0;
            ret = 1;
#endif
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
    } else if (name == "GetDlgCtrlID") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0xffffffffu;
        } else {
            lastError_ = 0;
            ret = it->second.menu;
        }
    } else if (name == "InvalidateRect") {
        if (!windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            GuestMessage erase{};
            erase.hwnd = a0;
            erase.message = 0x0014; // WM_ERASEBKGND
            erase.wParam = makeGuestDc(a0);
            erase.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(erase);
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
    } else if (name == "ScreenToClient") {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            int32_t x = int32_t(readU32(a1));
            int32_t y = int32_t(readU32(a1 + 4));
            const auto [originX, originY] = windowOrigin(a0);
            writeU32(a1, uint32_t(x - originX));
            writeU32(a1 + 4, uint32_t(y - originY));
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "KillTimer") {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            timers_.erase(guestTimerKey(a0, a1));
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
    } else if (name == "CreatePen") {
        ret = makeGuestPen(a0, a1, a2);
        lastError_ = 0;
    } else if (name == "CreateSolidBrush") {
        ret = makeGuestBrush(a0);
        lastError_ = 0;
    } else if (name == "CreateRectRgn") {
#if defined(_WIN32)
        HRGN region = CreateRectRgn(int(a0), int(a1), int(a2), int(a3));
        ret = region ? makeGuestHandle({GuestHandle::Kind::HostRegion, reinterpret_cast<uintptr_t>(region), 0}) : 0;
        lastError_ = ret ? 0 : GetLastError();
#else
        ret = makeGuestHandle({GuestHandle::Kind::HostRegion, 0, 0});
        lastError_ = ret ? 0 : 8;
#endif
    } else if (name == "CombineRgn") {
#if defined(_WIN32)
        auto dest = guestHandles_.find(a0);
        auto src1 = guestHandles_.find(a1);
        auto src2 = guestHandles_.find(a2);
        const bool needSrc2 = a3 != RGN_COPY;
        if (dest == guestHandles_.end() || src1 == guestHandles_.end() ||
            dest->second.kind != GuestHandle::Kind::HostRegion ||
            src1->second.kind != GuestHandle::Kind::HostRegion ||
            !dest->second.hostValue || !src1->second.hostValue ||
            (needSrc2 && (src2 == guestHandles_.end() ||
                          src2->second.kind != GuestHandle::Kind::HostRegion ||
                          !src2->second.hostValue))) {
            lastError_ = 6;
            ret = 0;
        } else {
            HRGN hostSrc2 = needSrc2 ? reinterpret_cast<HRGN>(src2->second.hostValue) : nullptr;
            ret = uint32_t(CombineRgn(reinterpret_cast<HRGN>(dest->second.hostValue),
                                      reinterpret_cast<HRGN>(src1->second.hostValue),
                                      hostSrc2,
                                      int(a3)));
            lastError_ = ret ? 0 : GetLastError();
        }
#else
        ret = 0;
        lastError_ = 120;
#endif
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
        } else if (object->second.kind == GuestHandle::Kind::GuestPen) {
            ret = dc->selectedPen;
            dc->selectedPen = a1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestFont) {
            ret = dc->selectedFont;
            dc->selectedFont = a1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostBitmap && bitmaps_.count(a1)) {
            ret = dc->selectedBitmap;
            dc->selectedBitmap = a1;
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
        } else if (object->second.filePointer && object->second.kind != GuestHandle::Kind::HostBitmap) {
            ret = 0;
            lastError_ = 6;
        } else if (object->second.kind == GuestHandle::Kind::GuestBrush) {
            brushes_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestPen) {
            pens_.erase(a0);
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
            bitmaps_.erase(a0);
            if (object->second.filePointer) releaseAllocation(object->second.filePointer);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostRegion) {
#if defined(_WIN32)
            if (object->second.hostValue) DeleteObject(reinterpret_cast<HRGN>(object->second.hostValue));
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
        auto pen = dc ? pens_.find(dc->selectedPen) : pens_.end();
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        if (!dc || (pen == pens_.end() && brush == brushes_.end())) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            const uint32_t colorRef = pen != pens_.end() ? pen->second.colorRef : brush->second.colorRef;
            drawFramebufferLine(*dc, dc->x, dc->y, int32_t(a1), int32_t(a2),
                                colorRefToPixel(colorRef));
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
            const bool supported = stackArg(11) == 0 && stackArg(12) == 0x00cc0020u;
            auto dstBitmap = bitmaps_.find(dc->selectedBitmap);
            const bool ok = supported && (dstBitmap != bitmaps_.end()
                ? stretchDibToBitmap(dstBitmap->second, int32_t(a1), int32_t(a2), int32_t(a3),
                                     int32_t(stackArg(4)), int32_t(stackArg(5)),
                                     int32_t(stackArg(6)), int32_t(stackArg(7)),
                                     int32_t(stackArg(8)), stackArg(9), stackArg(10))
                : stretchDibToFramebuffer(*dc, int32_t(a1), int32_t(a2), int32_t(a3),
                                          int32_t(stackArg(4)), int32_t(stackArg(5)),
                                          int32_t(stackArg(6)), int32_t(stackArg(7)),
                                          int32_t(stackArg(8)), stackArg(9), stackArg(10)));
            if (ok) {
                ret = uint32_t(std::abs(int32_t(stackArg(8))));
                lastError_ = 0;
            } else {
                spdlog::info("StretchDIBits failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dst={}x{} src={}x{} srcOrigin={},{} bits=0x{:08x} info=0x{:08x} "
                             "usage={} rop=0x{:08x}",
                             a0, dc->selectedBitmap, int32_t(a3), int32_t(stackArg(4)),
                             int32_t(stackArg(7)), int32_t(stackArg(8)), int32_t(stackArg(5)),
                             int32_t(stackArg(6)), stackArg(9), stackArg(10), stackArg(11), stackArg(12));
                lastError_ = 120;
                ret = 0xffffffffu;
            }
        }
    } else if (name == "TransparentImage") {
        GuestDc* dstDc = lookupGuestDc(a0);
        GuestDc* srcDc = lookupGuestDc(stackArg(5));
        auto srcBitmap = srcDc ? bitmaps_.find(srcDc->selectedBitmap) : bitmaps_.end();
        auto dstBitmap = dstDc ? bitmaps_.find(dstDc->selectedBitmap) : bitmaps_.end();
        const int32_t dstH = int32_t(stackArg(4));
        const int32_t srcX = int32_t(stackArg(6));
        const int32_t srcY = int32_t(stackArg(7));
        const int32_t srcW = int32_t(stackArg(8));
        const int32_t srcH = int32_t(stackArg(9));
        const uint32_t transparentColor = stackArg(10);
        if (!dstDc || !srcDc || srcBitmap == bitmaps_.end()) {
            spdlog::info("TransparentImage unsupported dst=0x{:08x} dstBitmap=0x{:08x} src=0x{:08x} "
                         "srcBitmap=0x{:08x} dst={}x{} src={}x{} srcOrigin={},{} color=0x{:08x}",
                         a0, dstDc ? dstDc->selectedBitmap : 0, stackArg(5),
                         srcDc ? srcDc->selectedBitmap : 0, int32_t(a3), dstH,
                         srcW, srcH, srcX, srcY, transparentColor);
            lastError_ = dstDc && srcDc ? 120 : 6;
            ret = 0;
        } else if (dstBitmap != bitmaps_.end()) {
            const bool ok = transparentImageToBitmap(dstBitmap->second, srcBitmap->second,
                                                     int32_t(a1), int32_t(a2), int32_t(a3), dstH,
                                                     srcX, srcY, srcW, srcH, transparentColor);
            if (!ok) {
                spdlog::info("TransparentImage bitmap blit failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dstBits=0x{:08x} dstSize={}x{} dstBpp={} src=0x{:08x} srcBitmap=0x{:08x} "
                             "srcBits=0x{:08x} srcSize={}x{} srcBpp={} dst={}x{} src={}x{} "
                             "srcOrigin={},{} color=0x{:08x}",
                             a0, dstDc->selectedBitmap, dstBitmap->second.bits, dstBitmap->second.width,
                             dstBitmap->second.heightRaw, dstBitmap->second.bpp, stackArg(5),
                             srcDc->selectedBitmap, srcBitmap->second.bits, srcBitmap->second.width,
                             srcBitmap->second.heightRaw, srcBitmap->second.bpp, int32_t(a3), dstH,
                             srcW, srcH, srcX, srcY, transparentColor);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        } else {
            const bool ok = transparentImageToFramebuffer(*dstDc, srcBitmap->second,
                                                          int32_t(a1), int32_t(a2), int32_t(a3), dstH,
                                                          srcX, srcY, srcW, srcH, transparentColor);
            if (!ok) {
                spdlog::info("TransparentImage framebuffer blit failed dst=0x{:08x} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} color=0x{:08x}",
                             a0, stackArg(5), srcDc->selectedBitmap, srcBitmap->second.bits,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcBitmap->second.bpp,
                             int32_t(a3), dstH, srcW, srcH, srcX, srcY, transparentColor);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
    } else if (name == "ExtTextOutW" || name == "DrawTextW") {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0;
        } else {
            const bool drawTextCall = name == "DrawTextW";
            const bool ok = drawTextCall
                ? drawHostTextToDc(*dc, 0, 0, 0, a2, a1, int32_t(a3), stackArg(4), true)
                : drawHostTextToDc(*dc, int32_t(a1), int32_t(a2), a3, stackArg(4),
                                   stackArg(5), int32_t(stackArg(6)), 0, false);
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
    } else if (name == "CreateCompatibleDC") {
        ret = makeGuestDc(0);
        if (GuestDc* dc = lookupGuestDc(ret)) {
            dc->selectedBitmap = 0;
        }
        lastError_ = ret ? 0 : 8;
    } else if (name == "DeleteDC") {
        auto handle = guestHandles_.find(a0);
        if (handle == guestHandles_.end() || handle->second.kind != GuestHandle::Kind::GuestDc) {
            lastError_ = 6;
            ret = 0;
        } else {
            dcs_.erase(a0);
            guestHandles_.erase(handle);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "BitBlt" || name == "StretchBlt") {
        GuestDc* dstDc = lookupGuestDc(a0);
        GuestDc* srcDc = lookupGuestDc(stackArg(5));
        const bool stretch = name == "StretchBlt";
        const uint32_t rop = stretch ? stackArg(10) : stackArg(8);
        const int32_t dstW = int32_t(a3);
        const int32_t dstH = int32_t(stackArg(4));
        const int32_t srcX = int32_t(stackArg(6));
        const int32_t srcY = int32_t(stackArg(7));
        const int32_t srcW = stretch ? int32_t(stackArg(8)) : dstW;
        const int32_t srcH = stretch ? int32_t(stackArg(9)) : dstH;
        auto srcBitmap = srcDc ? bitmaps_.find(srcDc->selectedBitmap) : bitmaps_.end();
        auto dstBitmap = dstDc ? bitmaps_.find(dstDc->selectedBitmap) : bitmaps_.end();
        if (!dstDc || !srcDc || srcBitmap == bitmaps_.end() || !supportedSourceRasterOp(rop)) {
            spdlog::info("{} unsupported dst=0x{:08x} dstBitmap=0x{:08x} src=0x{:08x} srcBitmap=0x{:08x} "
                         "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                         name, a0, dstDc ? dstDc->selectedBitmap : 0, stackArg(5),
                         srcDc ? srcDc->selectedBitmap : 0, dstW, dstH, srcW, srcH,
                         srcX, srcY, rop);
            lastError_ = dstDc && srcDc ? 120 : 6;
            ret = 0;
        } else if (dstW == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
            lastError_ = 0;
            ret = 1;
        } else if (dstBitmap != bitmaps_.end()) {
            const bool ok = bitBltToBitmap(dstBitmap->second, srcBitmap->second,
                                           int32_t(a1), int32_t(a2), dstW, dstH,
                                           srcX, srcY, srcW, srcH, rop);
            const int32_t srcBitmapHeight = std::abs(srcBitmap->second.heightRaw);
            const int32_t dstBitmapHeight = std::abs(dstBitmap->second.heightRaw);
            const bool splashSlice =
                ok && srcBitmap->second.width == 800 && dstBitmap->second.width == 800 &&
                dstBitmapHeight == 480 && std::abs(dstW) == 800 &&
                (std::abs(dstH) == 160 || std::abs(dstH) == 320) &&
                std::abs(srcW) == 800 && std::abs(srcH) == std::abs(dstH) &&
                (srcBitmapHeight == 160 || srcBitmapHeight == 320);
            bool dumpSplashSlice = false;
            std::string splashTag;
            if (splashSlice && srcBitmapHeight == 160 && !splashTopBlitDumped_) {
                splashTopBlitDumped_ = true;
                dumpSplashSlice = true;
                splashTag = name + "_splash_top";
            } else if (splashSlice && srcBitmapHeight == 320 && !splashBottomBlitDumped_) {
                splashBottomBlitDumped_ = true;
                dumpSplashSlice = true;
                splashTag = name + "_splash_bottom";
            }
            if (dumpSplashSlice) {
                splashCompositeBitmap_ = dstDc->selectedBitmap;
                dumpGuestBitmapPpm(srcDc->selectedBitmap, srcBitmap->second, splashTag + "_source");
                dumpGuestBitmapPpm(dstDc->selectedBitmap, dstBitmap->second, splashTag + "_result");
                spdlog::info("{} splash probe dstDc=0x{:08x} dstBitmap=0x{:08x} dstSize={}x{} "
                             "srcDc=0x{:08x} srcBitmap=0x{:08x} srcSize={}x{} dst={}x{} "
                             "src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, dstDc->selectedBitmap, dstBitmap->second.width,
                             dstBitmap->second.heightRaw, stackArg(5), srcDc->selectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, dstW, dstH,
                             srcW, srcH, srcX, srcY, rop);
            }
            if (!ok) {
                spdlog::info("{} bitmap blit failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dstBits=0x{:08x} dstSize={}x{} dstBpp={} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, dstDc->selectedBitmap, dstBitmap->second.bits,
                             dstBitmap->second.width, dstBitmap->second.heightRaw, dstBitmap->second.bpp,
                             stackArg(5), srcDc->selectedBitmap, srcBitmap->second.bits,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcBitmap->second.bpp,
                             dstW, dstH, srcW, srcH, srcX, srcY, rop);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        } else {
            const bool ok = bitBltToFramebuffer(*dstDc, srcBitmap->second,
                                                int32_t(a1), int32_t(a2), dstW, dstH,
                                                srcX, srcY, srcW, srcH, rop);
            const bool splashFrame =
                ok && !splashFramebufferDumped_ && srcDc->selectedBitmap == splashCompositeBitmap_ &&
                srcBitmap->second.width == 800 && std::abs(srcBitmap->second.heightRaw) == 480 &&
                std::abs(dstW) == 800 && std::abs(dstH) == 480 &&
                std::abs(srcW) == 800 && std::abs(srcH) == 480;
            if (splashFrame) {
                splashFramebufferDumped_ = true;
                const std::string tag = name + "_splash_framebuffer";
                dumpGuestBitmapPpm(srcDc->selectedBitmap, srcBitmap->second, tag + "_source");
                dumpFramebufferPpm(tag);
                spdlog::info("{} splash probe dstDc=0x{:08x} framebuffer srcDc=0x{:08x} "
                             "srcBitmap=0x{:08x} srcSize={}x{} dst={}x{} src={}x{} "
                             "srcOrigin={},{} rop=0x{:08x}",
                             name, a0, stackArg(5), srcDc->selectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, dstW, dstH,
                             srcW, srcH, srcX, srcY, rop);
            }
            if (!ok) {
                spdlog::info("{} framebuffer blit failed dst=0x{:08x} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, stackArg(5), srcDc->selectedBitmap, srcBitmap->second.bits,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcBitmap->second.bpp,
                             dstW, dstH, srcW, srcH, srcX, srcY, rop);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
    } else if (name == "GetWindowLongW") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = getWindowLongValue(it->second, int32_t(a1));
        }
    } else if (name == "SetWindowLongW") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = setWindowLongValue(it->second, int32_t(a1), a2);
        }
    } else if (name == "GetParent") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = it->second.parent;
        }
    } else if (name == "GetWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else if (a1 == 5) {
            ret = 0;
            for (const auto& [hwnd, window] : windows_) {
                if (!window.destroyed && window.parent == a0) {
                    ret = hwnd;
                    break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 4) {
            const auto owner = windows_.find(it->second.parent);
            ret = owner != windows_.end() && !owner->second.destroyed ? it->second.parent : 0;
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 2 || a1 == 3) {
            std::vector<uint32_t> siblings;
            for (const auto& [hwnd, window] : windows_) {
                if (!window.destroyed && window.parent == it->second.parent) siblings.push_back(hwnd);
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
                if (!window.destroyed && window.parent == it->second.parent) {
                    ret = hwnd;
                    if (a1 == 0) break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (name == "MoveWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool sizeChanged = it->second.width != int32_t(a3) ||
                                     it->second.height != int32_t(stackArg(4));
            it->second.x = int32_t(a1);
            it->second.y = int32_t(a2);
            it->second.width = int32_t(a3);
            it->second.height = int32_t(stackArg(4));
#if defined(_WIN32)
            if (it->second.hostHwnd) {
                HWND hwnd = reinterpret_cast<HWND>(it->second.hostHwnd);
                auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                const int hostWidth = presenter ? hostPresenterDisplayWidth(*presenter) : std::max(1, it->second.width);
                const int hostHeight = presenter ? hostPresenterDisplayHeight(*presenter) : std::max(1, it->second.height);
                SetWindowPos(hwnd, nullptr, it->second.x, it->second.y, hostWidth, hostHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                if (stackArg(5)) InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                ensureHostWindow(a0, it->second);
            }
#endif
            if (sizeChanged) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0005; // WM_SIZE
                message.lParam = (uint32_t(uint16_t(it->second.height)) << 16) | uint16_t(it->second.width);
                message.time = uint32_t(++tick_ * 16);
                guestMessages_.push_back(message);
            }
            lastError_ = 0;
            ret = 1;
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
            spdlog::info("SetWindowPos guest=0x{:08x} insertAfter=0x{:08x} x={} y={} cx={} cy={} flags=0x{:08x} oldRect={},{} {}x{} oldVisible={}",
                         a0, a1, int32_t(a2), int32_t(a3), newWidth, newHeight, flags,
                         it->second.x, it->second.y, it->second.width, it->second.height,
                         oldVisible ? 1 : 0);
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
#if defined(_WIN32)
            if (it->second.hostHwnd) {
                HWND hwnd = reinterpret_cast<HWND>(it->second.hostHwnd);
                if (!(flags & 0x0001u) || !(flags & 0x0002u)) {
                    auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                    const int hostWidth = presenter ? hostPresenterDisplayWidth(*presenter)
                                                    : std::max(1, it->second.width);
                    const int hostHeight = presenter ? hostPresenterDisplayHeight(*presenter)
                                                     : std::max(1, it->second.height);
                    SetWindowPos(hwnd, nullptr, it->second.x, it->second.y,
                                 hostWidth, hostHeight,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                ShowWindow(hwnd, it->second.visible ? SW_SHOWNORMAL : SW_HIDE);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else {
                ensureHostWindow(a0, it->second);
            }
#endif
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
    } else if (name == "SetWindowRgn") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
#if defined(_WIN32)
            bool hostOk = true;
            if (it->second.hostHwnd) {
                HRGN region = nullptr;
                auto regionHandle = guestHandles_.find(a1);
                if (a1) {
                    if (regionHandle == guestHandles_.end() ||
                        regionHandle->second.kind != GuestHandle::Kind::HostRegion) {
                        lastError_ = 6;
                        ret = 0;
                        return true;
                    }
                    region = reinterpret_cast<HRGN>(regionHandle->second.hostValue);
                }
                hostOk = SetWindowRgn(reinterpret_cast<HWND>(it->second.hostHwnd), region, a2 != 0) != 0;
                if (hostOk && regionHandle != guestHandles_.end()) {
                    regionHandle->second.hostValue = 0;
                    guestHandles_.erase(regionHandle);
                }
            }
            ret = hostOk ? 1 : 0;
            lastError_ = ret ? 0 : GetLastError();
#else
            ret = 1;
            lastError_ = 0;
#endif
        }
    } else if (name == "GetWindowRgn") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
#if defined(_WIN32)
            auto region = guestHandles_.find(a1);
            if (it->second.hostHwnd && region != guestHandles_.end() &&
                region->second.kind == GuestHandle::Kind::HostRegion && region->second.hostValue) {
                ret = uint32_t(GetWindowRgn(reinterpret_cast<HWND>(it->second.hostHwnd),
                                            reinterpret_cast<HRGN>(region->second.hostValue)));
                lastError_ = ret ? 0 : GetLastError();
            } else {
                ret = 0;
                lastError_ = 0;
            }
#else
            ret = 0;
            lastError_ = 0;
#endif
        }
    } else if (name == "DestroyWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            for (auto timer = timers_.begin(); timer != timers_.end();) {
                if (timer->second.hwnd == a0) timer = timers_.erase(timer);
                else ++timer;
            }
            if (focusedWindow_ == a0) focusedWindow_ = 0;
            if (capturedWindow_ == a0) capturedWindow_ = 0;
            if (hostPointerCaptureWindow_ == a0) hostPointerCaptureWindow_ = 0;
            it->second.visible = false;
            it->second.destroyed = true;
            GuestMessage destroy{};
            destroy.hwnd = a0;
            destroy.message = 0x0002; // WM_DESTROY
            destroy.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(destroy);
            GuestMessage ncDestroy{};
            ncDestroy.hwnd = a0;
            ncDestroy.message = 0x0082; // WM_NCDESTROY
            ncDestroy.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(ncDestroy);
            destroyHostWindow(it->second);
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
            spdlog::info("ShowWindow guest=0x{:08x} cmd={} oldVisible={} newVisible={}",
                         a0, int32_t(a1), wasVisible ? 1 : 0, it->second.visible ? 1 : 0);
            ensureHostWindow(a0, it->second);
#if defined(_WIN32)
            if (it->second.hostHwnd) {
                ShowWindow(reinterpret_cast<HWND>(it->second.hostHwnd), it->second.visible ? SW_SHOWNORMAL : SW_HIDE);
                InvalidateRect(reinterpret_cast<HWND>(it->second.hostHwnd), nullptr, FALSE);
            }
#endif
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
            GuestMessage erase{};
            erase.hwnd = a0;
            erase.message = 0x0014; // WM_ERASEBKGND
            erase.wParam = makeGuestDc(a0);
            erase.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(erase);
            GuestMessage message{};
            message.hwnd = a0;
            message.message = 0x000f; // WM_PAINT
            message.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(message);
            ensureHostWindow(a0, it->second);
            invalidateHostWindows();
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "DefWindowProcW") {
        ret = (a1 == 0x0081) ? 1 : 0; // WM_NCCREATE defaults to TRUE.
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
        enqueueDueTimers();
        GuestMessage message{};
        bool haveMessage = false;
        auto takeMessage = [&]() {
            if (guestMessages_.empty()) return false;
            message = guestMessages_.front();
            if (!peek || (removeFlags & 1)) guestMessages_.pop_front();
            return true;
        };
        haveMessage = takeMessage();
        if (!haveMessage && !peek && !quitPosted_ && !timers_.empty()) {
            const uint64_t now = hostTickMilliseconds();
            auto next = std::min_element(timers_.begin(), timers_.end(),
                                         [](const auto& left, const auto& right) {
                                             return left.second.nextDueMs < right.second.nextDueMs;
                                         });
            if (next != timers_.end()) {
                next->second.nextDueMs = now;
                enqueueDueTimers();
                haveMessage = takeMessage();
            }
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
    } else if (name == "SetForegroundWindow") {
        if (windows_.count(a0)) {
            guestMessages_.push_back({a0, 0x0006, 1, 0, uint32_t(++tick_ * 16), 0, 0});
            guestMessages_.push_back({a0, 0x0007, 0, 0, uint32_t(++tick_ * 16), 0, 0});
            focusedWindow_ = a0;
            lastError_ = 0;
            ret = 1;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (name == "GetActiveWindow") {
        ret = firstWindow();
        lastError_ = ret ? 0 : 1400;
    } else if (name == "IsWindowEnabled") {
        ret = windows_.count(a0) ? 1 : 0;
        lastError_ = ret ? 0 : 1400;
    } else if (name == "MessageBoxW") {
        spdlog::info("MessageBoxW caption=\"{}\" text=\"{}\" flags=0x{:08x}",
                     readUtf16(a2), readUtf16(a1), a3);
        ret = 1;
        lastError_ = 0;
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
            spdlog::info("{} miss type=0x{:08x} name=0x{:08x}", name, a2, a1);
            lastError_ = 1814;
            ret = 0;
        } else {
            const size_t index = size_t(resource - mainResources_.data());
            ret = makeGuestHandle({GuestHandle::Kind::GuestResource, index + 1, 0});
            spdlog::info("{} hit type=0x{:08x} name=0x{:08x} size={}", name, a2, a1, resource->data.size());
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
            spdlog::info("LoadStringW miss id={}", a1);
            lastError_ = 1814;
            ret = 0;
        } else {
            ret = a2 && a3 ? writeUtf16(a2, value, a3) : uint32_t(value.size());
            spdlog::info("LoadStringW hit id={} ret={} value=\"{}\"", a1, ret, value);
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
        ret = handleLoadImageApi(name, a0, a1, a2, a3, stackArg(4), stackArg(5));
    } else if (name == "LoadMenuW") {
        ret = loadMenuResourceHandle(a1);
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
    } else if (isRegistryApiName(name)) {
        return dispatchRegistryApi(name, args, ret);
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
    } else if (name == "waveInOpen" || name == "waveInReset" || name == "waveInGetID" ||
               name == "waveInAddBuffer" || name == "waveInUnprepareHeader" ||
               name == "waveInMessage" || name == "waveOutGetNumDevs" ||
               name == "waveOutOpen" || name == "waveOutSetVolume" ||
               name == "waveOutClose" || name == "waveOutPrepareHeader" ||
               name == "waveOutUnprepareHeader" || name == "waveOutWrite" ||
               name == "waveOutReset" ||
               name == "mixerGetControlDetails") {
        return dispatchWinmm(name, args, ret);
    } else {
        return false;
    }

    return true;
}

bool SyntheticDllRuntime::dispatchWinmm(const std::string& name,
                                        const GuestCallArgs& args,
                                        uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    if (name == "waveInOpen") {
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
    } else if (name == "waveOutGetNumDevs") {
#if defined(_WIN32)
        auto& winmm = winmmBridge();
        ret = winmm.waveOutGetNumDevs ? winmm.waveOutGetNumDevs() : 0;
#else
        ret = 0;
#endif
    } else if (name == "waveOutOpen") {
#if defined(_WIN32)
        auto& winmm = winmmBridge();
        if (!winmm.waveOutOpen || !a0 || !a2) {
            ret = MMSYSERR_INVALPARAM;
        } else {
            WAVEFORMATEX format{};
            if (uc_mem_read(uc_, a2, &format, sizeof(format)) != UC_ERR_OK) {
                ret = MMSYSERR_INVALPARAM;
            } else {
                HWAVEOUT host{};
                const DWORD_PTR instance = DWORD_PTR(stackArg(4));
                const DWORD flags = stackArg(5);
                const DWORD callbackFlags = flags & CALLBACK_TYPEMASK;
                const DWORD hostFlags = callbackFlags == CALLBACK_NULL ? flags : ((flags & ~CALLBACK_TYPEMASK) | CALLBACK_NULL);
                ret = winmm.waveOutOpen(&host, a1, &format, 0, instance, hostFlags);
                if (ret == MMSYSERR_NOERROR) {
                    const uint32_t guest = makeGuestHandle({
                        GuestHandle::Kind::HostWaveOut,
                        reinterpret_cast<uintptr_t>(host),
                        0,
                    });
                    waveOutStates_[guest] = {a3, uint32_t(instance), flags};
                    writeU32(a0, guest);
                }
            }
        }
#else
        ret = 2;
#endif
    } else if (name == "waveOutClose") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        if (handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutClose) {
            ret = winmm.waveOutClose(reinterpret_cast<HWAVEOUT>(handle->hostValue));
            if (ret == MMSYSERR_NOERROR) {
                handle->hostValue = 0;
                waveOutStates_.erase(a0);
            }
        } else {
            ret = MMSYSERR_INVALHANDLE;
        }
#else
        ret = 2;
#endif
    } else if (name == "waveOutSetVolume") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        ret = handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutSetVolume
            ? winmm.waveOutSetVolume(reinterpret_cast<HWAVEOUT>(handle->hostValue), a1)
            : MMSYSERR_INVALHANDLE;
#else
        ret = 2;
#endif
    } else if (name == "waveOutReset") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        ret = handle && handle->kind == GuestHandle::Kind::HostWaveOut && handle->hostValue && winmm.waveOutReset
            ? winmm.waveOutReset(reinterpret_cast<HWAVEOUT>(handle->hostValue))
            : MMSYSERR_INVALHANDLE;
#else
        ret = 2;
#endif
    } else if (name == "waveOutPrepareHeader" || name == "waveOutUnprepareHeader" || name == "waveOutWrite") {
#if defined(_WIN32)
        auto* handle = lookupGuestHandle(a0);
        auto& winmm = winmmBridge();
        if (!handle || handle->kind != GuestHandle::Kind::HostWaveOut || !handle->hostValue || !a1) {
            ret = MMSYSERR_INVALHANDLE;
        } else if (a2 < 32) {
            ret = MMSYSERR_INVALPARAM;
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
            if (!guestData || !guestLength || guestLength > 0x400000) {
                ret = MMSYSERR_INVALPARAM;
            } else {
                auto& stored = hostWaveBuffers_[a1];
                if (stored.data.size() != guestLength) stored.data.assign(guestLength, 0);
                uc_mem_read(uc_, guestData, stored.data.data(), stored.data.size());
                auto* header = reinterpret_cast<WAVEHDR*>(stored.header.data());
                *header = {};
                header->lpData = reinterpret_cast<LPSTR>(stored.data.data());
                header->dwBufferLength = guestLength;
                header->dwBytesRecorded = guestBytesRecorded;
                header->dwUser = guestUser;
                header->dwFlags = guestFlags;
                header->dwLoops = guestLoops;
                if (name == "waveOutPrepareHeader") {
                    ret = winmm.waveOutPrepareHeader
                        ? winmm.waveOutPrepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header))
                        : MMSYSERR_ERROR;
                } else if (name == "waveOutUnprepareHeader") {
                    ret = winmm.waveOutUnprepareHeader
                        ? winmm.waveOutUnprepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header))
                        : MMSYSERR_ERROR;
                } else {
                    if (winmm.waveOutPrepareHeader && !(header->dwFlags & WHDR_PREPARED)) {
                        winmm.waveOutPrepareHeader(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header));
                    }
                    ret = winmm.waveOutWrite
                        ? winmm.waveOutWrite(reinterpret_cast<HWAVEOUT>(handle->hostValue), header, sizeof(*header))
                        : MMSYSERR_ERROR;
                    if (ret == MMSYSERR_NOERROR) {
                        header->dwFlags |= WHDR_DONE;
                        auto state = waveOutStates_.find(a0);
                        if (state != waveOutStates_.end()) {
                            const uint32_t callbackType = state->second.flags & CALLBACK_TYPEMASK;
                            const uint32_t eventHandle = callbackType == CALLBACK_EVENT
                                ? state->second.callback
                                : guestUser;
                            auto* event = lookupGuestHandle(eventHandle);
                            if (event && event->kind == GuestHandle::Kind::HostEvent && event->hostValue) {
                                SetEvent(reinterpret_cast<HANDLE>(event->hostValue));
                                spdlog::info("waveOutWrite completed guest event=0x{:08x} callback=0x{:08x} flags=0x{:08x}",
                                             eventHandle, state->second.callback, state->second.flags);
                            }
                        }
                    }
                }
                writeU32(a1 + 16, uint32_t(header->dwFlags));
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
    } else if (name == "waveInGetID") {
        ret = handleWaveInGetID(a0, a1);
    } else if (name == "waveInAddBuffer" || name == "waveInUnprepareHeader") {
        ret = handleWaveInBuffer(name, a0, a1);
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

bool SyntheticDllRuntime::dispatchCommctrl(const std::string& name,
                                           const GuestCallArgs& args,
                                           uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;

    auto commandBarHeight = [] { return 26; };
    auto parentSize = [&](uint32_t hwnd, int32_t fallbackWidth, int32_t fallbackHeight) {
        auto parent = windows_.find(hwnd);
        if (parent == windows_.end()) return std::pair<int32_t, int32_t>{fallbackWidth, fallbackHeight};
        return std::pair<int32_t, int32_t>{std::max<int32_t>(1, parent->second.width),
                                           std::max<int32_t>(1, parent->second.height)};
    };
    auto topLevelWindow = [&](uint32_t hwnd) -> GuestWindow* {
        uint32_t current = hwnd;
        for (;;) {
            auto it = windows_.find(current);
            if (it == windows_.end()) return nullptr;
            if (!it->second.parent) return &it->second;
            current = it->second.parent;
        }
    };
    auto drawCommandBarMenu = [&](GuestWindow& commandBar) -> bool {
#if defined(_WIN32)
        auto* menuHandle = lookupGuestHandle(commandBar.menu);
        auto* top = topLevelWindow(commandBar.hwnd);
        if (!menuHandle || menuHandle->kind != GuestHandle::Kind::HostMenu || !menuHandle->hostValue ||
            !top || !top->hostHwnd) {
            return true;
        }
        HWND hostHwnd = reinterpret_cast<HWND>(top->hostHwnd);
        if (!IsWindow(hostHwnd)) return true;
        SetMenu(hostHwnd, reinterpret_cast<HMENU>(menuHandle->hostValue));
        DrawMenuBar(hostHwnd);
#endif
        return true;
    };

    if (name == "InitCommonControls") {
        ret = 0;
    } else if (name == "InitCommonControlsEx" ||
               name == "InitCapEdit" ||
               name == "InitDateClasses" ||
               name == "InitProgressClass" ||
               name == "InitReBarClass" ||
               name == "InitSBEdit" ||
               name == "InitStatusClass" ||
               name == "InitTTButton" ||
               name == "InitTTStatic" ||
               name == "InitToolTipsClass" ||
               name == "InitToolbarClass" ||
               name == "InitTrackBar" ||
               name == "InitUpDownClass" ||
               name == "?Header_Init@@YAHPAUHINSTANCE__@@@Z" ||
               name == "?ListView_Init@@YAHPAUHINSTANCE__@@@Z" ||
               name == "?TV_Init@@YAHPAUHINSTANCE__@@@Z" ||
               name == "Tab_Init") {
        ret = 1;
    } else if (name == "CommandBar_Create") {
        const auto [width, ignoredHeight] = parentSize(a1, framebufferWidth_ > 0 ? framebufferWidth_ : 800, 480);
        (void)ignoredHeight;
        ret = makeGuestWindow("CommandBar", {}, 0x50000000u, 0, a1, uint32_t(a2), a0, 0,
                              0, 0, width, commandBarHeight(), true);
        lastError_ = ret ? 0 : 8;
    } else if (name == "CommandBar_Show") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            it->second.visible = a1 != 0;
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "CommandBar_Height") {
        ret = windows_.count(a0) ? uint32_t(commandBarHeight()) : 0;
        lastError_ = ret ? 0 : 1400;
    } else if (name == "CommandBar_InsertComboBox" || name == "CommandBar_InsertControl") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const int32_t width = name == "CommandBar_InsertComboBox" ? std::max<int32_t>(1, int32_t(a2)) : 80;
            const uint32_t style = name == "CommandBar_InsertComboBox" ? a3 : 0x50000000u;
            const uint32_t id = name == "CommandBar_InsertComboBox" ? stackArg(4) : a2;
            ret = makeGuestWindow(name == "CommandBar_InsertComboBox" ? "ComboBox" : "CommandBarControl",
                                  {}, style | 0x50000000u, 0, a0, id, a1, 0,
                                  0, 0, width, 22, it->second.visible);
            lastError_ = ret ? 0 : 8;
        }
    } else if (name == "CommandBar_AddBitmap") {
        ret = findResource(2, a2) ? 0 : 0xffffffffu;
        lastError_ = ret == 0xffffffffu ? 1814 : 0;
    } else if (name == "CommandBar_InsertMenubar" || name == "CommandBar_InsertMenubarEx") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const uint32_t nameArg = name == "CommandBar_InsertMenubar" ? a2 : a2;
            const uint32_t menu = loadMenuResourceHandle(nameArg);
            if (!menu) {
                ret = 0;
            } else {
                it->second.menu = menu;
                drawCommandBarMenu(it->second);
                lastError_ = 0;
                ret = 1;
            }
        }
    } else if (name == "CommandBar_GetMenu") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = it->second.menu ? 0 : 1401;
            ret = it->second.menu;
        }
    } else if (name == "CommandBar_AddAdornments" ||
               name == "CommandBar_DrawMenuBar" ||
               name == "CommandBar_AlignAdornments") {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = name == "CommandBar_AlignAdornments" ? 0 : 0;
        } else {
            if (name == "CommandBar_DrawMenuBar") drawCommandBarMenu(it->second);
            lastError_ = 0;
            ret = name == "CommandBar_AlignAdornments" ? 0 : 1;
        }
    } else if (name == "IsCommandBarMessage") {
        ret = 0;
        lastError_ = 0;
    } else if (name == "CreateStatusWindowW") {
        const auto [parentWidth, parentHeight] = parentSize(a2, framebufferWidth_ > 0 ? framebufferWidth_ : 800,
                                                           framebufferHeight_ > 0 ? framebufferHeight_ : 480);
        const int32_t height = 22;
        ret = makeGuestWindow("msctls_statusbar32", readUtf16(a1), a0 | 0x50000000u, 0, a2, a3, 0, 0,
                              0, std::max<int32_t>(0, parentHeight - height), parentWidth, height, true);
        lastError_ = ret ? 0 : 8;
    } else if (name == "CreateToolbarEx" ||
               name == "?CreateToolbar@@YAPAUHWND__@@PAU1@KIHPAUHINSTANCE__@@IPBU_TBBUTTON@@H@Z" ||
               name == "CreateUpDownControl") {
        const uint32_t parent = name == "CreateUpDownControl" ? stackArg(9) : a0;
        const auto [parentWidth, ignoredHeight] = parentSize(parent, framebufferWidth_ > 0 ? framebufferWidth_ : 800, 480);
        (void)ignoredHeight;
        ret = makeGuestWindow(name == "CreateUpDownControl" ? "msctls_updown32" : "ToolbarWindow32",
                              {}, 0x50000000u, 0, parent, a2, a3, 0,
                              0, 0, parentWidth, commandBarHeight(), true);
        lastError_ = ret ? 0 : 8;
    } else if (name == "DrawStatusTextW") {
        GuestDc* dc = lookupGuestDc(a0);
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        if (!dc || !readGuestRect(a1, left, top, right, bottom)) {
            lastError_ = 87;
            ret = 0;
        } else {
            fillFramebufferRect(*dc, left, top, right, bottom, colorRefToPixel(0x00c0c0c0));
            lastError_ = 0;
            ret = 0;
        }
    } else if (name == "InvertRect") {
        GuestDc* dc = lookupGuestDc(a0);
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        if (!dc || !readGuestRect(a1, left, top, right, bottom)) {
            lastError_ = 87;
            ret = 0;
        } else {
            fillFramebufferRect(*dc, left, top, right, bottom, colorRefToPixel(0x00000000));
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "CreatePropertySheetPageW") {
        ret = makeGuestHandle({GuestHandle::Kind::GuestPropertySheetPage, 0, 0});
        lastError_ = ret ? 0 : 8;
    } else if (name == "DestroyPropertySheetPage") {
        auto it = guestHandles_.find(a0);
        if (it == guestHandles_.end() || it->second.kind != GuestHandle::Kind::GuestPropertySheetPage) {
            lastError_ = 6;
            ret = 0;
        } else {
            guestHandles_.erase(it);
            lastError_ = 0;
            ret = 1;
        }
    } else if (name == "PropertySheetW") {
        lastError_ = 120;
        ret = 0xffffffffu;
    } else if (name == "ListView_SetItemSpacing") {
        lastError_ = windows_.count(a0) ? 0 : 1400;
        ret = 0;
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
    auto comHandleFromThis = [&](uint32_t thisPtr) -> std::pair<uint32_t, GuestHandle*> {
        if (!thisPtr) return {0, nullptr};
        const uint32_t guestHandle = readU32(thisPtr + 4);
        GuestHandle* handle = lookupGuestHandle(guestHandle);
        if (!handle || handle->kind != GuestHandle::Kind::HostComInterface || !handle->hostValue) {
            return {guestHandle, nullptr};
        }
        return {guestHandle, handle};
    };
    if (name == "__ComQueryInterface") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        (void)guestHandle;
        GUID iid{};
        IUnknown* out = nullptr;
        HRESULT hr = E_POINTER;
        if (a2) writeU32(a2, 0);
        if (handle && readGuid(a1, iid) && a2) {
            hr = reinterpret_cast<IUnknown*>(handle->hostValue)->QueryInterface(iid, reinterpret_cast<void**>(&out));
            if (SUCCEEDED(hr) && out) writeU32(a2, makeGuestComProxy(reinterpret_cast<uintptr_t>(out)));
        }
        ret = uint32_t(hr);
    } else if (name == "__ComAddRef") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        (void)guestHandle;
        ret = handle ? reinterpret_cast<IUnknown*>(handle->hostValue)->AddRef() : 0;
    } else if (name == "__ComRelease") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        if (!handle) {
            ret = 0;
        } else {
            ret = reinterpret_cast<IUnknown*>(handle->hostValue)->Release();
            if (!ret) guestHandles_.erase(guestHandle);
        }
    } else if (name == "CoInitializeEx") {
        ret = uint32_t(::CoInitializeEx(nullptr, a1));
    } else if (name == "CoUninitialize") {
        ::CoUninitialize();
        ret = 0;
    } else if (name == "CoTaskMemAlloc") {
        ret = allocate(a0, false);
    } else if (name == "CoTaskMemRealloc") {
        if (!a1) {
            releaseAllocation(a0);
            ret = 0;
        } else {
            const uint32_t oldSize = allocationSize(a0);
            ret = allocate(a1, false);
            if (ret && a0 && oldSize) {
                copyGuest(ret, a0, std::min(oldSize, a1));
                releaseAllocation(a0);
            }
        }
    } else if (name == "CoTaskMemFree") {
        releaseAllocation(a0);
        ret = 0;
    } else if (name == "CoTaskMemSize") {
        ret = allocationSize(a0);
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
    } else if (name == "CoCreateInstance") {
        const uint32_t outPtr = stackArg(4);
        if (outPtr) writeU32(outPtr, 0);
        GUID clsid{};
        GUID iid{};
        IUnknown* out = nullptr;
        HRESULT hr = E_INVALIDARG;
        if (!outPtr) {
            hr = E_POINTER;
        } else if (a1) {
            hr = CLASS_E_NOAGGREGATION;
        } else if (readGuid(a0, clsid) && readGuid(a3, iid)) {
            const DWORD context = a2 ? a2 : CLSCTX_INPROC_SERVER;
            hr = ::CoCreateInstance(clsid, nullptr, context, iid, reinterpret_cast<void**>(&out));
            if (SUCCEEDED(hr) && out) writeU32(outPtr, makeGuestComProxy(reinterpret_cast<uintptr_t>(out)));
        }
        ret = uint32_t(hr);
    } else if (name == "OleCreate") {
        if (stackArg(6)) writeU32(stackArg(6), 0);
        ret = 0x80004001u;
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
    else if (name == "CoTaskMemFree") {
        releaseAllocation(a0);
        ret = 0;
    }
    else if (name == "CoUninitialize") ret = 0;
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
        if (a0 >= 4) releaseAllocation(a0 - 4);
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
        return wndProc;
    };
    auto finalizeDestroyedWindow = [&](uint32_t hwnd) {
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return;
        for (auto timer = timers_.begin(); timer != timers_.end();) {
            if (timer->second.hwnd == hwnd) timer = timers_.erase(timer);
            else ++timer;
        }
        if (focusedWindow_ == hwnd) focusedWindow_ = 0;
        if (capturedWindow_ == hwnd) capturedWindow_ = 0;
        if (hostPointerCaptureWindow_ == hwnd) hostPointerCaptureWindow_ = 0;
        it->second.visible = false;
        it->second.destroyed = true;
        destroyHostWindow(it->second);
    };
    auto dispatchQueuedPaintForBlockingApi = [&](PendingBlockingApi& pending, const char* reason) {
        if (pending.paintDispatches >= 16) return false;
        auto paint = std::find_if(guestMessages_.begin(), guestMessages_.end(),
                                  [&](const GuestMessage& message) {
                                      if (message.message != 0x0014 && message.message != 0x000f) return false;
                                      auto window = windows_.find(message.hwnd);
                                      return window != windows_.end() && !window->second.destroyed && window->second.wndProc;
                                  });
        if (paint == guestMessages_.end()) return false;
        const GuestMessage message = *paint;
        guestMessages_.erase(paint);
        auto window = windows_.find(message.hwnd);
        uint32_t wndProc = translatedWndProc(window->second.wndProc, pending.name.c_str());
        ++pending.paintDispatches;
        spdlog::info("{} cooperative dispatch {} hwnd=0x{:08x} msg=0x{:08x} wndproc=0x{:08x} count={}",
                     pending.name, reason, message.hwnd, message.message, wndProc, pending.paintDispatches);
        setReg(UC_MIPS_REG_A0, message.hwnd);
        setReg(UC_MIPS_REG_A1, message.message);
        setReg(UC_MIPS_REG_A2, message.wParam);
        setReg(UC_MIPS_REG_A3, message.lParam);
        setReg(UC_MIPS_REG_RA, blockingApiContinuationStub_);
        setReg(UC_MIPS_REG_PC, wndProc);
        return true;
    };

    if (mutableEntry.moduleName == "coredll.dll" && name == "__DestroyWindowContinue") {
        if (pendingDestroyWindows_.empty()) {
            spdlog::warn("DestroyWindow continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_PC, ra);
            return;
        }
        auto& pending = pendingDestroyWindows_.back();
        if (pending.stage == 0) {
            pending.stage = 1;
            uint32_t wndProc = pending.wndProc;
            auto window = windows_.find(pending.hwnd);
            if (window != windows_.end() && window->second.wndProc) wndProc = window->second.wndProc;
            wndProc = translatedWndProc(wndProc, "DestroyWindow");
            spdlog::info("DestroyWindow synchronous WM_NCDESTROY hwnd=0x{:08x} wndproc=0x{:08x}",
                         pending.hwnd, wndProc);
            setReg(UC_MIPS_REG_A0, pending.hwnd);
            setReg(UC_MIPS_REG_A1, 0x0082); // WM_NCDESTROY
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, 0);
            setReg(UC_MIPS_REG_RA, destroyWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t hwnd = pending.hwnd;
        const uint32_t originalRa = pending.originalRa;
        pendingDestroyWindows_.pop_back();
        finalizeDestroyedWindow(hwnd);
        lastError_ = 0;
        setReg(UC_MIPS_REG_V0, 1);
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_PC, originalRa);
        spdlog::info("DestroyWindow synchronous destroy complete hwnd=0x{:08x} return=0x{:08x}", hwnd, originalRa);
        pumpHostMessages();
        return;
    }

    if (mutableEntry.moduleName == "coredll.dll" && name == "__CreateWindowContinue") {
        if (pendingCreateWindows_.empty()) {
            spdlog::warn("CreateWindowExW continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 0);
            setReg(UC_MIPS_REG_PC, ra);
            return;
        }
        auto& pending = pendingCreateWindows_.back();
        const uint32_t wndProcResult = reg(UC_MIPS_REG_V0);
        if (pending.stage == 0) {
            if (wndProcResult == 0) {
                const uint32_t originalRa = pending.originalRa;
                const uint32_t hwnd = pending.hwnd;
                pendingCreateWindows_.pop_back();
                finalizeDestroyedWindow(hwnd);
                lastError_ = 0;
                setReg(UC_MIPS_REG_V0, 0);
                setReg(UC_MIPS_REG_RA, originalRa);
                setReg(UC_MIPS_REG_PC, originalRa);
                spdlog::info("CreateWindowExW synchronous WM_NCCREATE rejected hwnd=0x{:08x}", hwnd);
                pumpHostMessages();
                return;
            }
            pending.stage = 1;
            auto window = windows_.find(pending.hwnd);
            uint32_t wndProc = pending.wndProc;
            if (window != windows_.end() && window->second.wndProc) wndProc = window->second.wndProc;
            wndProc = translatedWndProc(wndProc, "CreateWindowExW");
            spdlog::info("CreateWindowExW synchronous WM_CREATE hwnd=0x{:08x} wndproc=0x{:08x}",
                         pending.hwnd, wndProc);
            setReg(UC_MIPS_REG_A0, pending.hwnd);
            setReg(UC_MIPS_REG_A1, 0x0001); // WM_CREATE
            setReg(UC_MIPS_REG_A2, 0);
            setReg(UC_MIPS_REG_A3, pending.createStruct);
            setReg(UC_MIPS_REG_RA, createWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t hwnd = pending.hwnd;
        const uint32_t originalRa = pending.originalRa;
        pendingCreateWindows_.pop_back();
        if (wndProcResult == 0xffffffffu) {
            finalizeDestroyedWindow(hwnd);
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, 0);
            spdlog::info("CreateWindowExW synchronous WM_CREATE failed hwnd=0x{:08x}", hwnd);
        } else {
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, hwnd);
            spdlog::info("CreateWindowExW synchronous create complete hwnd=0x{:08x}", hwnd);
        }
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_PC, originalRa);
        pumpHostMessages();
        return;
    }

    if (mutableEntry.moduleName == "coredll.dll" && name == "__BlockingApiContinue") {
        if (pendingBlockingApis_.empty()) {
            spdlog::warn("blocking API continuation reached with no pending call");
            setReg(UC_MIPS_REG_V0, 0);
            setReg(UC_MIPS_REG_PC, ra);
            return;
        }
        if (dispatchQueuedPaintForBlockingApi(pendingBlockingApis_.back(), "while blocked")) {
            return;
        }
        PendingBlockingApi pending = pendingBlockingApis_.back();
        pendingBlockingApis_.pop_back();
        uint32_t ret = 0;
        if (!dispatchHostWin32(pending.name, pending.args, ret)) {
            spdlog::warn("blocking API continuation could not resume {}", pending.name);
            lastError_ = 120;
            ret = 0;
        }
        setReg(UC_MIPS_REG_V0, ret);
        setReg(UC_MIPS_REG_RA, pending.args.ra);
        setReg(UC_MIPS_REG_PC, pending.args.ra);
        spdlog::info("{} resumed after cooperative paint -> 0x{:08x}", pending.name, ret);
        pumpHostMessages();
        return;
    }

    if (mutableEntry.moduleName == "coredll.dll" && name == "CreateWindowExW") {
        uint32_t ret = 0;
        if (!dispatchHostWin32(name, args, ret)) {
            lastError_ = 120;
            setReg(UC_MIPS_REG_V0, 0);
            pumpHostMessages();
            return;
        }
        auto window = windows_.find(ret);
        if (!ret || window == windows_.end() || !window->second.wndProc ||
            !window->second.createStruct || !createWindowContinuationStub_) {
            if (mutableEntry.calls <= 128) {
                spdlog::info("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, ret);
            }
            setReg(UC_MIPS_REG_V0, ret);
            pumpHostMessages();
            return;
        }
        const uint32_t wndProc = translatedWndProc(window->second.wndProc, "CreateWindowExW");
        pendingCreateWindows_.push_back(PendingCreateWindow{
            ret, wndProc, ra, window->second.createStruct, 0,
        });
        spdlog::info("CreateWindowExW synchronous WM_NCCREATE hwnd=0x{:08x} wndproc=0x{:08x} return=0x{:08x}",
                     ret, wndProc, ra);
        setReg(UC_MIPS_REG_A0, ret);
        setReg(UC_MIPS_REG_A1, 0x0081); // WM_NCCREATE
        setReg(UC_MIPS_REG_A2, 0);
        setReg(UC_MIPS_REG_A3, window->second.createStruct);
        setReg(UC_MIPS_REG_RA, createWindowContinuationStub_);
        setReg(UC_MIPS_REG_PC, wndProc);
        return;
    }

    if (mutableEntry.moduleName == "coredll.dll" && name == "DestroyWindow") {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            setReg(UC_MIPS_REG_V0, 0);
            pumpHostMessages();
            return;
        }
        const auto oldSize = guestMessages_.size();
        std::erase_if(guestMessages_, [&](const GuestMessage& message) { return message.hwnd == a0; });
        if (oldSize != guestMessages_.size()) {
            spdlog::info("DestroyWindow discarded {} pending posted messages for hwnd=0x{:08x}",
                         oldSize - guestMessages_.size(), a0);
        }
        it->second.visible = false;
        const uint32_t wndProc = translatedWndProc(it->second.wndProc, "DestroyWindow");
        if (!wndProc || !destroyWindowContinuationStub_) {
            finalizeDestroyedWindow(a0);
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, 1);
            pumpHostMessages();
            return;
        }
        pendingDestroyWindows_.push_back(PendingDestroyWindow{a0, wndProc, ra, 0});
        spdlog::info("DestroyWindow synchronous WM_DESTROY hwnd=0x{:08x} wndproc=0x{:08x} return=0x{:08x}",
                     a0, wndProc, ra);
        setReg(UC_MIPS_REG_A0, a0);
        setReg(UC_MIPS_REG_A1, 0x0002); // WM_DESTROY
        setReg(UC_MIPS_REG_A2, 0);
        setReg(UC_MIPS_REG_A3, 0);
        setReg(UC_MIPS_REG_RA, destroyWindowContinuationStub_);
        setReg(UC_MIPS_REG_PC, wndProc);
        return;
    }

    if (mutableEntry.moduleName == "coredll.dll" &&
        (name == "Sleep" || name == "WaitForSingleObject") && blockingApiContinuationStub_) {
        pendingBlockingApis_.push_back(PendingBlockingApi{name, args});
        if (dispatchQueuedPaintForBlockingApi(pendingBlockingApis_.back(), "before block")) {
            return;
        }
        pendingBlockingApis_.pop_back();
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
            if (msg == 0x0113 && lParam) {
                wndProc = lParam;
                if (a0) uc_mem_read(uc_, a0 + 16, &lParam, sizeof(lParam));
            } else {
                auto it = windows_.find(hwnd);
                wndProc = it == windows_.end() ? 0 : it->second.wndProc;
            }
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
            pumpHostMessages();
            return;
        }
        wndProc = translatedWndProc(wndProc, name.c_str());
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
        pumpHostMessages();
        return;
    }
    if (mutableEntry.moduleName == "coredll.dll") {
        lastError_ = 120; // ERROR_CALL_NOT_IMPLEMENTED
        ret = 0;
        if (mutableEntry.calls <= 128) {
            spdlog::warn("synthetic coredll.dll!{} unsupported by translate layer -> 0", name);
        }
        setReg(UC_MIPS_REG_V0, ret);
        pumpHostMessages();
        return;
    }

    bool handled = false;
    if (sameModule(mutableEntry.moduleName, "winsock.dll") ||
        sameModule(mutableEntry.moduleName, "ws2.dll")) {
        handled = dispatchWinsock(name, args, ret);
    } else if (sameModule(mutableEntry.moduleName, "commctrl.dll")) {
        handled = dispatchCommctrl(name, args, ret);
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
    pumpHostMessages();
}
