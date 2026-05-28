#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "synthetic_dll.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {
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
#endif

uint32_t guestAnsiCodePage(uint32_t codePage) {
    // The target SDK path is L.kor, so guest CP_ACP/CP_THREAD_ACP is Korean.
    return (codePage == 0 || codePage == 3) ? 949u : codePage;
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
