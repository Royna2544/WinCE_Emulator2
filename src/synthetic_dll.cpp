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
constexpr uint32_t kWindowStyleChild = 0x40000000u; // WS_CHILD

bool tracePrivateUiMessage(uint32_t msg) {
    return msg == 0x057c9 || // route-search handoff payload
           msg == 0x057cc || // route/result transition
           msg == 0x057ed || // route completion/update post
           msg == 0x057f5;   // route traffic/status update
}

bool traceGuestWindowMessage(uint32_t msg) {
    return msg == 0x0007 || msg == 0x0008 ||
           msg == 0x0200 || msg == 0x0201 || msg == 0x0202 ||
           msg == 0x032f0 || tracePrivateUiMessage(msg);
}

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

std::string pathWithBackslashes(std::filesystem::path path) {
    std::string text = pathToUtf8(path.lexically_normal());
    std::replace(text.begin(), text.end(), '/', '\\');
    while (!text.empty() && text.front() == '\\') text.erase(text.begin());
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

DWORD hostPresenterWindowStyle() {
    return WS_OVERLAPPEDWINDOW;
}

DWORD hostPresenterWindowExStyle() {
    return 0;
}

RECT hostPresenterOuterRectForClient(const HostPresenterWindow& presenter) {
    RECT rect{0, 0, hostPresenterDisplayWidth(presenter), hostPresenterDisplayHeight(presenter)};
    AdjustWindowRectEx(&rect, hostPresenterWindowStyle(), FALSE, hostPresenterWindowExStyle());
    return rect;
}

int hostPresenterOuterWidth(const HostPresenterWindow& presenter) {
    const RECT rect = hostPresenterOuterRectForClient(presenter);
    return std::max(1L, rect.right - rect.left);
}

int hostPresenterOuterHeight(const HostPresenterWindow& presenter) {
    const RECT rect = hostPresenterOuterRectForClient(presenter);
    return std::max(1L, rect.bottom - rect.top);
}

void presentHostWindowNow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    GdiFlush();
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
            GdiFlush();
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if ((message == WM_MOUSEMOVE && (wParam & MK_LBUTTON)) ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) {
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
            } else {
                guestMessage = 0x0200; // WM_MOUSEMOVE
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
#if defined(NDEBUG)
    diagnosticDumpsEnabled_ = false;
#else
    diagnosticDumpsEnabled_ = envFlagEnabled("INAVI_EMU_DUMPS");
#endif
    uc_mem_map(uc_, heapBase_, heapLimit_ - heapBase_, UC_PROT_ALL);
    uc_mem_map(uc_, 0x00005000, 0x00001000, UC_PROT_ALL);
    initializeUserKData();
}

void SyntheticDllRuntime::initializeUserKData() {
    mainThreadTls_ = allocate(64 * sizeof(uint32_t), true);

    // Windows CE exposes KDataStruct at PUserKData. On non-ARM builds this is
    // 0x00005800; application code reads SH_CURTHREAD/SH_CURPROC directly.
    writeU32(0x00005800, mainThreadTls_);          // lpvTls
    writeU32(0x00005804, 0);                       // SH_WIN32 API set handle placeholder
    writeU32(0x00005808, mainThreadPseudoHandle_); // SH_CURTHREAD
    writeU32(0x0000580c, mainProcessPseudoHandle_);// SH_CURPROC
    writeU32(0x00005810, 0);                       // SH_KWIN32 placeholder
}

void SyntheticDllRuntime::updateCurrentThreadKData(uint32_t currentThreadValue, uint32_t tlsBase) {
    writeU32(0x00005800, tlsBase ? tlsBase : mainThreadTls_);
    writeU32(0x00005808, currentThreadValue ? currentThreadValue : mainThreadPseudoHandle_);
    writeU32(0x0000580c, mainProcessPseudoHandle_);
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

uint32_t SyntheticDllRuntime::threadExitStubAddress() const {
    return threadExitStub_;
}

void SyntheticDllRuntime::runHostMessageLoopUntilClosed(bool showHostWindows) {
#if defined(_WIN32)
    if (!hasHostWindows()) return;
    struct HookGuard {
        uc_engine* uc{};
        uc_hook hook{};
        ~HookGuard() {
            if (uc && hook) uc_hook_del(uc, hook);
        }
    } interactiveSliceHook{uc_};
    const uc_err hookErr = uc_hook_add(uc_, &interactiveSliceHook.hook, UC_HOOK_BLOCK,
                                       (void*)SyntheticDllRuntime::hookBasicBlock, this, 1, 0);
    if (hookErr != UC_ERR_OK) {
        spdlog::warn("interactive basic-block watchdog hook failed: {} ({})",
                     int(hookErr), uc_strerror(hookErr));
    }
    if (showHostWindows) {
        for (auto& [guestHwnd, window] : windows_) {
            (void)guestHwnd;
            HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
            if (!hwnd || !IsWindow(hwnd)) continue;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindowNow(hwnd);
        }
        for (uintptr_t hostHwnd : retainedHostWindows_) {
            HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
            if (!hwnd || !IsWindow(hwnd)) continue;
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindowNow(hwnd);
        }
    }
    spdlog::info("entering host GUI message loop mode={}; close the presenter window to exit",
                 showHostWindows ? "visible" : "headless");
    MSG message{};
    auto hasPendingUserInput = [&]() {
        return std::any_of(guestMessages_.begin(), guestMessages_.end(), [](const GuestMessage& message) {
            return message.message == 0x0007 || message.message == 0x0008 ||
                   (message.message >= 0x0200 && message.message <= 0x0202);
        });
    };
    auto recentlyQueuedUserInput = [&]() {
        return lastHostInputQueuedAt_ != std::chrono::steady_clock::time_point{} &&
               (std::chrono::steady_clock::now() - lastHostInputQueuedAt_) < std::chrono::milliseconds(3000);
    };
    auto hasPendingSynchronousMessage = [&]() {
        return std::any_of(guestMessages_.begin(), guestMessages_.end(), [](const GuestMessage& message) {
            return message.synchronousSender != 0;
        });
    };
    auto resumeGuestSlice = [&](uint64_t instructionBudget, const char* reason) -> bool {
        uint32_t pc = 0;
        uint32_t ra = 0;
        uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
        uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
        const uint32_t startPc = pc;
        const auto sliceStart = std::chrono::steady_clock::now();
        // Guest worker threads run cooperatively on the host UI thread.  Keep
        // their slices short so touch input and presenter paints stay live even
        // when a worker performs a long pure-guest loop between API calls.
        const bool servicingQueuedMessages = std::strcmp(reason, "queued-message") == 0;
        const bool synchronousQueuedMessage = servicingQueuedMessages && hasPendingSynchronousMessage();
        const bool pendingUserInput = hasPendingUserInput() || recentlyQueuedUserInput();
        const bool backloggedQueuedWork = servicingQueuedMessages && guestMessages_.size() > 32;
        if (synchronousQueuedMessage) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 3000u);
        } else if (pendingUserInput) {
            instructionBudget = std::min<uint64_t>(instructionBudget, backloggedQueuedWork ? 50000u : 12000u);
        } else if (servicingQueuedMessages && !activeGuestThread_) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 6000u);
        } else if (activeGuestThread_) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 250000u);
        } else if (servicingQueuedMessages) {
            instructionBudget = std::min<uint64_t>(instructionBudget, 250000u);
        }
        const auto wallBudget = synchronousQueuedMessage
            ? std::chrono::milliseconds(12)
            : (pendingUserInput
                   ? (backloggedQueuedWork ? std::chrono::milliseconds(60) : std::chrono::milliseconds(12))
                   : (servicingQueuedMessages ? std::chrono::milliseconds(60)
                                              : (activeGuestThread_ ? std::chrono::milliseconds(60)
                                                                   : std::chrono::milliseconds(120))));
        beginInteractiveSlice(wallBudget, reason, instructionBudget);
        const uc_err err = uc_emu_start(uc_, pc, 0, 0, instructionBudget);
        const bool stoppedByWatchdog = interactiveSliceStopRequested_;
        endInteractiveSlice();
        uc_reg_read(uc_, UC_MIPS_REG_PC, &pc);
        uc_reg_read(uc_, UC_MIPS_REG_RA, &ra);
        if (err != UC_ERR_OK) {
            if ((err == UC_ERR_READ_UNMAPPED || err == UC_ERR_EXCEPTION) && pc == 0 && activeGuestThread_) {
                const uint32_t exitingThread = activeGuestThread_;
                const uint32_t exitCode = reg(UC_MIPS_REG_V0);
                spdlog::warn("guest thread reached null pc; treating as thread exit handle=0x{:08x} exitCode=0x{:08x} err={} ({}) ra=0x{:08x}",
                             exitingThread, exitCode, int(err), uc_strerror(err), ra);
                if (finishActiveGuestThread(exitCode)) {
                    pumpHostMessages();
                    compactQueuedPointerMotion();
                    enqueueDueTimers();
                    return true;
                }
            }
            spdlog::warn("interactive emulation stopped reason={} err={} ({}) pc=0x{:08x} ra=0x{:08x}",
                         reason, int(err), uc_strerror(err), pc, ra);
            auto describeAddress = [&](uint32_t address) {
                for (const auto& [base, module] : loadedModulesByBase_) {
                    const uint64_t begin = base;
                    const uint64_t end = begin + (module.imageSize ? module.imageSize : 0x1000u);
                    if (address >= begin && uint64_t(address) < end) {
                        std::ostringstream oss;
                        oss << module.name << "+0x" << std::hex << std::setw(8)
                            << std::setfill('0') << (address - base);
                        return oss.str();
                    }
                }
                return std::string{"<unmapped>"};
            };
            const uint32_t sp = reg(UC_MIPS_REG_SP);
            const uint32_t gp = reg(UC_MIPS_REG_GP);
            const uint32_t v0 = reg(UC_MIPS_REG_V0);
            const uint32_t a0 = reg(UC_MIPS_REG_A0);
            const uint32_t a1 = reg(UC_MIPS_REG_A1);
            const uint32_t a2 = reg(UC_MIPS_REG_A2);
            const uint32_t a3 = reg(UC_MIPS_REG_A3);
            const uint32_t t9 = reg(UC_MIPS_REG_T9);
            spdlog::warn("interactive crash context activeThread=0x{:08x} pc={} ra={} sp=0x{:08x} gp=0x{:08x} "
                         "v0=0x{:08x} a0=0x{:08x} a1=0x{:08x} a2=0x{:08x} a3=0x{:08x} t9=0x{:08x} queued={}",
                         activeGuestThread_, describeAddress(pc), describeAddress(ra), sp, gp,
                         v0, a0, a1, a2, a3, t9, guestMessages_.size());
            std::array<uint32_t, 8> stackWords{};
            if (sp && isGuestRangeReadable(sp, uint32_t(stackWords.size() * sizeof(uint32_t))) &&
                uc_mem_read(uc_, sp, stackWords.data(), stackWords.size() * sizeof(uint32_t)) == UC_ERR_OK) {
                spdlog::warn("interactive crash stack sp=0x{:08x}: {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                             sp,
                             stackWords[0], stackWords[1], stackWords[2], stackWords[3],
                             stackWords[4], stackWords[5], stackWords[6], stackWords[7]);
            }
            return false;
        }
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sliceStart).count();
        if (elapsedMs >= 250) {
            spdlog::info("long guest slice reason={} activeThread=0x{:08x} budget={} startPc=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={}",
                         reason, activeGuestThread_, instructionBudget, startPc, pc, ra, guestMessages_.size());
        }
        if (stoppedByWatchdog && activeGuestThread_) {
            spdlog::info("guest thread timeslice yield handle=0x{:08x} reason={} pc=0x{:08x} queued={}",
                         activeGuestThread_, reason, pc, guestMessages_.size());
            yieldActiveGuestThread("timeslice");
        }
        pumpHostMessages();
        compactQueuedPointerMotion();
        enqueueDueTimers();
        return true;
    };
    while (hasHostWindows()) {
        pollCrossProcessGuestMessages();
        enqueueDueTimers();
        if (!guestMessages_.empty() && hasHostWindows()) {
            compactQueuedPointerMotion();
            const uint64_t budget = 250000u;
            spdlog::debug("resuming guest for queued message queued={} budget={}", guestMessages_.size(), budget);
            if (!resumeGuestSlice(budget, "queued-message")) {
                return;
            }
            if (guestMessages_.empty() && hasHostWindows() && hasRunnableGuestThread()) {
                if (!activeGuestThread_) {
                    switchToRunnableGuestThread("queued-worker");
                }
                if (activeGuestThread_ && !resumeGuestSlice(25000, "queued-worker")) {
                    return;
                }
            }
        }
        const DWORD waitMs = std::max<DWORD>(1, std::min<DWORD>(50, timerWaitMilliseconds()));
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (guestMessages_.empty() && hasHostWindows() && !activeGuestThread_ && hasRunnableGuestThread()) {
            switchToRunnableGuestThread("idle-worker");
        }
        if (guestMessages_.empty() && hasHostWindows() && !resumeGuestSlice(5000000, "idle")) {
            return;
        }
        if (guestMessages_.empty() && !activeGuestThread_ && !hasRunnableGuestThread()) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        }
    }
#endif
}

std::optional<SyntheticModule> SyntheticDllRuntime::createModule(const std::string& dllName) {
    if (sameModule(dllName, "coredll.dll")) return createCoredll();
    if (sameModule(dllName, "commctrl.dll")) return createCommctrl();
    if (sameModule(dllName, "winsock.dll") || sameModule(dllName, "ws2.dll")) return createWinsock(dllName);
    if (sameModule(dllName, "ole32.dll")) return createOle32();
    if (sameModule(dllName, "oleaut32.dll")) return createOleAut32();
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
    registerCoredllSyncExports(module);
    registerExport(module, 0x0006, "ExitThread");
    registerCoredllTimeExports(module);
    registerExport(module, 0x002C, "GetAPIAddress");
    registerCoredllMemoryExports(module);
    registerExport(module, 0x0059, "SystemParametersInfoW");
    registerExport(module, 0x0058, "GlobalMemoryStatus");
    registerExport(module, 0x005A, "CreateDIBSection");
    registerExport(module, 0x005F, "RegisterClassW");
    registerCoredllRectExports(module);
    registerCoredllCommExports(module);
    registerCoredllFsExports(module);
    registerExport(module, 0x00B3, "DeviceIoControl");
    registerExport(module, 0x00C0, "IsDBCSLeadByteEx");
    registerExport(module, 0x00C1, "iswctype");
    registerExport(module, 0x00C4, "MultiByteToWideChar");
    registerExport(module, 0x00C5, "WideCharToMultiByte");
    registerExport(module, 0x00DD, "CharLowerW");
    registerExport(module, 0x00E0, "CharUpperW");
    registerExport(module, 0x00EA, "FormatMessageW");
    registerExport(module, 0x00F6, "CreateWindowExW");
    registerExport(module, 0x00F7, "SetWindowPos");
    registerExport(module, 0x00F8, "GetWindowRect");
    registerExport(module, 0x00F9, "GetClientRect");
    registerExport(module, 0x00FA, "InvalidateRect");
    registerExport(module, 0x00FB, "GetWindow");
    registerExport(module, 0x00FE, "ClientToScreen");
    registerExport(module, 0x00FF, "ScreenToClient");
    registerExport(module, 0x0102, "SetWindowLongW");
    registerExport(module, 0x0103, "GetWindowLongW");
    registerCoredllPaintExports(module);
    registerCoredllGuiExports(module);
    registerExport(module, 0x0108, "DefWindowProcW");
    registerExport(module, 0x0109, "DestroyWindow");
    registerExport(module, 0x010A, "ShowWindow");
    registerExport(module, 0x010B, "UpdateWindow");
    registerExport(module, 0x010D, "GetParent");
    registerExport(module, 0x0110, "MoveWindow");
    registerCoredllWindowExports(module);
    registerExport(module, 0x011D, "CallWindowProcW");
    registerExport(module, 0x011E, "FindWindowW");
    registerExport(module, 0x0143, "GetStoreInformation");
    registerCoredllAudioExports(module);
    registerCoredllRegistryExports(module);
    registerExport(module, 0x01BE, "WNetConnectionDialog1W");
    registerExport(module, 0x01C2, "WNetGetUniversalNameW");
    registerExport(module, 0x01C3, "WNetGetUserW");
    registerCoredllThreadExports(module);
    registerExport(module, 0x01ED, "CreateProcessW");
    registerExport(module, 0x01F1, "WaitForSingleObject");
    registerExport(module, 0x0210, "LoadLibraryW");
    registerExport(module, 0x0212, "GetProcAddressW");
    registerCoredllResExports(module);
    registerExport(module, 0x021E, "GetSystemInfo");
    registerExport(module, 0x0219, "GetModuleFileNameW");
    registerExport(module, 0x021D, "OutputDebugStringW");
    registerExport(module, 0x0224, "CreateFileMappingW");
    registerExport(module, 0x0225, "MapViewOfFile");
    registerExport(module, 0x0226, "UnmapViewOfFile");
    registerExport(module, 0x0227, "FlushViewOfFile");
    registerExport(module, 0x0228, "CreateFileForMapping");
    registerExport(module, 0x0229, "CloseHandle");
    registerExport(module, 0x022D, "KernelIoControl");
    registerCoredllSystemExports(module);
    registerExport(module, 0x02BA, "IsDialogMessageW");
    registerExport(module, 0x02CD, "GetVersionExW");
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
    registerExport(module, 0x0394, "GetDeviceCaps");
    registerExport(module, 0x037F, "CreateFontIndirectW");
    registerExport(module, 0x0380, "ExtTextOutW");
    registerExport(module, 0x0385, "CreateBitmap");
    registerExport(module, 0x0387, "BitBlt");
    registerExport(module, 0x038A, "TransparentImage");
    registerExport(module, 0x038E, "CreateCompatibleDC");
    registerExport(module, 0x0386, "CreateCompatibleBitmap");
    registerExport(module, 0x0389, "StretchBlt");
    registerExport(module, 0x0390, "DeleteObject");
    registerExport(module, 0x038F, "DeleteDC");
    registerExport(module, 0x0397, "GetStockObject");
    registerExport(module, 0x0396, "GetObjectW");
    registerExport(module, 0x0399, "SelectObject");
    registerExport(module, 0x039A, "SetBkColor");
    registerExport(module, 0x039B, "SetBkMode");
    registerExport(module, 0x039C, "SetTextColor");
    registerExport(module, 0x039D, "CreatePatternBrush");
    registerExport(module, 0x039E, "CreatePen");
    registerExport(module, 0x03A2, "CreatePenIndirect");
    registerExport(module, 0x03A3, "CreateSolidBrush");
    registerExport(module, 0x03A7, "FillRect");
    registerExport(module, 0x03AA, "PatBlt");
    registerExport(module, 0x03AB, "Polygon");
    registerExport(module, 0x03AC, "Polyline");
    registerExport(module, 0x03AD, "Rectangle");
    registerExport(module, 0x03AF, "SetBrushOrgEx");
    registerExport(module, 0x03B1, "DrawTextW");
    registerExport(module, 0x03D4, "CreateRectRgn");
    registerExport(module, 0x03C8, "CombineRgn");
    registerExport(module, 0x03CB, "GetClipBox");
    registerExport(module, 0x037C, "RegisterTaskBar");
    registerCoredllMathExports(module);
    registerCoredllCrtExports(module);
    registerExport(module, 0x0499, "GetModuleHandleW");
    registerExport(module, 0x04CE, "GetProcAddressA");
    registerExport(module, 0x05D1, "KernelLibIoControl");
    registerExport(module, 0x05E3, "RegisterDesktop");
    registerExport(module, 0x05EF, "GlobalAddAtomW");
    registerExport(module, 0x05F0, "GlobalDeleteAtom");
    registerExport(module, 0x05F1, "GlobalFindAtomW");
    registerExport(module, 0x0576, "SetWindowRgn");
    registerExport(module, 0x0577, "GetWindowRgn");
    registerExport(module, 0x0673, "MoveToEx");
    registerExport(module, 0x0674, "LineTo");
    registerExport(module, 0x0676, "SetTextAlign");
    registerExport(module, 0x0682, "SetDIBColorTable");
    registerExport(module, 0x0683, "StretchDIBits");
    registerExport(module, 0x06BD, "SetBitmapBits");
    registerExport(module, 0x06BE, "SetDIBitsToDevice");

    destroyWindowContinuationStub_ = module.imageBase + 0x0001f000;
    auto& destroyContinuation = exportsByAddress_[destroyWindowContinuationStub_];
    destroyContinuation.moduleName = module.moduleName;
    destroyContinuation.moduleKind = SyntheticModuleKind::Coredll;
    destroyContinuation.name = "__DestroyWindowContinue";
    writeStub(destroyWindowContinuationStub_);
    createWindowContinuationStub_ = module.imageBase + 0x0001f008;
    auto& createContinuation = exportsByAddress_[createWindowContinuationStub_];
    createContinuation.moduleName = module.moduleName;
    createContinuation.moduleKind = SyntheticModuleKind::Coredll;
    createContinuation.name = "__CreateWindowContinue";
    writeStub(createWindowContinuationStub_);
    blockingApiContinuationStub_ = module.imageBase + 0x0001f010;
    auto& blockingContinuation = exportsByAddress_[blockingApiContinuationStub_];
    blockingContinuation.moduleName = module.moduleName;
    blockingContinuation.moduleKind = SyntheticModuleKind::Coredll;
    blockingContinuation.name = "__BlockingApiContinue";
    writeStub(blockingApiContinuationStub_);
    updateWindowContinuationStub_ = module.imageBase + 0x0001f018;
    auto& updateWindowContinuation = exportsByAddress_[updateWindowContinuationStub_];
    updateWindowContinuation.moduleName = module.moduleName;
    updateWindowContinuation.moduleKind = SyntheticModuleKind::Coredll;
    updateWindowContinuation.name = "__UpdateWindowContinue";
    writeStub(updateWindowContinuationStub_);
    threadExitStub_ = module.imageBase + 0x0001f020;
    auto& threadExit = exportsByAddress_[threadExitStub_];
    threadExit.moduleName = module.moduleName;
    threadExit.moduleKind = SyntheticModuleKind::Coredll;
    threadExit.name = "__ThreadExit";
    writeStub(threadExitStub_);
    messageTransferContinuationStub_ = module.imageBase + 0x0001f028;
    auto& messageTransfer = exportsByAddress_[messageTransferContinuationStub_];
    messageTransfer.moduleName = module.moduleName;
    messageTransfer.moduleKind = SyntheticModuleKind::Coredll;
    messageTransfer.name = "__MessageTransferContinue";
    writeStub(messageTransferContinuationStub_);
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

    registerCommctrlExports(module);

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

std::optional<SyntheticModule> SyntheticDllRuntime::createGenericOrdinalDll(const SyntheticDllSpec& spec) {
    auto module = createGenericOrdinalDll(spec.name ? spec.name : "", spec.maxOrdinal);
    if (!module) return module;
    registerHandlers(*module, spec.handlers);
    return module;
}

void SyntheticDllRuntime::registerHandlers(SyntheticModule& module,
                                           const OrdinalHandlerGroup& group) {
    registerHandlers(module, group.handlers);
}

void SyntheticDllRuntime::registerHandlers(SyntheticModule& module,
                                           const OrdinalHandlerMap& handlers) {
    auto& dll = registeredDllsByName_[lowerAscii(module.moduleName)];
    if (dll.name.empty()) dll.name = module.moduleName;
    for (const auto& [ordinal, handler] : handlers) {
        dll.handlers[ordinal] = handler;
        registerExport(module, ordinal, handler.name ? handler.name : "", handler.code);
    }
}

void SyntheticDllRuntime::registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name,
                                         SyntheticExportCode code) {
    const uint32_t rva = 0x1000 + uint32_t(ordinal) * 8;
    module.exportsByOrdinal[ordinal] = rva;
    if (!name.empty()) {
        module.exportNamesByOrdinal[ordinal] = name;
        module.exportsByName[lowerAscii(name)] = rva;
    }
    const uint32_t address = module.imageBase + rva;
    auto& entry = exportsByAddress_[address];
    entry.moduleName = module.moduleName;
    entry.moduleKind = moduleKindForName(module.moduleName);
    entry.code = code;
    entry.ordinal = ordinal;
    if (!name.empty()) entry.name = name;
    writeStub(address);
}

const SyntheticDllRuntime::OrdinalHandlerSpec*
SyntheticDllRuntime::findOrdinalHandler(const ExportEntry& entry) const {
    const auto dllIt = registeredDllsByName_.find(lowerAscii(entry.moduleName));
    if (dllIt == registeredDllsByName_.end()) return nullptr;
    const auto handlerIt = dllIt->second.handlers.find(entry.ordinal);
    if (handlerIt == dllIt->second.handlers.end()) return nullptr;
    return &handlerIt->second;
}

SyntheticDllRuntime::SyntheticModuleKind
SyntheticDllRuntime::moduleKindForName(const std::string& moduleName) const {
    const std::string name = lowerAscii(moduleName);
    if (name == "coredll.dll") return SyntheticModuleKind::Coredll;
    if (name == "commctrl.dll") return SyntheticModuleKind::Commctrl;
    if (name == "winsock.dll" || name == "ws2.dll") return SyntheticModuleKind::Winsock;
    if (name == "ole32.dll") return SyntheticModuleKind::Ole32;
    if (name == "oleaut32.dll") return SyntheticModuleKind::OleAut32;
    return SyntheticModuleKind::Unknown;
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
                          runtime->interactiveSliceReason_, runtime->activeGuestThread_,
                          runtime->interactiveSliceInstructionBudget_, uint32_t(address), pc, ra,
                          runtime->guestMessages_.size());
        } else {
            spdlog::info("guest slice watchdog stop reason={} stopCause=deadline activeThread=0x{:08x} budget={} block=0x{:08x} pc=0x{:08x} ra=0x{:08x} queued={}",
                         runtime->interactiveSliceReason_, runtime->activeGuestThread_,
                         runtime->interactiveSliceInstructionBudget_, uint32_t(address), pc, ra,
                         runtime->guestMessages_.size());
        }
        runtime->interactiveSliceStopRequested_ = true;
    }
    uc_emu_stop(uc);
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

SyntheticDllRuntime::GuestCpuContext SyntheticDllRuntime::captureGuestCpuContext() const {
    static constexpr int kRegisters[] = {
        UC_MIPS_REG_PC, UC_MIPS_REG_RA, UC_MIPS_REG_SP, UC_MIPS_REG_GP, UC_MIPS_REG_FP,
        UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3,
        UC_MIPS_REG_V0, UC_MIPS_REG_V1, UC_MIPS_REG_AT,
        UC_MIPS_REG_T0, UC_MIPS_REG_T1, UC_MIPS_REG_T2, UC_MIPS_REG_T3, UC_MIPS_REG_T4,
        UC_MIPS_REG_T5, UC_MIPS_REG_T6, UC_MIPS_REG_T7, UC_MIPS_REG_T8, UC_MIPS_REG_T9,
        UC_MIPS_REG_S0, UC_MIPS_REG_S1, UC_MIPS_REG_S2, UC_MIPS_REG_S3, UC_MIPS_REG_S4,
        UC_MIPS_REG_S5, UC_MIPS_REG_S6, UC_MIPS_REG_S7,
        UC_MIPS_REG_K0, UC_MIPS_REG_K1, UC_MIPS_REG_HI, UC_MIPS_REG_LO,
    };
    GuestCpuContext context;
    context.valid = true;
    for (int regId : kRegisters) {
        uint32_t value = 0;
        uc_reg_read(uc_, regId, &value);
        context.registers[regId] = value;
    }
    return context;
}

SyntheticDllRuntime::GuestCpuContext SyntheticDllRuntime::initialGuestThreadContext(
    uint32_t startAddress,
    uint32_t parameter,
    uint32_t stackTop) const {
    GuestCpuContext context = captureGuestCpuContext();
    for (auto& [regId, value] : context.registers) {
        if (regId != UC_MIPS_REG_GP) value = 0;
    }
    context.registers[UC_MIPS_REG_PC] = startAddress;
    context.registers[UC_MIPS_REG_RA] = threadExitStub_;
    context.registers[UC_MIPS_REG_SP] = stackTop;
    context.registers[UC_MIPS_REG_FP] = 0;
    context.registers[UC_MIPS_REG_A0] = parameter;
    context.registers[UC_MIPS_REG_T9] = startAddress;
    return context;
}

void SyntheticDllRuntime::restoreGuestCpuContext(const GuestCpuContext& context) const {
    if (!context.valid) return;
    for (const auto& [regId, value] : context.registers) {
        uc_reg_write(uc_, regId, &value);
    }
}

const SyntheticDllRuntime::GuestThreadState* SyntheticDllRuntime::activeGuestThreadState() const {
    if (!activeGuestThread_) return nullptr;
    auto it = guestThreads_.find(activeGuestThread_);
    return it == guestThreads_.end() ? nullptr : &it->second;
}

std::string SyntheticDllRuntime::currentProcessModulePath() const {
    if (const auto* thread = activeGuestThreadState()) {
        if (!thread->modulePath.empty()) return thread->modulePath;
    }
    return mainModulePath_;
}

uint32_t SyntheticDllRuntime::currentProcessModuleBase() const {
    if (const auto* thread = activeGuestThreadState()) {
        if (thread->moduleBase) return thread->moduleBase;
    }
    return mainModuleBase_;
}

uint32_t SyntheticDllRuntime::createGuestThread(uint32_t startAddress, uint32_t parameter, uint32_t flags) {
    if (!startAddress || !threadExitStub_) {
        lastError_ = 87;
        return 0;
    }
    constexpr uint32_t kGuestThreadStackSize = 0x10000;
    const uint32_t stackBase = allocate(kGuestThreadStackSize, true);
    if (!stackBase) {
        lastError_ = 8;
        return 0;
    }
    const uint32_t guestHandle = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
    const uint32_t stackTop = (stackBase + kGuestThreadStackSize - 0x100u) & ~0x0fu;
    GuestThreadState thread;
    thread.handle = guestHandle;
    thread.threadId = nextGuestThreadId_++;
    thread.startAddress = startAddress;
    thread.parameter = parameter;
    thread.stackBase = stackBase;
    thread.stackSize = kGuestThreadStackSize;
    thread.tlsBase = allocate(64 * sizeof(uint32_t), true);
    if (!thread.tlsBase) {
        releaseAllocation(stackBase);
        lastError_ = 8;
        return 0;
    }
    thread.suspendCount = (flags & 0x00000004u) ? 1 : 0;
    thread.state = thread.suspendCount ? GuestThreadRunState::Suspended : GuestThreadRunState::Runnable;
    if (const auto* active = activeGuestThreadState()) {
        thread.processHandle = active->processHandle;
        thread.processId = active->processId;
        thread.moduleBase = active->moduleBase;
        thread.modulePath = active->modulePath;
    } else {
        thread.processHandle = mainProcessPseudoHandle_;
        thread.processId = mainProcessId_;
        thread.moduleBase = mainModuleBase_;
        thread.modulePath = mainModulePath_;
    }
    thread.context = initialGuestThreadContext(startAddress, parameter, stackTop);
    guestThreads_[guestHandle] = std::move(thread);
    lastError_ = 0;
    return guestHandle;
}

bool SyntheticDllRuntime::startGuestProcessImage(const std::string& guestApplication,
                                                 const std::filesystem::path& hostApplication,
                                                 uint32_t moduleBase,
                                                 uint32_t entryPoint,
                                                 const std::string& commandLine,
                                                 uint32_t& processHandle,
                                                 uint32_t& threadHandle,
                                                 uint32_t& processId,
                                                 uint32_t& threadId) {
    processHandle = 0;
    threadHandle = 0;
    processId = 0;
    threadId = 0;
    if (!moduleBase || !entryPoint || !threadExitStub_) {
        lastError_ = 87;
        return false;
    }

    constexpr uint32_t kGuestThreadStackSize = 0x10000;
    const uint32_t stackBase = allocate(kGuestThreadStackSize, true);
    if (!stackBase) {
        lastError_ = 8;
        return false;
    }
    const uint32_t commandLineChars = uint32_t(commandLine.size() + 1);
    const uint32_t commandLinePtr = allocate(std::max<uint32_t>(2, commandLineChars * 2), true);
    if (!commandLinePtr) {
        releaseAllocation(stackBase);
        lastError_ = 8;
        return false;
    }
    writeUtf16(commandLinePtr, commandLine, commandLineChars);

    processId = nextGuestProcessId_++;
    processHandle = makeGuestHandle({GuestHandle::Kind::GuestProcess, 0, processId});
    threadHandle = makeGuestHandle({GuestHandle::Kind::GuestThread, 0, 0});
    const uint32_t stackTop = (stackBase + kGuestThreadStackSize - 0x100u) & ~0x0fu;

    GuestThreadState thread;
    thread.handle = threadHandle;
    thread.threadId = nextGuestThreadId_++;
    thread.startAddress = entryPoint;
    thread.parameter = commandLinePtr;
    thread.stackBase = stackBase;
    thread.stackSize = kGuestThreadStackSize;
    thread.tlsBase = allocate(64 * sizeof(uint32_t), true);
    if (!thread.tlsBase) {
        releaseAllocation(commandLinePtr);
        releaseAllocation(stackBase);
        lastError_ = 8;
        return false;
    }
    thread.processHandle = processHandle;
    thread.processId = processId;
    thread.moduleBase = moduleBase;
    thread.modulePath = guestApplication.empty()
        ? ("\\" + pathToUtf8(hostApplication.filename()))
        : guestApplication;
    thread.state = GuestThreadRunState::Runnable;
    thread.context = initialGuestThreadContext(entryPoint, 0, stackTop);
    thread.context.registers[UC_MIPS_REG_GP] = moduleBase + 0x8000;
    thread.context.registers[UC_MIPS_REG_A0] = moduleBase;
    thread.context.registers[UC_MIPS_REG_A1] = 0;
    thread.context.registers[UC_MIPS_REG_A2] = commandLinePtr;
    thread.context.registers[UC_MIPS_REG_A3] = 1;
    thread.context.registers[UC_MIPS_REG_T9] = entryPoint;

    threadId = thread.threadId;
    guestThreads_[threadHandle] = std::move(thread);
    lastError_ = 0;
    spdlog::info("CreateProcessW guest image scheduled app=\"{}\" host=\"{}\" base=0x{:08x} entry=0x{:08x} process=0x{:08x}/{} thread=0x{:08x}/{} cmd=\"{}\"",
                 guestApplication, pathToUtf8(hostApplication), moduleBase, entryPoint,
                 processHandle, processId, threadHandle, threadId, commandLine);
    return true;
}

uint32_t SyntheticDllRuntime::resumeGuestThread(uint32_t guestHandle) {
    auto* handle = lookupGuestHandle(guestHandle);
    if (!handle || handle->kind != GuestHandle::Kind::GuestThread) {
        lastError_ = 6;
        return 0xffffffffu;
    }
    auto thread = guestThreads_.find(guestHandle);
    if (thread == guestThreads_.end()) {
        lastError_ = 0;
        return 1;
    }
    if (thread->second.state == GuestThreadRunState::Terminated) {
        lastError_ = 0;
        return 0;
    }
    const uint32_t previousSuspendCount = thread->second.suspendCount;
    if (thread->second.suspendCount) --thread->second.suspendCount;
    if (!thread->second.suspendCount && thread->second.state == GuestThreadRunState::Suspended) {
        thread->second.state = GuestThreadRunState::Runnable;
    }
    lastError_ = 0;
    return previousSuspendCount;
}

void SyntheticDllRuntime::wakeGuestThreadsWaitingForMessage() {
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::WaitingForMessage) continue;
        thread.state = GuestThreadRunState::Runnable;
        spdlog::info("guest thread message wait satisfied handle=0x{:08x}", threadHandle);
    }
}

void SyntheticDllRuntime::refreshSignaledGuestWaits() {
    refreshCompletedHostWaveBuffers();
#if defined(_WIN32)
    constexpr DWORD kWaitObject0 = WAIT_OBJECT_0;
    constexpr DWORD kWaitTimeout = WAIT_TIMEOUT;
    const uint64_t now = hostTickMilliseconds();
    for (auto& [threadHandle, thread] : guestThreads_) {
        if (thread.state != GuestThreadRunState::Waiting) continue;
        if (thread.sleepUntilMs) {
            if (now < thread.sleepUntilMs) continue;
            thread.sleepUntilMs = 0;
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[UC_MIPS_REG_V0] = 0;
            lastError_ = 0;
            spdlog::debug("guest thread sleep satisfied handle=0x{:08x}", threadHandle);
            continue;
        }
        std::vector<uint32_t> handles = thread.waitHandles;
        if (handles.empty() && thread.waitHandle) handles.push_back(thread.waitHandle);
        if (handles.empty()) continue;

        bool allReady = true;
        for (size_t i = 0; i < handles.size(); ++i) {
            auto* handle = lookupGuestHandle(handles[i]);
            if (!handle) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = 0xffffffffu;
                lastError_ = 6;
                spdlog::warn("guest thread wait invalid handle=0x{:08x} waitHandle=0x{:08x}",
                             threadHandle, handles[i]);
                break;
            }

            bool ready = false;
            DWORD wait = kWaitObject0;
            if (handle->hostValue &&
                (handle->kind == GuestHandle::Kind::HostEvent ||
                 handle->kind == GuestHandle::Kind::HostMutex ||
                 handle->kind == GuestHandle::Kind::GuestProcess ||
                 handle->kind == GuestHandle::Kind::GuestThread)) {
                wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0);
                ready = wait == kWaitObject0;
            } else if (handle->kind == GuestHandle::Kind::GuestThread) {
                auto waitedThread = guestThreads_.find(handles[i]);
                ready = waitedThread == guestThreads_.end() ||
                        waitedThread->second.state == GuestThreadRunState::Terminated;
            } else {
                ready = true;
            }

            if (!ready && wait != kWaitTimeout) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = 0xffffffffu;
                lastError_ = GetLastError();
                spdlog::warn("guest thread wait failed handle=0x{:08x} waitHandle=0x{:08x} error={}",
                             threadHandle, handles[i], lastError_);
                break;
            }

            allReady = allReady && ready;
            if (!thread.waitAll && ready) {
                thread.state = GuestThreadRunState::Runnable;
                thread.waitHandle = 0;
                thread.waitHandles.clear();
                thread.context.registers[UC_MIPS_REG_V0] = uint32_t(i);
                lastError_ = 0;
                spdlog::info("guest thread wait satisfied handle=0x{:08x} waitHandle=0x{:08x} index={}",
                             threadHandle, handles[i], i);
                break;
            }
        }
        if (thread.state == GuestThreadRunState::Waiting && thread.waitAll && allReady) {
            thread.state = GuestThreadRunState::Runnable;
            thread.waitHandle = 0;
            thread.waitHandles.clear();
            thread.context.registers[UC_MIPS_REG_V0] = 0;
            lastError_ = 0;
            spdlog::info("guest thread wait-all satisfied handle=0x{:08x} count={}",
                         threadHandle, handles.size());
        }
    }
#endif
}

bool SyntheticDllRuntime::hasRunnableGuestThread() {
    refreshSignaledGuestWaits();
    for (const auto& [handle, thread] : guestThreads_) {
        (void)handle;
        if (thread.state == GuestThreadRunState::Runnable) return true;
    }
    return false;
}

bool SyntheticDllRuntime::switchToRunnableGuestThread(const char* reason,
                                                      uint32_t returnAddress,
                                                      uint32_t preferredHandle) {
    refreshSignaledGuestWaits();
    auto switchTo = [&](uint32_t handle, GuestThreadState& thread) {
        if (activeGuestThread_) {
            auto active = guestThreads_.find(activeGuestThread_);
            if (active != guestThreads_.end()) {
                active->second.context = captureGuestCpuContext();
                if (returnAddress) active->second.context.registers[UC_MIPS_REG_PC] = returnAddress;
                if (active->second.state == GuestThreadRunState::Running) {
                    active->second.state = GuestThreadRunState::Runnable;
                }
            }
        } else {
            mainThreadContext_ = captureGuestCpuContext();
            if (returnAddress) mainThreadContext_.registers[UC_MIPS_REG_PC] = returnAddress;
            const uint32_t savedPc = mainThreadContext_.registers.count(UC_MIPS_REG_PC)
                ? mainThreadContext_.registers[UC_MIPS_REG_PC]
                : 0;
            const uint32_t savedRa = mainThreadContext_.registers.count(UC_MIPS_REG_RA)
                ? mainThreadContext_.registers[UC_MIPS_REG_RA]
                : 0;
            const uint32_t savedSp = mainThreadContext_.registers.count(UC_MIPS_REG_SP)
                ? mainThreadContext_.registers[UC_MIPS_REG_SP]
                : 0;
            spdlog::debug("main thread context saved reason={} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                          reason ? reason : "cooperate", savedPc, savedRa, savedSp);
        }
        const uint32_t threadPc = thread.context.registers.count(UC_MIPS_REG_PC)
            ? thread.context.registers.at(UC_MIPS_REG_PC)
            : 0;
        const uint32_t threadRa = thread.context.registers.count(UC_MIPS_REG_RA)
            ? thread.context.registers.at(UC_MIPS_REG_RA)
            : 0;
        const uint32_t threadSp = thread.context.registers.count(UC_MIPS_REG_SP)
            ? thread.context.registers.at(UC_MIPS_REG_SP)
            : 0;
        const uint32_t threadV0 = thread.context.registers.count(UC_MIPS_REG_V0)
            ? thread.context.registers.at(UC_MIPS_REG_V0)
            : 0;
        thread.state = GuestThreadRunState::Running;
        activeGuestThread_ = handle;
        lastScheduledGuestThread_ = handle;
        updateCurrentThreadKData(thread.threadId, thread.tlsBase);
        restoreGuestCpuContext(thread.context);
        spdlog::debug("guest thread switch reason={} handle=0x{:08x} start=0x{:08x} pc=0x{:08x} ra=0x{:08x} sp=0x{:08x} v0=0x{:08x}",
                      reason ? reason : "cooperate", handle, thread.startAddress,
                      threadPc, threadRa, threadSp, threadV0);
        return true;
    };

    if (preferredHandle) {
        auto preferred = guestThreads_.find(preferredHandle);
        if (preferred != guestThreads_.end() &&
            preferred->second.state == GuestThreadRunState::Runnable &&
            preferred->second.context.valid) {
            return switchTo(preferred->first, preferred->second);
        }
    }

    auto first = lastScheduledGuestThread_ ? guestThreads_.upper_bound(lastScheduledGuestThread_)
                                           : guestThreads_.begin();
    for (auto it = first; it != guestThreads_.end(); ++it) {
        if (it->second.state == GuestThreadRunState::Runnable && it->second.context.valid) {
            return switchTo(it->first, it->second);
        }
    }
    for (auto it = guestThreads_.begin(); it != first; ++it) {
        if (it->second.state == GuestThreadRunState::Runnable && it->second.context.valid) {
            return switchTo(it->first, it->second);
        }
    }
    return false;
}

bool SyntheticDllRuntime::yieldActiveGuestThread(const char* reason, uint32_t returnAddress) {
    if (!activeGuestThread_) return switchToRunnableGuestThread(reason, returnAddress);
    auto active = guestThreads_.find(activeGuestThread_);
    if (active != guestThreads_.end() && active->second.state == GuestThreadRunState::Running) {
        active->second.context = captureGuestCpuContext();
        if (returnAddress) active->second.context.registers[UC_MIPS_REG_PC] = returnAddress;
        active->second.state = GuestThreadRunState::Runnable;
        spdlog::info("guest thread yield reason={} handle=0x{:08x} pc=0x{:08x}",
                     reason ? reason : "cooperate", activeGuestThread_,
                     active->second.context.registers.count(UC_MIPS_REG_PC)
                         ? active->second.context.registers.at(UC_MIPS_REG_PC)
                         : 0);
    }
    activeGuestThread_ = 0;
    if (mainThreadContext_.valid) {
        updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
        restoreGuestCpuContext(mainThreadContext_);
        return true;
    }
    return switchToRunnableGuestThread(reason);
}

bool SyntheticDllRuntime::finishActiveGuestThread(uint32_t exitCode) {
    if (!activeGuestThread_) return false;
    auto active = guestThreads_.find(activeGuestThread_);
    if (active != guestThreads_.end()) {
        active->second.exitCode = exitCode;
        active->second.state = GuestThreadRunState::Terminated;
        active->second.context = captureGuestCpuContext();
        spdlog::info("guest thread exit handle=0x{:08x} exitCode=0x{:08x}", activeGuestThread_, exitCode);
    }
    if (lastScheduledGuestThread_ == activeGuestThread_) lastScheduledGuestThread_ = 0;
    activeGuestThread_ = 0;
    if (mainThreadContext_.valid) {
        updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
        restoreGuestCpuContext(mainThreadContext_);
        return true;
    }
    return switchToRunnableGuestThread("thread exit");
}

bool SyntheticDllRuntime::cooperateGuestThreadsAfterCall(const std::string& name, uint32_t returnAddress) {
    if (!returnAddress) returnAddress = reg(UC_MIPS_REG_RA);
    const bool yieldingCall = name == "Sleep" || name == "WaitForSingleObject" ||
                              name == "WaitForMultipleObjects";
    const bool queuedUiWork = name == "PostMessageW" || name == "InvalidateRect" ||
                              name == "SetTimer" || name == "ShowWindow";
    if (activeGuestThread_ && (yieldingCall || (queuedUiWork && !guestMessages_.empty()))) {
        return yieldActiveGuestThread(name.c_str(), returnAddress);
    }
    const bool processStarterCall = name == "CreateProcessW";
    if (activeGuestThread_ && processStarterCall && hasRunnableGuestThread()) {
        return yieldActiveGuestThread(name.c_str(), returnAddress);
    }
    if (!activeGuestThread_ && name == "Sleep" && hasRunnableGuestThread()) {
        setReg(UC_MIPS_REG_V0, 0);
        return switchToRunnableGuestThread(name.c_str(), returnAddress);
    }
    const bool threadStarterCall = name == "CreateThread" || name == "ResumeThread" ||
                                   processStarterCall;
    if (!activeGuestThread_ && threadStarterCall && hasRunnableGuestThread()) {
        return switchToRunnableGuestThread(name.c_str(), returnAddress);
    }
    return false;
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
        guestDeviceConfigsByHandle_.erase(guestHandle);
        fileReadCounts_.erase(guestHandle);
        fileSeekCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostCrtFile) {
        fileHandleDebugNames_.erase(guestHandle);
        fileReadCounts_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostFind ||
               it->second.kind == GuestHandle::Kind::GuestFind) {
        fileHandleDebugNames_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostWaveOut) {
        waveOutStates_.erase(guestHandle);
        hostWaveBuffers_.clear();
    } else if (it->second.kind == GuestHandle::Kind::GuestFileMapping) {
        const bool hasMappedView = std::any_of(mappedViews_.begin(), mappedViews_.end(),
                                               [&](const auto& entry) {
                                                   return entry.second.mappingHandle == guestHandle;
                                               });
        if (!hasMappedView) fileMappings_.erase(guestHandle);
    } else if (it->second.kind == GuestHandle::Kind::HostBitmap) {
        bitmaps_.erase(guestHandle);
        if (it->second.filePointer) releaseAllocation(it->second.filePointer);
    } else if (it->second.kind == GuestHandle::Kind::GuestThread) {
        auto thread = guestThreads_.find(guestHandle);
        if (thread != guestThreads_.end() && thread->second.state != GuestThreadRunState::Running) {
            releaseAllocation(thread->second.stackBase);
            releaseAllocation(thread->second.tlsBase);
            guestThreads_.erase(thread);
            if (lastScheduledGuestThread_ == guestHandle) lastScheduledGuestThread_ = 0;
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

bool SyntheticDllRuntime::readGuestWaitHandles(uint32_t count,
                                               uint32_t handlesPtr,
                                               std::vector<uint32_t>& handles) {
    handles.clear();
    if (!count || !handlesPtr || count > 64) {
        lastError_ = 87;
        return false;
    }
    handles.resize(count);
    if (uc_mem_read(uc_, handlesPtr, handles.data(), handles.size() * sizeof(uint32_t)) != UC_ERR_OK) {
        handles.clear();
        lastError_ = 87;
        return false;
    }
    return true;
}

uint32_t SyntheticDllRuntime::waitForMultipleGuestObjects(uint32_t count,
                                                          uint32_t handlesPtr,
                                                          bool waitAll) {
    constexpr uint32_t kWaitObject0 = 0x00000000u;
    constexpr uint32_t kWaitTimeout = 0x00000102u;
    constexpr uint32_t kWaitFailed = 0xffffffffu;

    std::vector<uint32_t> handles;
    if (!readGuestWaitHandles(count, handlesPtr, handles)) return kWaitFailed;

    bool allReady = true;
    for (uint32_t i = 0; i < count; ++i) {
        auto* handle = lookupGuestHandle(handles[i]);
        if (!handle) {
            lastError_ = 6;
            return kWaitFailed;
        }
        bool ready = false;
#if defined(_WIN32)
        if (handle->hostValue &&
            (handle->kind == GuestHandle::Kind::HostEvent ||
             handle->kind == GuestHandle::Kind::HostMutex ||
             handle->kind == GuestHandle::Kind::GuestProcess ||
             handle->kind == GuestHandle::Kind::GuestThread)) {
            ready = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0) == kWaitObject0;
        } else
#endif
        if (handle->kind == GuestHandle::Kind::GuestThread) {
            auto thread = guestThreads_.find(handles[i]);
            ready = thread == guestThreads_.end() ||
                    thread->second.state == GuestThreadRunState::Terminated;
        }
        else {
            ready = true;
        }
        allReady = allReady && ready;
        if (!waitAll && ready) {
            lastError_ = 0;
            return kWaitObject0 + i;
        }
    }

    lastError_ = 0;
    return waitAll && allReady ? kWaitObject0 : kWaitTimeout;
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
    const uint32_t ra = reg(UC_MIPS_REG_RA);
    const std::string deviceKey = normalizeGuestDeviceName(guestPath);
    const auto mapped = serialDevicesByGuest_.find(deviceKey);
    if (mapped != serialDevicesByGuest_.end()) {
        const SerialDeviceConfig& config = mapped->second;
        const std::string note = config.note.empty() ? std::string{} : " (" + config.note + ")";
        if (!config.enabled || config.backend == "stub" || config.type == "ioctl_device") {
            const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
            const std::string state = config.enabled ? config.backend : std::string("disabled");
            fileHandleDebugNames_[guest] = guestPath + " -> " + config.type + " " + state + note;
            guestDeviceConfigsByHandle_[guest] = config;
            lastError_ = 0;
            spdlog::info("CreateFileW guest device=\"{}\" mapped type={} backend={} enabled={} guestHandle=0x{:08x} access=0x{:08x} share=0x{:08x} ra=0x{:08x}{}",
                         guestPath, config.type, config.backend, config.enabled ? 1 : 0,
                         guest, access, share, ra, note);
            return guest;
        }

#if defined(_WIN32)
        const std::wstring hostPort = normalizeHostCommPort(config.host);
        const DWORD desiredAccess = access ? access : (GENERIC_READ | GENERIC_WRITE);
        HANDLE host = CreateFileW(hostPort.c_str(), desiredAccess, share, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::string displayName = narrowAsciiLossy(hostPort);
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
        spdlog::warn("CreateFileW guest device=\"{}\" host=\"{}\" unavailable lastError={} ra=0x{:08x}; using stub guest device{}",
                     guestPath, displayName, GetLastError(), ra, note);
#else
        spdlog::warn("CreateFileW guest device=\"{}\" backend=win32_com unavailable on this host ra=0x{:08x}; using stub guest device{}",
                     guestPath, ra, note);
#endif
    }
    const uint32_t guest = makeGuestHandle({GuestHandle::Kind::GuestSerialDevice, 0, 0});
    fileHandleDebugNames_[guest] = guestPath + " -> disconnected";
    lastError_ = 0;
    spdlog::info("CreateFileW guest device=\"{}\" guestHandle=0x{:08x} disconnected access=0x{:08x} share=0x{:08x} ra=0x{:08x}",
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
        auto configIt = guestDeviceConfigsByHandle_.find(handleValue);
        if (configIt != guestDeviceConfigsByHandle_.end() &&
            configIt->second.enabled &&
            configIt->second.type == "ioctl_device" &&
            configIt->second.backend == "nanduuid_return" &&
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
                                 configIt->second.guest, controlCode, outSize,
                                 pathToUtf8(sdmmcHostRoot_ / "Device.uid"), compactId);
                    return 1;
                }
                const uint32_t transferred = std::min<uint32_t>(outSize, uint32_t(uid.size()));
                uc_mem_write(uc_, outPtr, uid.data(), transferred);
                if (bytesReturnedPtr) writeU32(bytesReturnedPtr, transferred);
                lastError_ = 0;
                spdlog::info("DeviceIoControl ioctl backend=NANDUUID_RETURN guest=\"{}\" code=0x{:08x} outSize={} transferred={} source=\"{}\" value=\"{}\"",
                             configIt->second.guest, controlCode, outSize, transferred,
                             pathToUtf8(sdmmcHostRoot_ / "Device.uid"),
                             std::string(reinterpret_cast<const char*>(uid.data()),
                                         reinterpret_cast<const char*>(uid.data()) + std::min<std::streamsize>(available, uid.size())));
                return 1;
            }
            lastError_ = 2;
            spdlog::warn("DeviceIoControl ioctl backend=NANDUUID_RETURN guest=\"{}\" code=0x{:08x} missing source=\"{}\"",
                         configIt->second.guest, controlCode, pathToUtf8(sdmmcHostRoot_ / "Device.uid"));
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
    brushes_[handle] = GuestBrush{colorRef, 0, stock};
    return handle;
}

uint32_t SyntheticDllRuntime::createPatternBrushFromBitmap(uint32_t bitmapHandle) {
    uint32_t colorRef = 0;
    auto bitmapIt = bitmaps_.find(bitmapHandle);
    if (bitmapIt != bitmaps_.end()) {
        const GuestBitmap& bitmap = bitmapIt->second;
        const int32_t height = std::abs(bitmap.heightRaw);
        const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
        if (bitmap.bits && bitmap.width > 0 && height > 0 && byteCount && byteCount <= 0x2000000ull) {
            std::vector<uint8_t> bits(static_cast<size_t>(byteCount));
            if (uc_mem_read(uc_, bitmap.bits, bits.data(), bits.size()) == UC_ERR_OK) {
                for (int32_t y = 0; y < height; ++y) {
                    bool found = false;
                    for (int32_t x = 0; x < bitmap.width; ++x) {
                        uint32_t pixel = 0;
                        if (readBitmapPixel(bitmap, bits, height, x, y, pixel) &&
                            (pixel & 0x00ffffffu) != 0) {
                            colorRef = ((pixel >> 16) & 0x000000ffu) |
                                       (pixel & 0x0000ff00u) |
                                       ((pixel & 0x000000ffu) << 16);
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
    }
    const uint32_t brush = makeGuestBrush(colorRef);
    brushes_[brush].patternBitmap = bitmapHandle;
    return brush;
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
    case 21: { // DEFAULT_BITMAP
        handle = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, 0});
        GuestBitmap bitmap{};
        bitmap.width = 1;
        bitmap.heightRaw = -1;
        bitmap.bpp = 1;
        bitmap.stride = 4;
        bitmap.palette = defaultIndexedPalette(1);
        bitmap.stock = true;
        bitmaps_[handle] = std::move(bitmap);
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
    window.ownerThread = activeGuestThread_ ? activeGuestThread_ : mainThreadPseudoHandle_;
    window.zOrder = nextWindowZOrder();
    window.x = x;
    window.y = y;
    window.width = std::max<int32_t>(1, width);
    window.height = std::max<int32_t>(1, height);
    window.visible = visible;
    windows_[hwnd] = window;
    ensureHostWindow(hwnd, windows_[hwnd]);
    publishGuestWindowState(hwnd);
    return hwnd;
}

uint64_t SyntheticDllRuntime::nextWindowZOrder() {
    return ++windowZOrder_;
}

uint64_t SyntheticDllRuntime::windowZOrder(uint32_t hwnd) const {
    const auto it = windows_.find(hwnd);
    return it == windows_.end() ? 0 : it->second.zOrder;
}

std::vector<uint32_t> SyntheticDllRuntime::orderedWindowsTopToBottom() const {
    std::vector<uint32_t> ordered;
    ordered.reserve(windows_.size());
    for (const auto& [hwnd, _] : windows_) ordered.push_back(hwnd);
    std::sort(ordered.begin(), ordered.end(), [&](uint32_t left, uint32_t right) {
        const auto leftIt = windows_.find(left);
        const auto rightIt = windows_.find(right);
        const uint64_t leftZ = leftIt == windows_.end() ? 0 : leftIt->second.zOrder;
        const uint64_t rightZ = rightIt == windows_.end() ? 0 : rightIt->second.zOrder;
        if (leftZ != rightZ) return leftZ > rightZ;
        return left > right;
    });
    return ordered;
}

std::vector<uint32_t> SyntheticDllRuntime::orderedSiblingWindows(uint32_t parent,
                                                                 bool childWindow) const {
    std::vector<uint32_t> siblings;
    for (const auto& [hwnd, window] : windows_) {
        if (!window.destroyed && window.parent == parent &&
            ((window.style & kWindowStyleChild) != 0) == childWindow) {
            siblings.push_back(hwnd);
        }
    }
    std::sort(siblings.begin(), siblings.end(), [&](uint32_t left, uint32_t right) {
        const auto leftIt = windows_.find(left);
        const auto rightIt = windows_.find(right);
        const uint64_t leftZ = leftIt == windows_.end() ? 0 : leftIt->second.zOrder;
        const uint64_t rightZ = rightIt == windows_.end() ? 0 : rightIt->second.zOrder;
        if (leftZ != rightZ) return leftZ > rightZ;
        return left > right;
    });
    return siblings;
}

void SyntheticDllRuntime::ensureHostWindow(uint32_t guestHwnd, GuestWindow& window) {
#if defined(_WIN32)
    if (window.destroyed) return;
    if (window.parent || !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    if (hostPresenterGuestHwnd_ && hostPresenterGuestHwnd_ != guestHwnd) return;
    if (!window.hostHwnd) {
        if (!registerHostPresenterClass()) {
            spdlog::warn("host presenter RegisterClassW failed error={}", GetLastError());
            return;
        }
        auto* presenter = new HostPresenterWindow{this, guestHwnd, framebuffer_, framebufferWidth_, framebufferHeight_};
        const std::wstring title = widenLossy(window.title.empty() ? "FakeCE" : window.title);
        HWND hwnd = CreateWindowExW(hostPresenterWindowExStyle(), hostPresenterClassName(), title.c_str(),
                                    hostPresenterWindowStyle(),
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    hostPresenterOuterWidth(*presenter), hostPresenterOuterHeight(*presenter),
                                    nullptr, nullptr, GetModuleHandleW(nullptr), presenter);
        if (!hwnd) {
            spdlog::warn("host presenter CreateWindowExW failed guest=0x{:08x} error={}", guestHwnd, GetLastError());
            delete presenter;
            return;
        }
        hostPresenterGuestHwnd_ = guestHwnd;
        window.hostHwnd = reinterpret_cast<uintptr_t>(hwnd);
        spdlog::info("created host presenter HWND={} for guest HWND=0x{:08x} guest={}x{} framebuffer={}x{}",
                     static_cast<void*>(hwnd), guestHwnd, window.width, window.height,
                     presenter->width, presenter->height);
    }
    HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
    if (window.visible) ShowWindow(hwnd, SW_SHOWNORMAL);
    presentHostWindows(true);
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
            uint32_t replacementHwnd = 0;
            GuestWindow* replacementWindow = nullptr;
            for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
                GuestWindow& candidate = it->second;
                if (candidate.hwnd == window.hwnd || candidate.destroyed || !candidate.visible || candidate.hostHwnd) {
                    continue;
                }
                const bool coversFramebuffer =
                    candidate.width >= framebufferWidth_ && candidate.height >= framebufferHeight_;
                const bool ownedByDestroyedWindow = candidate.parent == window.hwnd;
                if (!replacementWindow || coversFramebuffer || ownedByDestroyedWindow) {
                    replacementHwnd = candidate.hwnd;
                    replacementWindow = &candidate;
                    if (coversFramebuffer || ownedByDestroyedWindow) break;
                }
            }
            if (replacementWindow) {
                auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (presenter) presenter->guestHwnd = replacementHwnd;
                replacementWindow->hostHwnd = window.hostHwnd;
                window.hostHwnd = 0;
                hostPresenterGuestHwnd_ = replacementHwnd;
                SetWindowTextW(hwnd, widenLossy(replacementWindow->title.empty()
                                                    ? "FakeCE"
                                                    : replacementWindow->title).c_str());
                spdlog::info("transferred host presenter HWND={} from destroyed guest HWND=0x{:08x} to live guest HWND=0x{:08x}",
                             static_cast<void*>(hwnd), window.hwnd, replacementHwnd);
                presentHostWindows(true);
                return;
            }
            SetWindowTextW(hwnd, L"FakeCE presenter (guest HWND destroyed)");
            ShowWindow(hwnd, SW_SHOWNORMAL);
            presentHostWindows(true);
            retainedHostWindows_.push_back(window.hostHwnd);
            if (hostPresenterGuestHwnd_ == window.hwnd) hostPresenterGuestHwnd_ = 0;
        }
        window.hostHwnd = 0;
    }
#else
    (void)window;
#endif
}

void SyntheticDllRuntime::syncHostWindowPlacement(GuestWindow& window, bool present) {
#if defined(_WIN32)
    if (!window.hostHwnd) return;
    HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
    if (!IsWindow(hwnd)) return;
    auto* presenter = reinterpret_cast<HostPresenterWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    const int hostWidth = presenter ? hostPresenterOuterWidth(*presenter) : std::max(1, window.width);
    const int hostHeight = presenter ? hostPresenterOuterHeight(*presenter) : std::max(1, window.height);
    SetWindowPos(hwnd, nullptr, window.x, window.y, hostWidth, hostHeight,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, window.visible ? SW_SHOWNORMAL : SW_HIDE);
    if (present) presentHostWindows(true);
#else
    (void)window;
    (void)present;
#endif
}

void SyntheticDllRuntime::presentHostWindows(bool force) {
#if defined(_WIN32)
    if (!force && !hostPresentDirty_) return;
    const uint64_t now = hostTickMilliseconds();
    constexpr uint64_t kMinPresentIntervalMs = 16;
    if (!force && lastHostPresentMs_ && now - lastHostPresentMs_ < kMinPresentIntervalMs) return;
    hostPresentDirty_ = false;
    lastHostPresentMs_ = now;
    for (auto& [guestHwnd, window] : windows_) {
        (void)guestHwnd;
        if (!window.hostHwnd) continue;
        HWND hwnd = reinterpret_cast<HWND>(window.hostHwnd);
        if (!IsWindow(hwnd)) continue;
        presentHostWindowNow(hwnd);
    }
    for (uintptr_t hostHwnd : retainedHostWindows_) {
        HWND hwnd = reinterpret_cast<HWND>(hostHwnd);
        if (!hwnd || !IsWindow(hwnd)) continue;
        presentHostWindowNow(hwnd);
    }
#else
    (void)force;
#endif
}

void SyntheticDllRuntime::invalidateHostWindows() {
    hostPresentDirty_ = true;
}

void SyntheticDllRuntime::queueGuestPaint(uint32_t hwnd, bool erase) {
    auto it = windows_.find(hwnd);
    if (!hwnd || it == windows_.end() || it->second.destroyed || !it->second.visible) return;
    const auto hasQueued = [&](uint32_t message) {
        return std::any_of(guestMessages_.begin(), guestMessages_.end(),
                           [&](const GuestMessage& queued) {
                               return queued.hwnd == hwnd && queued.message == message;
                           });
    };
    if (erase) {
        if (!hasQueued(0x0014)) {
            GuestMessage eraseMessage{};
            eraseMessage.hwnd = hwnd;
            eraseMessage.message = 0x0014; // WM_ERASEBKGND
            eraseMessage.wParam = makeGuestDc(hwnd);
            eraseMessage.time = uint32_t(++tick_ * 16);
            guestMessages_.push_back(eraseMessage);
        }
    }
    if (!hasQueued(0x000f)) {
        GuestMessage paint{};
        paint.hwnd = hwnd;
        paint.message = 0x000f; // WM_PAINT
        paint.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(paint);
    }
    invalidateHostWindows();
}

void SyntheticDllRuntime::prioritizeQueuedWindowMessages(uint32_t hwnd) {
    std::deque<GuestMessage> selected;
    for (auto it = guestMessages_.begin(); it != guestMessages_.end();) {
        if (it->hwnd == hwnd) {
            selected.push_back(*it);
            it = guestMessages_.erase(it);
        } else {
            ++it;
        }
    }
    while (!selected.empty()) {
        guestMessages_.push_front(selected.back());
        selected.pop_back();
    }
}

void SyntheticDllRuntime::queueVisibleFullScreenPopupPaint(uint32_t hwnd) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end() || it->second.destroyed || !it->second.visible ||
        !isOwnedPopupWindow(hwnd) || !guestWindowCoversFramebuffer(hwnd)) {
        return;
    }
    const bool replacesOlderFullScreenPopup = std::any_of(windows_.begin(), windows_.end(),
                                                          [&](const auto& entry) {
                                                              const uint32_t otherHwnd = entry.first;
                                                              const GuestWindow& other = entry.second;
                                                              return windowZOrder(otherHwnd) < windowZOrder(hwnd) &&
                                                                     !other.destroyed && other.visible &&
                                                                     isOwnedPopupWindow(otherHwnd) &&
                                                                     guestWindowCoversFramebuffer(otherHwnd);
                                                          });
    if (!replacesOlderFullScreenPopup) return;

    retireOlderFullScreenOwnedPopupsForPopup(hwnd);
    const bool hasShow = std::any_of(guestMessages_.begin(), guestMessages_.end(),
                                     [&](const GuestMessage& message) {
                                         return message.hwnd == hwnd && message.message == 0x0018;
                                     });
    if (!hasShow) {
        GuestMessage show{};
        show.hwnd = hwnd;
        show.message = 0x0018; // WM_SHOWWINDOW
        show.wParam = 1;
        show.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(show);
    }
    queueGuestPaint(hwnd, true);
    prioritizeQueuedWindowMessages(hwnd);
    spdlog::info("prioritized visible full-screen owned popup paint hwnd=0x{:08x}", hwnd);
}

void SyntheticDllRuntime::queueVisiblePopupPaint(uint32_t hwnd) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end() || it->second.destroyed || !it->second.visible ||
        (it->second.style & kWindowStyleChild) || it->second.width <= 0 || it->second.height <= 0) {
        return;
    }
    constexpr uint32_t kWindowStylePopup = 0x80000000u;
    const bool ownedPopup = isOwnedPopupWindow(hwnd);
    const bool topLevelPopup = !it->second.parent && (it->second.style & kWindowStylePopup);
    if (!ownedPopup && !topLevelPopup) return;
    if (ownedPopup && guestWindowCoversFramebuffer(hwnd)) return;

    const bool hasShow = std::any_of(guestMessages_.begin(), guestMessages_.end(),
                                     [&](const GuestMessage& message) {
                                         return message.hwnd == hwnd && message.message == 0x0018;
                                     });
    if (!hasShow) {
        GuestMessage show{};
        show.hwnd = hwnd;
        show.message = 0x0018; // WM_SHOWWINDOW
        show.wParam = 1;
        show.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(show);
    }
    queueGuestPaint(hwnd, true);
    size_t discardedMouseMoves = 0;
    guestMessages_.erase(std::remove_if(guestMessages_.begin(), guestMessages_.end(),
                                        [&](const GuestMessage& message) {
                                            if (message.message != 0x0200) return false;
                                            if (message.hwnd == hwnd ||
                                                isWindowInOwnedPopupStack(message.hwnd, hwnd)) {
                                                return false;
                                            }
                                            ++discardedMouseMoves;
                                            return true;
                                        }),
                         guestMessages_.end());
    prioritizeQueuedWindowMessages(hwnd);
    spdlog::info("queued visible popup paint hwnd=0x{:08x} discardedMouseMoves={}",
                 hwnd, discardedMouseMoves);
}

void SyntheticDllRuntime::queueVisiblePopupPaintsAbove(uint32_t hwnd) {
    std::vector<uint32_t> popups;
    for (const auto& [popupHwnd, window] : windows_) {
        if (windowZOrder(popupHwnd) <= windowZOrder(hwnd) || window.destroyed || !window.visible ||
            (window.style & kWindowStyleChild) || window.width <= 0 || window.height <= 0) {
            continue;
        }
        constexpr uint32_t kWindowStylePopup = 0x80000000u;
        const bool ownedPopup = isOwnedPopupWindow(popupHwnd);
        const bool topLevelPopup = !window.parent && (window.style & kWindowStylePopup);
        if (!ownedPopup && !topLevelPopup) continue;
        popups.push_back(popupHwnd);
    }

    if (popups.empty()) return;
    for (uint32_t popupHwnd : popups) {
        queueGuestPaint(popupHwnd, true);
    }
    for (auto it = popups.rbegin(); it != popups.rend(); ++it) {
        prioritizeQueuedWindowMessages(*it);
    }
    spdlog::info("queued {} visible popup repaint(s) above hwnd=0x{:08x}",
                 popups.size(), hwnd);
}

std::pair<int32_t, int32_t> SyntheticDllRuntime::guestWindowOrigin(uint32_t hwnd) const {
    int32_t x = 0;
    int32_t y = 0;
    for (uint32_t current = hwnd; current;) {
        auto it = windows_.find(current);
        if (it == windows_.end()) break;
        x += it->second.x;
        y += it->second.y;
        current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
    }
    return {x, y};
}

void SyntheticDllRuntime::noteGuestWindowPaint(uint32_t hwnd,
                                               int32_t left,
                                               int32_t top,
                                               int32_t right,
                                               int32_t bottom) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end() || it->second.destroyed || left >= right || top >= bottom) return;
    GuestWindow& window = it->second;
    if (!window.paintBoundsValid) {
        window.paintLeft = left;
        window.paintTop = top;
        window.paintRight = right;
        window.paintBottom = bottom;
        window.paintBoundsValid = true;
        return;
    }
    window.paintLeft = std::min(window.paintLeft, left);
    window.paintTop = std::min(window.paintTop, top);
    window.paintRight = std::max(window.paintRight, right);
    window.paintBottom = std::max(window.paintBottom, bottom);
}

void SyntheticDllRuntime::captureGuestWindowBacking(uint32_t hwnd) {
    auto it = windows_.find(hwnd);
    if (it == windows_.end()) return;
    const bool childWindow = (it->second.style & kWindowStyleChild) != 0;
    const bool ownedPopup = isOwnedPopupWindow(hwnd);
    const bool fullScreenPopup = ownedPopup && guestWindowCoversFramebuffer(hwnd);
    if (!fullScreenPopup && coveringFullScreenOwnedPopup(hwnd)) return;
    if (fullScreenPopup) {
        retireOlderFullScreenOwnedPopupsForPopup(hwnd);
    }
    const uint32_t visualParent = (childWindow || ownedPopup) ? it->second.parent : 0;
    if (!visualParent || it->second.backingValid ||
        !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        it->second.width <= 0 || it->second.height <= 0) {
        return;
    }
    auto parent = windows_.find(visualParent);
    if (parent == windows_.end()) return;
    if (childWindow && !parent->second.parent) return;

    const auto [originX, originY] = guestWindowOrigin(hwnd);
    const int32_t left = std::clamp<int32_t>(originX, 0, framebufferWidth_);
    const int32_t top = std::clamp<int32_t>(originY, 0, framebufferHeight_);
    const int32_t right = std::clamp<int32_t>(originX + it->second.width, 0, framebufferWidth_);
    const int32_t bottom = std::clamp<int32_t>(originY + it->second.height, 0, framebufferHeight_);
    if (left >= right || top >= bottom) return;

    const int32_t width = right - left;
    const int32_t height = bottom - top;
    std::vector<uint32_t> pixels(size_t(width) * size_t(height));
    for (int32_t y = 0; y < height; ++y) {
        const uint32_t* src = framebuffer_ + size_t(top + y) * size_t(framebufferWidth_) + size_t(left);
        std::copy(src, src + width, pixels.begin() + size_t(y) * size_t(width));
    }
    it->second.backingX = left;
    it->second.backingY = top;
    it->second.backingWidth = width;
    it->second.backingHeight = height;
    it->second.backingPixels = std::move(pixels);
    it->second.backingValid = true;
    spdlog::info("captured guest window backing hwnd=0x{:08x} rect={},{} {}x{}",
                 hwnd, left, top, width, height);
}

bool SyntheticDllRuntime::guestWindowCoversFramebuffer(uint32_t hwnd) const {
    auto it = windows_.find(hwnd);
    if (it == windows_.end() || it->second.destroyed || !framebuffer_ ||
        framebufferWidth_ <= 0 || framebufferHeight_ <= 0) {
        return false;
    }
    const auto [x, y] = guestWindowOrigin(hwnd);
    return x <= 0 && y <= 0 &&
           x + it->second.width >= framebufferWidth_ &&
           y + it->second.height >= framebufferHeight_;
}

bool SyntheticDllRuntime::isWindowInOwnedPopupStack(uint32_t hwnd, uint32_t ancestor) const {
    for (uint32_t current = hwnd; current;) {
        if (current == ancestor) return true;
        auto it = windows_.find(current);
        if (it == windows_.end()) break;
        current = it->second.parent;
    }
    return false;
}

uint32_t SyntheticDllRuntime::coveringFullScreenOwnedPopup(uint32_t hwnd) const {
    for (uint32_t popupHwnd : orderedWindowsTopToBottom()) {
        const auto it = windows_.find(popupHwnd);
        if (it == windows_.end()) continue;
        const GuestWindow& popup = it->second;
        if (windowZOrder(popupHwnd) <= windowZOrder(hwnd) || popup.destroyed || !popup.visible ||
            !isOwnedPopupWindow(popupHwnd) || !guestWindowCoversFramebuffer(popupHwnd)) {
            continue;
        }
        if (isWindowInOwnedPopupStack(hwnd, popupHwnd)) continue;
        return popupHwnd;
    }
    return 0;
}

void SyntheticDllRuntime::retireOlderFullScreenOwnedPopupsForPopup(uint32_t popupHwnd) {
    auto target = windows_.find(popupHwnd);
    if (target == windows_.end() || target->second.destroyed || !target->second.visible ||
        !isOwnedPopupWindow(popupHwnd) || !guestWindowCoversFramebuffer(popupHwnd)) {
        return;
    }

    std::vector<uint32_t> retired;
    for (auto& [hwnd, window] : windows_) {
        if (window.zOrder >= target->second.zOrder || window.destroyed || !window.visible ||
            !isOwnedPopupWindow(hwnd) || !guestWindowCoversFramebuffer(hwnd)) {
            continue;
        }

        restoreGuestWindowBacking(hwnd, window, true, false);
        window.visible = false;
        window.backingValid = false;
        window.backingPixels.clear();
        retired.push_back(hwnd);
    }

    if (retired.empty()) return;
    guestMessages_.erase(std::remove_if(guestMessages_.begin(), guestMessages_.end(),
                                        [&](const GuestMessage& message) {
                                            return std::find(retired.begin(), retired.end(), message.hwnd) != retired.end();
                                        }),
                         guestMessages_.end());
    for (uint32_t hwnd : retired) {
        spdlog::info("retired older full-screen owned popup hwnd=0x{:08x} for popup hwnd=0x{:08x}",
                     hwnd, popupHwnd);
    }
}

bool SyntheticDllRuntime::restoreGuestWindowBacking(uint32_t hwnd,
                                                   GuestWindow& window,
                                                   bool allowCoveredByNewer,
                                                   bool presentRestoredFrame) {
    (void)hwnd;
    if (!window.backingValid || window.backingPixels.empty() ||
        !framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        window.backingWidth <= 0 || window.backingHeight <= 0) {
        return false;
    }
    if (!allowCoveredByNewer && isOwnedPopupWindow(hwnd) && guestWindowCoversFramebuffer(hwnd)) {
        const auto coversFramebuffer = [&](const GuestWindow& candidate) {
            const auto [x, y] = guestWindowOrigin(candidate.hwnd);
            return x <= 0 && y <= 0 &&
                   x + candidate.width >= framebufferWidth_ &&
                   y + candidate.height >= framebufferHeight_;
        };
        for (const auto& [candidateHwnd, candidate] : windows_) {
            if (candidate.zOrder > windowZOrder(hwnd) && !candidate.destroyed && candidate.visible &&
                candidate.parent && !(candidate.style & kWindowStyleChild) && coversFramebuffer(candidate)) {
                spdlog::info("skipped stale full-screen owned popup backing restore hwnd=0x{:08x} newer=0x{:08x}",
                             hwnd, candidateHwnd);
                window.backingValid = false;
                window.backingPixels.clear();
                return true;
            }
        }
    }
    if (isOwnedPopupWindow(hwnd) && !guestWindowCoversFramebuffer(hwnd) &&
        coveringFullScreenOwnedPopup(hwnd)) {
        spdlog::info("skipped covered owned popup backing restore hwnd=0x{:08x}", hwnd);
        window.backingValid = false;
        window.backingPixels.clear();
        return true;
    }
    const int32_t left = std::clamp<int32_t>(window.backingX, 0, framebufferWidth_);
    const int32_t top = std::clamp<int32_t>(window.backingY, 0, framebufferHeight_);
    const int32_t right = std::clamp<int32_t>(window.backingX + window.backingWidth, 0, framebufferWidth_);
    const int32_t bottom = std::clamp<int32_t>(window.backingY + window.backingHeight, 0, framebufferHeight_);
    if (left >= right || top >= bottom) {
        window.backingValid = false;
        window.backingPixels.clear();
        return false;
    }
    const int32_t copyWidth = right - left;
    const int32_t copyHeight = bottom - top;
    if (size_t(window.backingWidth) * size_t(window.backingHeight) > window.backingPixels.size()) {
        window.backingValid = false;
        window.backingPixels.clear();
        return false;
    }
    for (int32_t y = 0; y < copyHeight; ++y) {
        const uint32_t* src = window.backingPixels.data() + size_t(y) * size_t(window.backingWidth);
        uint32_t* dst = framebuffer_ + size_t(top + y) * size_t(framebufferWidth_) + size_t(left);
        std::copy(src, src + copyWidth, dst);
    }
    spdlog::info("restored guest window backing hwnd=0x{:08x} rect={},{} {}x{}",
                 hwnd, left, top, copyWidth, copyHeight);
    window.backingValid = false;
    window.backingPixels.clear();
    if (presentRestoredFrame) presentHostWindows(true);
    return true;
}

void SyntheticDllRuntime::eraseGuestWindowArea(uint32_t hwnd, const GuestWindow& window) {
    if (!framebuffer_ || framebufferWidth_ <= 0 || framebufferHeight_ <= 0 ||
        window.width <= 0 || window.height <= 0) {
        return;
    }
    auto mutableWindow = windows_.find(hwnd);
    if (mutableWindow != windows_.end() && restoreGuestWindowBacking(hwnd, mutableWindow->second)) {
        return;
    }
    if (isOwnedPopupWindow(hwnd)) {
        spdlog::info("skipped black erase for owned popup hwnd=0x{:08x} without saved backing", hwnd);
        return;
    }
    if (window.parent) {
        auto parent = windows_.find(window.parent);
        if (parent != windows_.end() && !parent->second.parent) return;
    }
    uint32_t pixel = 0xff000000u;
    if (window.parent) {
        auto parent = windows_.find(window.parent);
        if (parent != windows_.end()) {
            auto cls = windowClassesByName_.find(parent->second.className);
            uint32_t brushHandle = 0;
            if (cls != windowClassesByName_.end()) {
                std::memcpy(&brushHandle, cls->second.bytes.data() + 28, sizeof(brushHandle));
            }
            auto brush = brushes_.find(brushHandle);
            if (brush != brushes_.end()) pixel = colorRefToPixel(brush->second.colorRef);
        }
    }
    if (pixel == 0) pixel = 0xff000000u;
    const auto [x, y] = guestWindowOrigin(hwnd);
    GuestDc screenDc{};
    screenDc.hwnd = 0;
    fillFramebufferRect(screenDc, x, y, x + window.width, y + window.height, pixel);
    presentHostWindows(true);
}

bool SyntheticDllRuntime::isWindowOrDescendant(uint32_t hwnd, uint32_t ancestor) const {
    for (uint32_t current = hwnd; current;) {
        if (current == ancestor) return true;
        auto it = windows_.find(current);
        if (it == windows_.end()) break;
        current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
    }
    return false;
}

bool SyntheticDllRuntime::isOwnedPopupWindow(uint32_t hwnd) const {
    auto it = windows_.find(hwnd);
    return it != windows_.end() && it->second.parent &&
           !(it->second.style & kWindowStyleChild);
}

bool SyntheticDllRuntime::hasCoveringRootPopup(uint32_t hwnd) const {
    auto target = windows_.find(hwnd);
    if (target == windows_.end() || target->second.destroyed || isOwnedPopupWindow(hwnd)) return false;

    uint32_t current = hwnd;
    uint32_t root = hwnd;
    bool nestedChild = false;
    while (current) {
        auto it = windows_.find(current);
        if (it == windows_.end()) break;
        if (it->second.style & kWindowStyleChild) {
            nestedChild = true;
            root = it->second.parent;
            current = it->second.parent;
        } else {
            root = current;
            break;
        }
    }
    auto rootWindow = windows_.find(root);
    if (!nestedChild || rootWindow == windows_.end() || rootWindow->second.destroyed ||
        rootWindow->second.parent) {
        return false;
    }

    if ((target->second.style & kWindowStyleChild) && target->second.parent == root) {
        return false;
    }

    const auto [rootX, rootY] = guestWindowOrigin(root);
    const int32_t rootRight = rootX + rootWindow->second.width;
    const int32_t rootBottom = rootY + rootWindow->second.height;
    for (const auto& [popupHwnd, popup] : windows_) {
        if (popup.zOrder <= target->second.zOrder) continue;
        if (!popup.visible || popup.destroyed || popup.parent != root ||
            (popup.style & kWindowStyleChild)) {
            continue;
        }
        const auto [popupX, popupY] = guestWindowOrigin(popupHwnd);
        if (popupX <= rootX && popupY <= rootY &&
            popupX + popup.width >= rootRight &&
            popupY + popup.height >= rootBottom) {
            return true;
        }
    }
    return false;
}

uint32_t SyntheticDllRuntime::readFramebufferTargetPixel(uint32_t targetHwnd,
                                                         int32_t x,
                                                         int32_t y) const {
    if (!framebuffer_ || x < 0 || y < 0 || x >= framebufferWidth_ || y >= framebufferHeight_) {
        return 0xff000000u;
    }
    if (targetHwnd && isOwnedPopupWindow(targetHwnd) &&
        !guestWindowCoversFramebuffer(targetHwnd) &&
        coveringFullScreenOwnedPopup(targetHwnd)) {
        return framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)];
    }
    if (!isOwnedPopupWindow(targetHwnd)) {
        for (const auto& [hwnd, window] : windows_) {
            if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
                window.backingWidth <= 0 || window.backingHeight <= 0 ||
                isWindowOrDescendant(targetHwnd, hwnd)) {
                continue;
            }
            if (targetHwnd && !isOwnedPopupWindow(hwnd) &&
                window.zOrder < windowZOrder(targetHwnd)) continue;
            if (x < window.backingX || y < window.backingY ||
                x >= window.backingX + window.backingWidth ||
                y >= window.backingY + window.backingHeight) {
                continue;
            }
            const size_t offset = size_t(y - window.backingY) * size_t(window.backingWidth) +
                                  size_t(x - window.backingX);
            if (offset < window.backingPixels.size()) return window.backingPixels[offset];
        }
    }
    return framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)];
}

void SyntheticDllRuntime::writeFramebufferTargetPixel(uint32_t targetHwnd,
                                                      int32_t x,
                                                      int32_t y,
                                                      uint32_t pixel) {
    if (!framebuffer_ || x < 0 || y < 0 || x >= framebufferWidth_ || y >= framebufferHeight_) {
        return;
    }
    if (targetHwnd && isOwnedPopupWindow(targetHwnd) &&
        !guestWindowCoversFramebuffer(targetHwnd) &&
        coveringFullScreenOwnedPopup(targetHwnd)) {
        return;
    }
    bool covered = false;
    if (!isOwnedPopupWindow(targetHwnd)) {
        for (auto& [hwnd, window] : windows_) {
            if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
                window.backingWidth <= 0 || window.backingHeight <= 0 ||
                isWindowOrDescendant(targetHwnd, hwnd)) {
                continue;
            }
            if (targetHwnd && !isOwnedPopupWindow(hwnd) &&
                window.zOrder < windowZOrder(targetHwnd)) continue;
            if (x < window.backingX || y < window.backingY ||
                x >= window.backingX + window.backingWidth ||
                y >= window.backingY + window.backingHeight) {
                continue;
            }
            const size_t offset = size_t(y - window.backingY) * size_t(window.backingWidth) +
                                  size_t(x - window.backingX);
            if (offset < window.backingPixels.size()) {
                window.backingPixels[offset] = pixel;
                covered = true;
            }
        }
    }
    if (!covered) framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)] = pixel;
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
        const bool alreadyQueued = std::any_of(guestMessages_.begin(), guestMessages_.end(),
                                               [&](const GuestMessage& queued) {
                                                   return queued.hwnd == timer.hwnd &&
                                                          queued.message == 0x0113 &&
                                                          queued.wParam == timer.id;
                                               });
        if (alreadyQueued) continue;
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
    auto root = windows_.find(rootGuestHwnd);
    if (root == windows_.end() || root->second.destroyed) rootGuestHwnd = 0;
    uint32_t best = rootGuestHwnd;
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
            current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
        }
        return std::pair<int32_t, int32_t>{ox, oy};
    };
    const std::vector<uint32_t> topToBottom = orderedWindowsTopToBottom();
    for (uint32_t hwnd : topToBottom) {
        const auto it = windows_.find(hwnd);
        if (it == windows_.end()) continue;
        const GuestWindow& window = it->second;
        if (hwnd == rootGuestHwnd || window.destroyed || !window.visible ||
            !window.enabled || window.parent || !(window.style & 0x80000000u)) {
            continue;
        }
        const auto [ox, oy] = originOf(hwnd);
        rootGuestHwnd = hwnd;
        best = hwnd;
        bestX = ox;
        bestY = oy;
        const int32_t right = ox + window.width;
        const int32_t bottom = oy + window.height;
        if (x < ox || y < oy || x >= right || y >= bottom) {
            clientX = x - ox;
            clientY = y - oy;
            return hwnd;
        }
        break;
    }
    uint32_t modalPopup = 0;
    int32_t modalX = 0;
    int32_t modalY = 0;
    for (uint32_t hwnd : topToBottom) {
        const auto it = windows_.find(hwnd);
        if (it == windows_.end()) continue;
        const GuestWindow& window = it->second;
        if (window.destroyed || !window.visible || !window.enabled || !belongsToRoot(hwnd)) continue;
        if (!isOwnedPopupWindow(hwnd) || guestWindowCoversFramebuffer(hwnd)) continue;
        if (hasCoveringRootPopup(hwnd)) continue;
        const auto [ox, oy] = originOf(hwnd);
        modalPopup = hwnd;
        modalX = ox;
        modalY = oy;
        break;
    }
    for (uint32_t hwnd : topToBottom) {
        const auto it = windows_.find(hwnd);
        if (it == windows_.end()) continue;
        const GuestWindow& window = it->second;
        if (window.destroyed || !window.visible || !window.enabled || !belongsToRoot(hwnd)) continue;
        if (hasCoveringRootPopup(hwnd)) continue;
        const auto [ox, oy] = originOf(hwnd);
        int32_t left = ox;
        int32_t top = oy;
        int32_t right = ox + window.width;
        int32_t bottom = oy + window.height;
        if ((window.width <= 0 || window.height <= 0) && window.paintBoundsValid) {
            left = window.paintLeft;
            top = window.paintTop;
            right = window.paintRight;
            bottom = window.paintBottom;
        }
        if (x < left || y < top || x >= right || y >= bottom) continue;
        best = hwnd;
        bestX = ox;
        bestY = oy;
        break;
    }
    if (modalPopup && !isWindowInOwnedPopupStack(best, modalPopup)) {
        best = modalPopup;
        bestX = modalX;
        bestY = modalY;
    }
    clientX = x - bestX;
    clientY = y - bestY;
    return best;
}

void SyntheticDllRuntime::compactQueuedPointerMotion(size_t maxMotionPerWindow) {
    std::map<uint32_t, size_t> keptMotion;
    for (auto it = guestMessages_.rbegin(); it != guestMessages_.rend();) {
        if (it->message == 0x0200) {
            size_t& kept = keptMotion[it->hwnd];
            if (kept >= maxMotionPerWindow) {
                auto eraseIt = std::next(it).base();
                it = std::make_reverse_iterator(guestMessages_.erase(eraseIt));
                continue;
            }
            ++kept;
        }
        ++it;
    }
}

void SyntheticDllRuntime::queueHostMouseMessage(uint32_t rootGuestHwnd, uint32_t message,
                                                int32_t hostX, int32_t hostY) {
    auto root = windows_.find(rootGuestHwnd);
    if (root == windows_.end() || root->second.destroyed) {
        auto presenterRoot = windows_.find(hostPresenterGuestHwnd_);
        if (presenterRoot != windows_.end() && !presenterRoot->second.destroyed) {
            spdlog::info("rebased host mouse root from destroyed guest HWND=0x{:08x} to presenter guest HWND=0x{:08x}",
                         rootGuestHwnd, hostPresenterGuestHwnd_);
            rootGuestHwnd = hostPresenterGuestHwnd_;
        } else {
            rootGuestHwnd = 0;
        }
    }
    int32_t clientX = hostX;
    int32_t clientY = hostY;
    uint32_t hwnd = 0;
    auto ownedPopupRootForInput = [&](uint32_t target) {
        for (uint32_t current = target; current;) {
            if (isOwnedPopupWindow(current)) return current;
            auto window = windows_.find(current);
            if (window == windows_.end()) break;
            current = window->second.parent;
        }
        return 0u;
    };
    auto canUseCapturedWindow = [&](uint32_t capturedHwnd) {
        const uint32_t popupRoot = ownedPopupRootForInput(capturedHwnd);
        bool abovePopupRoot = false;
        for (uint32_t current = capturedHwnd; current;) {
            auto window = windows_.find(current);
            if (window == windows_.end() || window->second.destroyed ||
                !window->second.visible || (!window->second.enabled && !abovePopupRoot)) {
                return false;
            }
            if (current == popupRoot) abovePopupRoot = true;
            current = window->second.parent;
        }
        return true;
    };
    if (message == 0x0202 && hostPointerCaptureWindow_) {
        auto captured = windows_.find(hostPointerCaptureWindow_);
        if (captured != windows_.end() && canUseCapturedWindow(hostPointerCaptureWindow_)) {
            hwnd = hostPointerCaptureWindow_;
        } else {
            hostPointerCaptureWindow_ = 0;
        }
    } else if (message != 0x0201 && capturedWindow_) {
        auto captured = windows_.find(capturedWindow_);
        if (captured != windows_.end() && canUseCapturedWindow(capturedWindow_)) {
            hwnd = capturedWindow_;
        } else {
            capturedWindow_ = 0;
        }
    }
    if (!hwnd) {
        hwnd = windowAtPoint(rootGuestHwnd, hostX, hostY, clientX, clientY);
    }
    if (!hwnd) return;

    auto targetWindow = windows_.find(hwnd);
    if (targetWindow != windows_.end() && !targetWindow->second.parent &&
        hwnd != rootGuestHwnd && (targetWindow->second.style & 0x80000000u) &&
        (clientX < 0 || clientY < 0 || clientX >= targetWindow->second.width ||
         clientY >= targetWindow->second.height)) {
        if (message == 0x0202 && hostPointerCaptureWindow_ == hwnd) {
            hostPointerCaptureWindow_ = 0;
        }
        spdlog::info("discarded host mouse msg=0x{:04x} outside modal popup hwnd=0x{:08x} point={},{} client={},{}",
                     message, hwnd, hostX, hostY, clientX, clientY);
        return;
    }

    const uint32_t popupRoot = ownedPopupRootForInput(hwnd);
    bool abovePopupRoot = false;
    for (uint32_t current = hwnd; current;) {
        auto window = windows_.find(current);
        if (window == windows_.end()) break;
        if (!window->second.enabled && !abovePopupRoot) {
            if (message == 0x0202 && hostPointerCaptureWindow_ == hwnd) {
                hostPointerCaptureWindow_ = 0;
            }
            spdlog::info("discarded host mouse msg=0x{:04x} hwnd=0x{:08x} disabledAt=0x{:08x} point={},{}",
                         message, hwnd, current, hostX, hostY);
            return;
        }
        if (current == popupRoot) abovePopupRoot = true;
        current = window->second.parent;
    }

    auto originOf = [&](uint32_t target) {
        int32_t x = 0;
        int32_t y = 0;
        for (uint32_t current = target; current;) {
            auto it = windows_.find(current);
            if (it == windows_.end()) break;
            x += it->second.x;
            y += it->second.y;
            current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
        }
        return std::pair<int32_t, int32_t>{x, y};
    };
    const auto [originX, originY] = originOf(hwnd);
    clientX = hostX - originX;
    clientY = hostY - originY;
    auto queueInputMessage = [&](const GuestMessage& input) {
        const auto isInputPriority = [](const GuestMessage& queued) {
            return queued.message == 0x0007 || queued.message == 0x0008 ||
                   queued.message == 0x0200 || queued.message == 0x0201 ||
                   queued.message == 0x0202;
        };
        auto insertAt = guestMessages_.begin();
        while (insertAt != guestMessages_.end() && isInputPriority(*insertAt)) {
            ++insertAt;
        }
        guestMessages_.insert(insertAt, input);
        compactQueuedPointerMotion();
    };
    if (message == 0x0201) {
        hostPointerCaptureWindow_ = hwnd;
        if (focusedWindow_ != hwnd) {
            auto focused = windows_.find(focusedWindow_);
            if (focused != windows_.end() && !focused->second.destroyed) {
                queueInputMessage({focusedWindow_, 0x0008, hwnd, 0, uint32_t(++tick_ * 16), 0, 0});
            }
            queueInputMessage({hwnd, 0x0007, focusedWindow_, 0, uint32_t(++tick_ * 16), 0, 0});
            focusedWindow_ = hwnd;
        }
    } else if (message == 0x0202) {
        if (pendingSyntheticChildButtonUpWindow_) {
            const uint32_t childHwnd = pendingSyntheticChildButtonUpWindow_;
            pendingSyntheticChildButtonUpWindow_ = 0;
            auto child = windows_.find(childHwnd);
            if (child != windows_.end() && !child->second.destroyed) {
                GuestMessage childUp{};
                childUp.hwnd = childHwnd;
                childUp.message = 0x0202; // WM_LBUTTONUP
                childUp.wParam = 0;
                childUp.lParam = 0;
                childUp.time = uint32_t(++tick_ * 16);
                childUp.x = uint32_t(hostX);
                childUp.y = uint32_t(hostY);
                queueInputMessage(childUp);
                spdlog::info("queued mirrored child button-up hwnd=0x{:08x} for host release at {},{}",
                             childHwnd, hostX, hostY);
            }
        }
        hostPointerCaptureWindow_ = 0;
    }
    GuestMessage guest{};
    guest.hwnd = hwnd;
    guest.message = message;
    guest.wParam = message == 0x0201 ? 1 : 0;
    guest.lParam = uint32_t(uint16_t(clientX) | (uint32_t(uint16_t(clientY)) << 16));
    guest.time = uint32_t(++tick_ * 16);
    // MSG.pt/GetMessagePos are screen/root coordinates; lParam remains client.
    guest.x = uint32_t(hostX);
    guest.y = uint32_t(hostY);
    lastHostInputQueuedAt_ = std::chrono::steady_clock::now();
    queueInputMessage(guest);
    spdlog::info("queued host mouse msg=0x{:04x} root=0x{:08x} hwnd=0x{:08x} point={},{} client={},{} queued={}",
                 message, rootGuestHwnd, hwnd, hostX, hostY, clientX, clientY, guestMessages_.size());
    uc_emu_stop(uc_);
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
    captureGuestWindowBacking(dc.hwnd);
    int32_t originX = 0;
    int32_t originY = 0;
    if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
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
    noteGuestWindowPaint(dc.hwnd, left, top, right, bottom);

    if (dc.hwnd && isOwnedPopupWindow(dc.hwnd) &&
        !guestWindowCoversFramebuffer(dc.hwnd) &&
        coveringFullScreenOwnedPopup(dc.hwnd)) {
        return;
    }

    bool needsLayeredWrite = false;
    if (!isOwnedPopupWindow(dc.hwnd)) {
        for (const auto& [hwnd, window] : windows_) {
            if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
                window.backingWidth <= 0 || window.backingHeight <= 0 ||
                isWindowOrDescendant(dc.hwnd, hwnd)) {
                continue;
            }
            if (dc.hwnd && !isOwnedPopupWindow(hwnd) &&
                window.zOrder < windowZOrder(dc.hwnd)) continue;
            const int32_t backingRight = window.backingX + window.backingWidth;
            const int32_t backingBottom = window.backingY + window.backingHeight;
            if (left < backingRight && right > window.backingX &&
                top < backingBottom && bottom > window.backingY) {
                needsLayeredWrite = true;
                break;
            }
        }
    }

    if (!needsLayeredWrite) {
        for (int32_t y = top; y < bottom; ++y) {
            uint32_t* row = framebuffer_ + size_t(y) * size_t(framebufferWidth_) + size_t(left);
            std::fill(row, row + (right - left), pixel);
        }
    } else {
        for (int32_t y = top; y < bottom; ++y) {
            for (int32_t x = left; x < right; ++x) {
                writeFramebufferTargetPixel(dc.hwnd, x, y, pixel);
            }
        }
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
    captureGuestWindowBacking(dc.hwnd);
    int32_t originX = 0;
    int32_t originY = 0;
    if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
    x0 += originX;
    x1 += originX;
    y0 += originY;
    y1 += originY;
    noteGuestWindowPaint(dc.hwnd,
                         std::clamp<int32_t>(std::min(x0, x1), 0, framebufferWidth_),
                         std::clamp<int32_t>(std::min(y0, y1), 0, framebufferHeight_),
                         std::clamp<int32_t>(std::max(x0, x1) + 1, 0, framebufferWidth_),
                         std::clamp<int32_t>(std::max(y0, y1) + 1, 0, framebufferHeight_));
    const int32_t dx = std::abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < framebufferWidth_ && y0 >= 0 && y0 < framebufferHeight_) {
            writeFramebufferTargetPixel(dc.hwnd, x0, y0, pixel);
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

bool SyntheticDllRuntime::fillBitmapRect(const GuestBitmap& bitmap,
                                         int32_t left,
                                         int32_t top,
                                         int32_t right,
                                         int32_t bottom,
                                         uint32_t pixel) {
    const int32_t height = std::abs(bitmap.heightRaw);
    if (!bitmap.bits || bitmap.width <= 0 || height <= 0 || bitmap.stride == 0 || pixel == 0) return false;
    const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
    if (!byteCount || byteCount > 0x2000000ull) return false;
    std::vector<uint8_t> raw(static_cast<size_t>(byteCount));
    if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK) return false;
    if (left > right) std::swap(left, right);
    if (top > bottom) std::swap(top, bottom);
    left = std::clamp<int32_t>(left, 0, bitmap.width);
    right = std::clamp<int32_t>(right, 0, bitmap.width);
    top = std::clamp<int32_t>(top, 0, height);
    bottom = std::clamp<int32_t>(bottom, 0, height);
    for (int32_t y = top; y < bottom; ++y) {
        for (int32_t x = left; x < right; ++x) {
            writeBitmapPixel(bitmap, raw, height, x, y, pixel);
        }
    }
    return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::drawBitmapLine(const GuestBitmap& bitmap,
                                         int32_t x0,
                                         int32_t y0,
                                         int32_t x1,
                                         int32_t y1,
                                         uint32_t pixel) {
    const int32_t height = std::abs(bitmap.heightRaw);
    if (!bitmap.bits || bitmap.width <= 0 || height <= 0 || bitmap.stride == 0 || pixel == 0) return false;
    const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(height);
    if (!byteCount || byteCount > 0x2000000ull) return false;
    std::vector<uint8_t> raw(static_cast<size_t>(byteCount));
    if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK) return false;
    const int32_t dx = std::abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        if (x0 >= 0 && x0 < bitmap.width && y0 >= 0 && y0 < height) {
            writeBitmapPixel(bitmap, raw, height, x0, y0, pixel);
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
    return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::fillDcRect(const GuestDc& dc,
                                     int32_t left,
                                     int32_t top,
                                     int32_t right,
                                     int32_t bottom,
                                     uint32_t pixel) {
    auto bitmap = bitmaps_.find(dc.selectedBitmap);
    if (bitmap != bitmaps_.end()) return fillBitmapRect(bitmap->second, left, top, right, bottom, pixel);
    fillFramebufferRect(dc, left, top, right, bottom, pixel);
    return true;
}

bool SyntheticDllRuntime::drawDcLine(const GuestDc& dc,
                                     int32_t x0,
                                     int32_t y0,
                                     int32_t x1,
                                     int32_t y1,
                                     uint32_t pixel) {
    auto bitmap = bitmaps_.find(dc.selectedBitmap);
    if (bitmap != bitmaps_.end()) return drawBitmapLine(bitmap->second, x0, y0, x1, y1, pixel);
    drawFramebufferLine(dc, x0, y0, x1, y1, pixel);
    return true;
}

bool SyntheticDllRuntime::fillDcPolygon(const GuestDc& dc,
                                        const std::vector<std::pair<int32_t, int32_t>>& points,
                                        uint32_t pixel) {
    if (points.size() < 3 || pixel == 0) return false;

    std::vector<std::pair<int32_t, int32_t>> translatedPoints = points;
    auto bitmapIt = bitmaps_.find(dc.selectedBitmap);
    if (bitmapIt != bitmaps_.end()) {
#if defined(_WIN32)
        const GuestBitmap& bitmap = bitmapIt->second;
        const int32_t bitmapHeight = std::abs(bitmap.heightRaw);
        if (!bitmap.bits || bitmap.width <= 0 || bitmapHeight <= 0 || bitmap.stride == 0) return false;

        int32_t minX = points.front().first;
        int32_t maxX = points.front().first;
        int32_t minY = points.front().second;
        int32_t maxY = points.front().second;
        for (const auto& point : points) {
            minX = std::min(minX, point.first);
            maxX = std::max(maxX, point.first);
            minY = std::min(minY, point.second);
            maxY = std::max(maxY, point.second);
        }
        const int32_t clipLeft = std::clamp<int32_t>(minX, 0, bitmap.width - 1);
        const int32_t clipTop = std::clamp<int32_t>(minY, 0, bitmapHeight - 1);
        const int32_t clipRight = std::clamp<int32_t>(maxX, 0, bitmap.width - 1);
        const int32_t clipBottom = std::clamp<int32_t>(maxY, 0, bitmapHeight - 1);
        if (clipRight < clipLeft || clipBottom < clipTop) return true;

        const int32_t dibWidth = clipRight - clipLeft + 1;
        const int32_t dibHeight = clipBottom - clipTop + 1;
        const uint64_t bitmapByteCount = uint64_t(bitmap.stride) * uint64_t(bitmapHeight);
        const uint64_t dibByteCount = uint64_t(dibWidth) * uint64_t(dibHeight) * 4ull;
        if (!bitmapByteCount || bitmapByteCount > 0x2000000ull ||
            !dibByteCount || dibByteCount > 0x2000000ull) {
            return false;
        }

        std::vector<uint8_t> raw(static_cast<size_t>(bitmapByteCount));
        if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK) return false;

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = dibWidth;
        info.bmiHeader.biHeight = -dibHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* dibBits = nullptr;
        HDC hdc = CreateCompatibleDC(nullptr);
        HBITMAP dib = hdc ? CreateDIBSection(hdc, &info, DIB_RGB_COLORS, &dibBits, nullptr, 0) : nullptr;
        if (!hdc || !dib || !dibBits) {
            if (dib) DeleteObject(dib);
            if (hdc) DeleteDC(hdc);
            return false;
        }

        auto* pixels = static_cast<uint8_t*>(dibBits);
        for (int32_t y = 0; y < dibHeight; ++y) {
            for (int32_t x = 0; x < dibWidth; ++x) {
                uint32_t current = 0;
                readBitmapPixel(bitmap, raw, bitmapHeight, clipLeft + x, clipTop + y, current);
                const size_t offset = (size_t(y) * size_t(dibWidth) + size_t(x)) * 4;
                pixels[offset + 0] = uint8_t(current & 0xff);
                pixels[offset + 1] = uint8_t((current >> 8) & 0xff);
                pixels[offset + 2] = uint8_t((current >> 16) & 0xff);
                pixels[offset + 3] = 0xff;
            }
        }

        HGDIOBJ oldBitmap = SelectObject(hdc, dib);
        HBRUSH brush = CreateSolidBrush(RGB((pixel >> 16) & 0xff, (pixel >> 8) & 0xff, pixel & 0xff));
        HGDIOBJ oldBrush = SelectObject(hdc, brush);
        HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
        SetPolyFillMode(hdc, ALTERNATE);

        std::vector<POINT> gdiPoints;
        gdiPoints.reserve(points.size());
        for (const auto& point : points) {
            POINT p{};
            p.x = point.first - clipLeft;
            p.y = point.second - clipTop;
            gdiPoints.push_back(p);
        }
        const BOOL drew = Polygon(hdc, gdiPoints.data(), int(gdiPoints.size()));

        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldBitmap);
        DeleteObject(brush);

        if (drew) {
            GdiFlush();
            for (int32_t y = 0; y < dibHeight; ++y) {
                for (int32_t x = 0; x < dibWidth; ++x) {
                    const size_t offset = (size_t(y) * size_t(dibWidth) + size_t(x)) * 4;
                    const uint32_t outPixel = 0xff000000u |
                        (uint32_t(pixels[offset + 2]) << 16) |
                        (uint32_t(pixels[offset + 1]) << 8) |
                        uint32_t(pixels[offset + 0]);
                    writeBitmapPixel(bitmap, raw, bitmapHeight, clipLeft + x, clipTop + y, outPixel);
                }
            }
        }

        DeleteObject(dib);
        DeleteDC(hdc);
        if (!drew) return false;
        return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
#endif
    }
    if (bitmapIt == bitmaps_.end()) {
        int32_t originX = 0;
        int32_t originY = 0;
        if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
        for (auto& point : translatedPoints) {
            point.first += originX;
            point.second += originY;
        }
        captureGuestWindowBacking(dc.hwnd);
    }

    int32_t minY = translatedPoints.front().second;
    int32_t maxY = translatedPoints.front().second;
    int32_t minX = translatedPoints.front().first;
    int32_t maxX = translatedPoints.front().first;
    for (const auto& point : translatedPoints) {
        minX = std::min(minX, point.first);
        maxX = std::max(maxX, point.first);
        minY = std::min(minY, point.second);
        maxY = std::max(maxY, point.second);
    }

    std::vector<uint8_t> raw;
    int32_t bitmapHeight = 0;
    if (bitmapIt != bitmaps_.end()) {
        const GuestBitmap& bitmap = bitmapIt->second;
        bitmapHeight = std::abs(bitmap.heightRaw);
        if (!bitmap.bits || bitmap.width <= 0 || bitmapHeight <= 0 || bitmap.stride == 0) return false;
        const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(bitmapHeight);
        if (!byteCount || byteCount > 0x2000000ull) return false;
        raw.resize(static_cast<size_t>(byteCount));
        if (uc_mem_read(uc_, bitmap.bits, raw.data(), raw.size()) != UC_ERR_OK) return false;
        minY = std::clamp<int32_t>(minY, 0, bitmapHeight - 1);
        maxY = std::clamp<int32_t>(maxY, 0, bitmapHeight - 1);
    } else {
        if (!framebuffer_) return false;
        noteGuestWindowPaint(dc.hwnd,
                             std::clamp<int32_t>(minX, 0, framebufferWidth_),
                             std::clamp<int32_t>(minY, 0, framebufferHeight_),
                             std::clamp<int32_t>(maxX + 1, 0, framebufferWidth_),
                             std::clamp<int32_t>(maxY + 1, 0, framebufferHeight_));
        minY = std::clamp<int32_t>(minY, 0, framebufferHeight_ - 1);
        maxY = std::clamp<int32_t>(maxY, 0, framebufferHeight_ - 1);
    }

    std::vector<int32_t> intersections;
    intersections.reserve(points.size());
    for (int32_t y = minY; y <= maxY; ++y) {
        intersections.clear();
        const double scanY = double(y) + 0.5;
        for (size_t i = 0; i < translatedPoints.size(); ++i) {
            const auto& p0 = translatedPoints[i];
            const auto& p1 = translatedPoints[(i + 1) % translatedPoints.size()];
            if (p0.second == p1.second) continue;
            const int32_t edgeMinY = std::min(p0.second, p1.second);
            const int32_t edgeMaxY = std::max(p0.second, p1.second);
            if (scanY < double(edgeMinY) || scanY >= double(edgeMaxY)) continue;
            const double t = (scanY - double(p0.second)) / double(p1.second - p0.second);
            intersections.push_back(int32_t(std::floor(double(p0.first) + t * double(p1.first))));
        }
        std::sort(intersections.begin(), intersections.end());
        for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
            int32_t left = intersections[i];
            int32_t right = intersections[i + 1];
            if (left > right) std::swap(left, right);
            if (bitmapIt != bitmaps_.end()) {
                const GuestBitmap& bitmap = bitmapIt->second;
                left = std::clamp<int32_t>(left, 0, bitmap.width - 1);
                right = std::clamp<int32_t>(right, 0, bitmap.width - 1);
                for (int32_t x = left; x <= right; ++x) {
                    writeBitmapPixel(bitmap, raw, bitmapHeight, x, y, pixel);
                }
            } else {
                left = std::clamp<int32_t>(left, 0, framebufferWidth_ - 1);
                right = std::clamp<int32_t>(right, 0, framebufferWidth_ - 1);
                for (int32_t x = left; x <= right; ++x) {
                    writeFramebufferTargetPixel(dc.hwnd, x, y, pixel);
                }
            }
        }
    }

    if (bitmapIt != bitmaps_.end()) {
        const GuestBitmap& bitmap = bitmapIt->second;
        return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
    }
    invalidateHostWindows();
    return true;
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
        if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
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
    captureGuestWindowBacking(dc.hwnd);
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

SyntheticDllRuntime::BitmapProbeStats
SyntheticDllRuntime::bitmapProbeStats(const GuestBitmap& bitmap,
                                      int32_t x,
                                      int32_t y,
                                      int32_t width,
                                      int32_t height) const {
    BitmapProbeStats stats{};
    const int32_t bitmapHeight = std::abs(bitmap.heightRaw);
    if (bitmap.width <= 0 || bitmapHeight <= 0 || !bitmap.bits || !bitmap.stride ||
        width == 0 || height == 0) {
        return stats;
    }
    const uint64_t byteCount = uint64_t(bitmap.stride) * uint64_t(bitmapHeight);
    if (!byteCount || byteCount > 0x2000000ull) return stats;

    std::vector<uint8_t> bits(static_cast<size_t>(byteCount));
    if (uc_mem_read(uc_, bitmap.bits, bits.data(), bits.size()) != UC_ERR_OK) return stats;

    const int32_t sampleW = std::abs(width);
    const int32_t sampleH = std::abs(height);
    const int32_t left = width < 0 ? x + width : x;
    const int32_t top = height < 0 ? y + height : y;
    const int32_t stepX = std::max<int32_t>(1, sampleW / 32);
    const int32_t stepY = std::max<int32_t>(1, sampleH / 32);
    std::array<uint32_t, 16> uniquePixels{};
    uint32_t uniqueCount = 0;

    for (int32_t py = 0; py < sampleH; py += stepY) {
        const int32_t sy = top + py;
        for (int32_t px = 0; px < sampleW; px += stepX) {
            const int32_t sx = left + px;
            uint32_t pixel = 0;
            if (!readBitmapPixel(bitmap, bits, bitmapHeight, sx, sy, pixel)) continue;
            if (!stats.sampled) stats.firstPixel = pixel;
            stats.lastPixel = pixel;
            ++stats.sampled;
            if ((pixel & 0x00ffffffu) != 0) ++stats.nonBlack;
            bool seen = false;
            for (uint32_t i = 0; i < uniqueCount; ++i) {
                if ((uniquePixels[i] & 0x00ffffffu) == (pixel & 0x00ffffffu)) {
                    seen = true;
                    break;
                }
            }
            if (!seen && uniqueCount < uniquePixels.size()) uniquePixels[uniqueCount++] = pixel;
        }
    }
    stats.uniqueApprox = uniqueCount;
    return stats;
}

void SyntheticDllRuntime::dumpGuestBitmapPpm(uint32_t bitmapHandle,
                                             const GuestBitmap& bitmap,
                                             const std::string& tag) {
    if (!diagnosticDumpsEnabled_) return;
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
    spdlog::debug("dumped splash source bitmap handle=0x{:08x} tag={} file={}",
                  bitmapHandle, tag, path);
}

void SyntheticDllRuntime::dumpFramebufferPpm(const std::string& tag) {
    if (!diagnosticDumpsEnabled_) return;
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
    spdlog::debug("dumped splash framebuffer tag={} file={}", tag, path);
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
        if (std::abs(dstW) >= 200 || std::abs(scanLines) >= 120 || dc->hwnd) {
            spdlog::info("SetDIBitsToDevice ok dst=0x{:08x} hwnd=0x{:08x} dstBitmap=0x{:08x} dst={},{} {}x{} srcOrigin={},{} startScan={} scanLines={} bits=0x{:08x} info=0x{:08x}",
                         args.a0, dc->hwnd, dc->selectedBitmap, int32_t(args.a1), int32_t(args.a2),
                         dstW, dstH, srcX, int32_t(stackArg(6)), stackArg(7), stackArg(8),
                         stackArg(9), stackArg(10));
        }
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
    captureGuestWindowBacking(dc.hwnd);

    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    int32_t originX = 0;
    int32_t originY = 0;
    if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
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
            writeFramebufferTargetPixel(dc.hwnd, dstPx, dstPy, pixel);
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
    captureGuestWindowBacking(dstDc.hwnd);

    int32_t originX = 0;
    int32_t originY = 0;
    if (dstDc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dstDc.hwnd);
    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    outLeft += originX;
    outTop += originY;
    noteGuestWindowPaint(dstDc.hwnd, outLeft, outTop, outLeft + outW, outTop + outH);

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
            const uint32_t outPixel = rop == 0x00cc0020u
                ? pixel
                : applySourceRasterOp(rop, pixel, readFramebufferTargetPixel(dstDc.hwnd, dstPx, dstPy));
            writeFramebufferTargetPixel(dstDc.hwnd, dstPx, dstPy, outPixel);
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
    captureGuestWindowBacking(dstDc.hwnd);

    int32_t originX = 0;
    int32_t originY = 0;
    if (dstDc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dstDc.hwnd);

    const uint32_t transparentPixel = colorRefToPixel(transparentColor) & 0x00ffffffu;
    const int32_t outW = std::abs(dstW);
    const int32_t outH = std::abs(dstH);
    int32_t outLeft = dstW < 0 ? dstX + dstW : dstX;
    int32_t outTop = dstH < 0 ? dstY + dstH : dstY;
    outLeft += originX;
    outTop += originY;
    noteGuestWindowPaint(dstDc.hwnd, outLeft, outTop, outLeft + outW, outTop + outH);

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
                writeFramebufferTargetPixel(dstDc.hwnd, dstPx, dstPy, pixel);
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

    while (!normalized.empty() && (normalized.front() == '\\' || normalized.front() == '/')) {
        normalized.erase(normalized.begin());
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
        while (!sdmmcRelative.empty() && sdmmcRelative.front() == '\\') sdmmcRelative.erase(sdmmcRelative.begin());
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

    auto finishImmediateReturn = [&](uint32_t value, bool cooperateThreads = false) {
        if (mutableEntry.calls <= 128) {
            spdlog::debug("synthetic {}!{} -> 0x{:08x}", mutableEntry.moduleName, name, value);
        }
        setReg(UC_MIPS_REG_V0, value);
        setReg(UC_MIPS_REG_PC, ra);
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
        return wndProc;
    };
    auto finalizeDestroyedWindow = [&](uint32_t hwnd,
                                       std::optional<bool> visibleOverride = std::nullopt,
                                       std::optional<uint32_t> parentOverride = std::nullopt) {
        auto it = windows_.find(hwnd);
        if (it == windows_.end()) return;
        const bool wasVisible = visibleOverride.value_or(it->second.visible);
        const uint32_t parent = parentOverride.value_or(it->second.parent);
        const bool exposesCoveredWindows =
            wasVisible && isOwnedPopupWindow(hwnd) && guestWindowCoversFramebuffer(hwnd);
        for (auto timer = timers_.begin(); timer != timers_.end();) {
            if (timer->second.hwnd == hwnd) timer = timers_.erase(timer);
            else ++timer;
        }
        if (focusedWindow_ == hwnd) focusedWindow_ = 0;
        if (capturedWindow_ == hwnd) capturedWindow_ = 0;
        if (hostPointerCaptureWindow_ == hwnd) hostPointerCaptureWindow_ = 0;
        if (pendingSyntheticChildButtonUpWindow_ == hwnd) pendingSyntheticChildButtonUpWindow_ = 0;
        if (wasVisible) eraseGuestWindowArea(hwnd, it->second);
        it->second.visible = false;
        it->second.destroyed = true;
        destroyHostWindow(it->second);
        if (wasVisible && parent) {
            spdlog::info("DestroyWindow invalidating parent=0x{:08x} after child=0x{:08x}", parent, hwnd);
            queueGuestPaint(parent, true);
        }
        if (exposesCoveredWindows) {
            size_t exposed = 0;
            for (const auto& [otherHwnd, window] : windows_) {
                if (otherHwnd == hwnd || window.destroyed || !window.visible) continue;
                queueGuestPaint(otherHwnd, true);
                ++exposed;
            }
            spdlog::info("DestroyWindow exposed full-screen popup hwnd=0x{:08x}; queued repaint for {} visible windows",
                         hwnd, exposed);
        }
    };
    auto dispatchQueuedPaintForBlockingApi = [&](PendingBlockingApi& pending, const char* reason) {
        // Only dispatch paint/timer maintenance from a synthetic blocking wait
        // boundary.  Posted private messages and pointer input must stay in the
        // normal GetMessage/DispatchMessage path; reentering them from an
        // unrelated Sleep/Wait hook can leave MFC/iNavi state transitions
        // half-applied, which shows up as buttons painting pressed but not
        // switching views.
        if (pending.paintDispatches >= 64) return false;
        auto isPaintish = [](uint32_t msg) {
            return msg == 0x000f || // WM_PAINT
                   msg == 0x0014 || // WM_ERASEBKGND
                   msg == 0x0018 || // WM_SHOWWINDOW
                   msg == 0x0113;   // WM_TIMER
        };
        auto queued = std::find_if(guestMessages_.begin(), guestMessages_.end(),
                                   [&](const GuestMessage& message) {
                                       if (!isPaintish(message.message)) return false;
                                       auto window = windows_.find(message.hwnd);
                                       if (window == windows_.end() || window->second.destroyed || !window->second.wndProc) {
                                           return false;
                                       }
                                       const bool paintMessage = message.message == 0x0014 || message.message == 0x000f;
                                       return !paintMessage || !hasCoveringRootPopup(message.hwnd);
                                   });
        if (queued == guestMessages_.end()) return false;
        const GuestMessage message = *queued;
        guestMessages_.erase(queued);
        auto window = windows_.find(message.hwnd);
        uint32_t wndProc = translatedWndProc(window->second.wndProc, pending.name.c_str());
        ++pending.paintDispatches;
        spdlog::debug("{} cooperative paint dispatch {} hwnd=0x{:08x} msg=0x{:08x} wndproc=0x{:08x} count={}",
                      pending.name, reason, message.hwnd, message.message, wndProc, pending.paintDispatches);
        setReg(UC_MIPS_REG_A0, message.hwnd);
        setReg(UC_MIPS_REG_A1, message.message);
        setReg(UC_MIPS_REG_A2, message.wParam);
        setReg(UC_MIPS_REG_A3, message.lParam);
        setReg(UC_MIPS_REG_RA, blockingApiContinuationStub_);
        setReg(UC_MIPS_REG_PC, wndProc);
        return true;
    };

    if (isCoredll && pc == destroyWindowContinuationStub_) {
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
        const uint32_t parent = pending.parent;
        const bool wasVisible = pending.wasVisible;
        pendingDestroyWindows_.pop_back();
        finalizeDestroyedWindow(hwnd, wasVisible, parent);
        lastError_ = 0;
        setReg(UC_MIPS_REG_V0, 1);
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_PC, originalRa);
        spdlog::info("DestroyWindow synchronous destroy complete hwnd=0x{:08x} return=0x{:08x}", hwnd, originalRa);
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == createWindowContinuationStub_) {
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
        auto completedWindow = windows_.find(hwnd);
        if (wndProcResult == 0xffffffffu ||
            completedWindow == windows_.end() ||
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
        setReg(UC_MIPS_REG_PC, originalRa);
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == blockingApiContinuationStub_) {
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
        if (pending.ordinal == 0x01F0) {
            handleSleep(SyntheticExportCode::CoreDllSleep, pending.args, ret);
        } else if (!dispatchHostWin32(pending.ordinal, pending.args, ret)) {
            spdlog::warn("blocking API continuation could not resume {}", pending.name);
            lastError_ = 120;
            ret = 0;
        }
        setReg(UC_MIPS_REG_V0, ret);
        setReg(UC_MIPS_REG_RA, pending.args.ra);
        setReg(UC_MIPS_REG_PC, pending.args.ra);
        spdlog::info("{} resumed after cooperative paint -> 0x{:08x}", pending.name, ret);
        pumpHostMessages();
        cooperateGuestThreadsAfterCall(pending.name);
        return;
    }

    if (isCoredll && pc == updateWindowContinuationStub_) {
        if (pendingUpdateWindows_.empty()) {
            spdlog::warn("UpdateWindow continuation reached with no pending window");
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_PC, ra);
            return;
        }
        auto& pending = pendingUpdateWindows_.back();
        auto window = windows_.find(pending.hwnd);
        const std::string sourceName = pending.sourceName.empty() ? "UpdateWindow" : pending.sourceName;
        uint32_t wndProc = pending.wndProc;
        if (window == windows_.end() || window->second.destroyed || !window->second.visible) {
            const uint32_t originalRa = pending.originalRa;
            const uint32_t hwnd = pending.hwnd;
            pendingUpdateWindows_.pop_back();
            lastError_ = 0;
            setReg(UC_MIPS_REG_V0, 1);
            setReg(UC_MIPS_REG_RA, originalRa);
            setReg(UC_MIPS_REG_PC, originalRa);
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
            setReg(UC_MIPS_REG_RA, updateWindowContinuationStub_);
            setReg(UC_MIPS_REG_PC, wndProc);
            return;
        }
        const uint32_t originalRa = pending.originalRa;
        const uint32_t hwnd = pending.hwnd;
        pendingUpdateWindows_.pop_back();
        lastError_ = 0;
        setReg(UC_MIPS_REG_V0, 1);
        setReg(UC_MIPS_REG_RA, originalRa);
        setReg(UC_MIPS_REG_PC, originalRa);
        spdlog::info("{} synchronous paint complete hwnd=0x{:08x} return=0x{:08x}",
                     sourceName, hwnd, originalRa);
        queueVisiblePopupPaintsAbove(hwnd);
        presentHostWindows(true);
        pumpHostMessages();
        return;
    }

    if (isCoredll && pc == messageTransferContinuationStub_) {
        if (pendingMessageTransfers_.empty()) {
            spdlog::warn("message transfer continuation reached with no pending message");
            setReg(UC_MIPS_REG_PC, ra);
            return;
        }
        PendingMessageTransfer pending = pendingMessageTransfers_.back();
        pendingMessageTransfers_.pop_back();
        const uint32_t wndProcResult = reg(UC_MIPS_REG_V0);
        if (pending.synchronousSender) {
            auto sender = guestThreads_.find(pending.synchronousSender);
            if (sender != guestThreads_.end() &&
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
            spdlog::info("{} message transfer complete hwnd=0x{:08x} msg=0x{:08x} result=0x{:08x} return=0x{:08x}",
                         pending.sourceName,
                         pending.hwnd,
                         pending.message,
                         wndProcResult,
                         pending.originalRa);
        }
        setReg(UC_MIPS_REG_RA, pending.originalRa);
        setReg(UC_MIPS_REG_PC, pending.originalRa);
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

    if (isCoredll && ordinal == 0x0006) {
        if (!finishActiveGuestThread(a0)) {
            spdlog::warn("ExitThread called without active guest thread exitCode=0x{:08x}", a0);
            setReg(UC_MIPS_REG_V0, 0);
            setReg(UC_MIPS_REG_PC, ra);
        }
        pumpHostMessages();
        return;
    }

    if (isCoredll && ordinal == 0x00F6) {
        uint32_t ret = 0;
        if (!dispatchHostWin32(ordinal, args, ret)) {
            lastError_ = 120;
            finishImmediateReturn(0);
            return;
        }
        auto window = windows_.find(ret);
        if (!ret || window == windows_.end() || !window->second.wndProc ||
            !window->second.createStruct || !createWindowContinuationStub_) {
            finishImmediateReturn(ret);
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

    if (isCoredll && ordinal == 0x010B) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
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
        pendingUpdateWindows_.push_back(PendingUpdateWindow{a0, wndProc, ra, eraseDc, 0, "UpdateWindow"});
        spdlog::info("UpdateWindow synchronous WM_ERASEBKGND hwnd=0x{:08x} wndproc=0x{:08x}",
                     a0, wndProc);
        setReg(UC_MIPS_REG_A0, a0);
        setReg(UC_MIPS_REG_A1, 0x0014); // WM_ERASEBKGND
        setReg(UC_MIPS_REG_A2, eraseDc);
        setReg(UC_MIPS_REG_A3, 0);
        setReg(UC_MIPS_REG_RA, updateWindowContinuationStub_);
        setReg(UC_MIPS_REG_PC, wndProc);
        return;
    }

    if (isCoredll && ordinal == 0x0109) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            finishImmediateReturn(0);
            return;
        }
        const auto oldSize = guestMessages_.size();
        std::erase_if(guestMessages_, [&](const GuestMessage& message) { return message.hwnd == a0; });
        if (oldSize != guestMessages_.size()) {
            spdlog::info("DestroyWindow discarded {} pending posted messages for hwnd=0x{:08x}",
                         oldSize - guestMessages_.size(), a0);
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
        pendingDestroyWindows_.push_back(PendingDestroyWindow{a0, wndProc, ra, 0, parent, wasVisible});
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

    if (isCoredll && ordinal == 0x01F0 && activeGuestThread_) {
        auto active = guestThreads_.find(activeGuestThread_);
        if (active != guestThreads_.end()) {
            active->second.context = captureGuestCpuContext();
            active->second.context.registers[UC_MIPS_REG_PC] = ra;
            active->second.context.registers[UC_MIPS_REG_V0] = 0;
            if (a0 == 0) {
                active->second.state = GuestThreadRunState::Runnable;
                active->second.sleepUntilMs = 0;
            } else {
                active->second.state = GuestThreadRunState::Waiting;
                active->second.waitHandle = 0;
                active->second.waitHandles.clear();
                active->second.waitAll = false;
                active->second.sleepUntilMs = hostTickMilliseconds() + uint64_t(a0);
            }
            const uint32_t savedRa = active->second.context.registers.count(UC_MIPS_REG_RA)
                ? active->second.context.registers[UC_MIPS_REG_RA]
                : 0;
            const uint32_t savedSp = active->second.context.registers.count(UC_MIPS_REG_SP)
                ? active->second.context.registers[UC_MIPS_REG_SP]
                : 0;
            spdlog::debug("guest thread sleep handle=0x{:08x} timeout={} return=0x{:08x} savedRa=0x{:08x} savedSp=0x{:08x}",
                          activeGuestThread_, a0, ra, savedRa, savedSp);
        }
        activeGuestThread_ = 0;
        if (mainThreadContext_.valid) {
            updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
            restoreGuestCpuContext(mainThreadContext_);
        } else {
            switchToRunnableGuestThread(name.c_str());
        }
        pumpHostMessages();
        return;
    }

    if (isCoredll && ordinal == 0x01F2) {
        constexpr uint32_t kWaitTimeout = 0x00000102u;
        constexpr uint32_t kWaitFailed = 0xffffffffu;
        if (activeGuestThread_) {
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
                auto active = guestThreads_.find(activeGuestThread_);
                if (active != guestThreads_.end()) {
                    active->second.context = captureGuestCpuContext();
                    active->second.context.registers[UC_MIPS_REG_PC] = ra;
                    active->second.context.registers[UC_MIPS_REG_V0] = 0;
                    active->second.state = GuestThreadRunState::Waiting;
                    active->second.waitHandle = handles.size() == 1 ? handles.front() : 0;
                    active->second.waitHandles = std::move(handles);
                    active->second.waitAll = a2 != 0;
                    spdlog::info("guest thread wait-multiple handle=0x{:08x} count={} waitAll={} timeout=0x{:08x} return=0x{:08x}",
                                 activeGuestThread_, a0, a2 != 0, a3, ra);
                }
                activeGuestThread_ = 0;
                if (mainThreadContext_.valid) {
                    updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
                    restoreGuestCpuContext(mainThreadContext_);
                } else {
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
        if (guestMessages_.empty() && hasRunnableGuestThread()) {
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
    }

    if (isCoredll && ordinal == 0x01F1) {
        if (activeGuestThread_) {
            uint32_t ret = 0xffffffffu;
            bool wouldBlock = false;
            auto* handle = lookupGuestHandle(a0);
#if defined(_WIN32)
            if (handle && handle->kind == GuestHandle::Kind::HostEvent && handle->hostValue) {
                ret = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0);
                if (ret == 0x00000102u && a1 != 0) wouldBlock = true; // WAIT_TIMEOUT
                if (ret == 0xffffffffu) lastError_ = GetLastError();
                else lastError_ = 0;
            } else
#endif
            if (!dispatchHostWin32(ordinal, args, ret)) {
                lastError_ = 120;
                ret = 0xffffffffu;
            }
            if (wouldBlock) {
                auto active = guestThreads_.find(activeGuestThread_);
                if (active != guestThreads_.end()) {
                    active->second.context = captureGuestCpuContext();
                    active->second.context.registers[UC_MIPS_REG_PC] = ra;
                    active->second.context.registers[UC_MIPS_REG_V0] = 0; // completed wait result after wake
                    active->second.state = GuestThreadRunState::Waiting;
                    active->second.waitHandle = a0;
                    active->second.waitHandles.clear();
                    active->second.waitAll = false;
                    spdlog::info("guest thread wait handle=0x{:08x} wait=0x{:08x} return=0x{:08x}",
                                 activeGuestThread_, a0, ra);
                }
                activeGuestThread_ = 0;
                if (mainThreadContext_.valid) {
                    updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
                    restoreGuestCpuContext(mainThreadContext_);
                } else {
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
        if (guestMessages_.empty() && hasRunnableGuestThread()) {
            uint32_t preferredThread = 0;
            auto* handle = lookupGuestHandle(a0);
            if (handle && handle->kind == GuestHandle::Kind::GuestThread) preferredThread = a0;
            bool ready = false;
#if defined(_WIN32)
            if (handle && handle->hostValue &&
                (handle->kind == GuestHandle::Kind::HostEvent ||
                 handle->kind == GuestHandle::Kind::HostMutex ||
                 handle->kind == GuestHandle::Kind::GuestProcess ||
                 handle->kind == GuestHandle::Kind::GuestThread)) {
                ready = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle->hostValue), 0) == 0;
            } else
#endif
            if (handle && handle->kind == GuestHandle::Kind::GuestThread) {
                auto thread = guestThreads_.find(a0);
                ready = thread == guestThreads_.end() ||
                        thread->second.state == GuestThreadRunState::Terminated;
            }
            if (ready || a1 == 0) {
                setReg(UC_MIPS_REG_V0, ready ? 0x00000000u : 0x00000102u);
                setReg(UC_MIPS_REG_PC, ra);
                pumpHostMessages();
                return;
            }
            spdlog::info("WaitForSingleObject cooperative guest-thread slice wait=0x{:08x} timeout=0x{:08x} retry=1",
                         a0, a1);
            switchToRunnableGuestThread(name.c_str(), 0, preferredThread);
            pumpHostMessages();
            return;
        }
    }

    if (isCoredll && (ordinal == 0x039D || ordinal == 0x03AF || ordinal == 0x03CB)) {
        uint32_t ret = 0;
        if (ordinal == 0x039D) {
            ret = createPatternBrushFromBitmap(a0);
            lastError_ = 0;
        } else if (ordinal == 0x03AF) {
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
        } else {
            GuestDc* dc = lookupGuestDc(a0);
            if (!dc || !a1) {
                lastError_ = dc ? 87 : 6;
                ret = 0;
            } else {
                int32_t width = framebufferWidth_;
                int32_t height = framebufferHeight_;
                auto bitmap = bitmaps_.find(dc->selectedBitmap);
                if (bitmap != bitmaps_.end()) {
                    width = bitmap->second.width;
                    height = std::abs(bitmap->second.heightRaw);
                }
                writeGuestRect(a1, 0, 0, width, height);
                lastError_ = 0;
                ret = 2; // SIMPLEREGION
            }
        }
        finishImmediateReturn(ret, true);
        return;
    }

    if (isCoredll && (ordinal == 0x01F0 || ordinal == 0x01F1) && blockingApiContinuationStub_) {
        pendingBlockingApis_.push_back(PendingBlockingApi{name, ordinal, args});
        if (dispatchQueuedPaintForBlockingApi(pendingBlockingApis_.back(), "before block")) {
            return;
        }
        pendingBlockingApis_.pop_back();
    }

    if (isCoredll && ordinal == 0x035D && activeGuestThread_ && guestMessages_.empty() && !quitPosted_) {
        auto active = guestThreads_.find(activeGuestThread_);
        if (active != guestThreads_.end()) {
            active->second.context = captureGuestCpuContext();
            active->second.context.registers[UC_MIPS_REG_PC] = pc;
            active->second.state = GuestThreadRunState::WaitingForMessage;
            spdlog::info("guest thread message wait handle=0x{:08x} return=0x{:08x} msgPtr=0x{:08x}",
                         activeGuestThread_, ra, a0);
        }
        activeGuestThread_ = 0;
        if (mainThreadContext_.valid) {
            updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
            restoreGuestCpuContext(mainThreadContext_);
        } else {
            switchToRunnableGuestThread(name.c_str());
        }
        pumpHostMessages();
        return;
    }

    uint32_t ret = 1;
    if (isCoredll && (ordinal == 0x011D || ordinal == 0x035B || ordinal == 0x0364)) {
        uint32_t wndProc = a0;
        uint32_t hwnd = a1;
        uint32_t msg = a2;
        uint32_t wParam = a3;
        uint32_t lParam = stackArg(4);
        uint32_t synchronousSender = 0;
        if (ordinal == 0x035B) {
            if (a0) {
                uc_mem_read(uc_, a0, &hwnd, sizeof(hwnd));
                uc_mem_read(uc_, a0 + 4, &msg, sizeof(msg));
                uc_mem_read(uc_, a0 + 8, &wParam, sizeof(wParam));
                uc_mem_read(uc_, a0 + 12, &lParam, sizeof(lParam));
                auto syncSender = retrievedSyncSendersByMsgPtr_.find(a0);
                if (syncSender != retrievedSyncSendersByMsgPtr_.end()) {
                    synchronousSender = syncSender->second;
                    retrievedSyncSendersByMsgPtr_.erase(syncSender);
                }
            }
            if (msg == 0x0113 && lParam) {
                wndProc = lParam;
                if (a0) uc_mem_read(uc_, a0 + 16, &lParam, sizeof(lParam));
            } else {
                auto it = windows_.find(hwnd);
                wndProc = it == windows_.end() ? 0 : it->second.wndProc;
            }
        } else if (ordinal == 0x0364) {
            auto it = windows_.find(a0);
            wndProc = it == windows_.end() ? 0 : it->second.wndProc;
            hwnd = a0;
            msg = a1;
            wParam = a2;
            lParam = a3;
        }
        auto targetWindow = windows_.find(hwnd);
        if (targetWindow != windows_.end() && targetWindow->second.externalProcess) {
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
        if (ordinal == 0x0364 && activeGuestThread_ &&
            targetWindow != windows_.end() &&
            targetWindow->second.ownerThread == mainThreadPseudoHandle_ &&
            targetWindow->second.ownerThread != activeGuestThread_) {
            GuestMessage message{};
            message.hwnd = hwnd;
            message.message = msg;
            message.wParam = wParam;
            message.lParam = lParam;
            message.time = uint32_t(++tick_ * 16);
            message.synchronousSender = activeGuestThread_;
            auto insertAt = std::find_if(guestMessages_.begin(), guestMessages_.end(),
                                         [](const GuestMessage& queued) {
                                             return queued.synchronousSender == 0;
                                         });
            guestMessages_.insert(insertAt, message);
            wakeGuestThreadsWaitingForMessage();
            lastError_ = 0;
            const uint32_t senderHandle = activeGuestThread_;
            auto sender = guestThreads_.find(senderHandle);
            if (sender != guestThreads_.end() &&
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
                             targetWindow->second.ownerThread,
                             guestMessages_.size(),
                             sender != guestThreads_.end());
            } else {
                spdlog::debug("SendMessageW cross-thread queued hwnd=0x{:08x} msg=0x{:08x} sender=0x{:08x} owner=0x{:08x} queued={} waiting={}",
                              hwnd,
                              msg,
                              senderHandle,
                              targetWindow->second.ownerThread,
                              guestMessages_.size(),
                              sender != guestThreads_.end());
            }
            activeGuestThread_ = 0;
            if (mainThreadContext_.valid) {
                const uint32_t mainPc = mainThreadContext_.registers.count(UC_MIPS_REG_PC)
                    ? mainThreadContext_.registers[UC_MIPS_REG_PC]
                    : 0;
                const uint32_t mainRa = mainThreadContext_.registers.count(UC_MIPS_REG_RA)
                    ? mainThreadContext_.registers[UC_MIPS_REG_RA]
                    : 0;
                const uint32_t mainSp = mainThreadContext_.registers.count(UC_MIPS_REG_SP)
                    ? mainThreadContext_.registers[UC_MIPS_REG_SP]
                    : 0;
                spdlog::debug("SendMessageW cross-thread restoring main context pc=0x{:08x} ra=0x{:08x} sp=0x{:08x}",
                              mainPc, mainRa, mainSp);
                updateCurrentThreadKData(mainThreadPseudoHandle_, mainThreadTls_);
                restoreGuestCpuContext(mainThreadContext_);
            } else {
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
        if (ordinal == 0x0364 && msg == 0x0201 && wParam == 0 && lParam == 0) {
            auto target = windows_.find(hwnd);
            if (target != windows_.end() && !target->second.destroyed &&
                (target->second.style & kWindowStyleChild) && target->second.parent) {
                pendingSyntheticChildButtonUpWindow_ = hwnd;
                spdlog::info("remembered synthetic child button-down hwnd=0x{:08x} parent=0x{:08x}",
                             hwnd, target->second.parent);
            }
        }
        setReg(UC_MIPS_REG_A0, hwnd);
        setReg(UC_MIPS_REG_A1, msg);
        setReg(UC_MIPS_REG_A2, wParam);
        setReg(UC_MIPS_REG_A3, lParam);
        if (messageTransferContinuationStub_) {
            pendingMessageTransfers_.push_back(PendingMessageTransfer{
                hwnd,
                msg,
                ra,
                synchronousSender,
                name,
            });
            setReg(UC_MIPS_REG_RA, messageTransferContinuationStub_);
        }
        setReg(UC_MIPS_REG_PC, wndProc);
        return;
    }
    if (const auto* ordinalHandler = findOrdinalHandler(mutableEntry);
        ordinalHandler && ordinalHandler->handler) {
        bool handled = (this->*ordinalHandler->handler)(ordinalHandler->code, args, ret);
        if (handled) {
            finishImmediateReturn(ret, true);
            return;
        }
    }
    if (isCoredll &&
        (dispatchHostWin32(ordinal, args, ret) || dispatchGuestMemoryApi(ordinal, args, ret))) {
        if (ordinal == 0x035D && ret == 0 && !quitPosted_) {
            spdlog::debug("GetMessageW empty queue blocks main context pc=0x{:08x} ra=0x{:08x} queued={}",
                          pc, ra, guestMessages_.size());
            setReg(UC_MIPS_REG_PC, pc);
            pumpHostMessages();
            if (!switchToRunnableGuestThread("GetMessageW-blocking", pc)) {
                uc_emu_stop(uc_);
            }
            return;
        }
        finishImmediateReturn(ret, true);
        return;
    }
    if (isCoredll) {
        lastError_ = 120; // ERROR_CALL_NOT_IMPLEMENTED
        ret = 0;
        if (ordinal == 1167) {
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
        } else if (mutableEntry.calls == 1) {
            spdlog::warn("synthetic coredll.dll!{} unsupported by translate layer -> 0", name);
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
