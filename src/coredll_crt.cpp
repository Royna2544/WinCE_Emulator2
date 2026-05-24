#include "synthetic_dll.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

std::string pathToUtf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

}

void SyntheticDllRuntime::registerCoredllCrtExports(SyntheticModule& module) {
    struct CoreDllCrt {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.crt",
                {
                    {0x0038, {"wsprintfW", Code::CoreDllWsprintfW, &SyntheticDllRuntime::handleWideFormat}},
                    {0x003B, {"wcschr", Code::CoreDllWcschr, &SyntheticDllRuntime::handleWcschr}},
                    {0x003D, {"wcscpy", Code::CoreDllWcscpy, &SyntheticDllRuntime::handleWcscpy}},
                    {0x003E, {"wcscspn", Code::CoreDllWcscspn, &SyntheticDllRuntime::handleWcscspn}},
                    {0x003F, {"wcslen", Code::CoreDllWcslen, &SyntheticDllRuntime::handleWcslen}},
                    {0x0041, {"wcsncmp", Code::CoreDllWcsncmp, &SyntheticDllRuntime::handleWcsncmp}},
                    {0x0045, {"wcsrchr", Code::CoreDllWcsrchr, &SyntheticDllRuntime::handleWcschr}},
                    {0x0049, {"wcsstr", Code::CoreDllWcsstr, &SyntheticDllRuntime::handleWcsstr}},
                    {0x004A, {"_wcsdup", Code::CoreDllWcsdup, &SyntheticDllRuntime::handleWcsdup}},
                    {0x004E, {"_wtol", Code::CoreDllWtol, &SyntheticDllRuntime::handleWtol}},
                    {0x00E5, {"_wcsnicmp", Code::CoreDllWcsnicmp, &SyntheticDllRuntime::handleWcsncmp}},
                    {0x00E6, {"_wcsicmp", Code::CoreDllWcsicmp, &SyntheticDllRuntime::handleWcsicmp}},
                    {0x02CF, {"sprintf", Code::CoreDllSprintf, &SyntheticDllRuntime::handleNarrowFormat}},
                    {0x02D9, {"_snprintf", Code::CoreDllSnprintf, &SyntheticDllRuntime::handleNarrowFormat}},
                    {0x040C, {"longjmp", Code::CoreDllLongjmp, &SyntheticDllRuntime::handleLongjmp}},
                    {0x0413, {"memcmp", Code::CoreDllMemcmp, &SyntheticDllRuntime::handleMemcmp}},
                    {0x0414, {"memcpy", Code::CoreDllMemcpy, &SyntheticDllRuntime::handleMemcpy}},
                    {0x0416, {"memmove", Code::CoreDllMemmove, &SyntheticDllRuntime::handleMemcpy}},
                    {0x0417, {"memset", Code::CoreDllMemset, &SyntheticDllRuntime::handleMemset}},
                    {0x0427, {"strcat", Code::CoreDllStrcat, &SyntheticDllRuntime::handleStrcat}},
                    {0x0429, {"strcmp", Code::CoreDllStrcmp, &SyntheticDllRuntime::handleStrcmp}},
                    {0x042A, {"strcpy", Code::CoreDllStrcpy, &SyntheticDllRuntime::handleStrcpy}},
                    {0x042B, {"strcspn", Code::CoreDllStrcspn, &SyntheticDllRuntime::handleStrcspn}},
                    {0x042C, {"strlen", Code::CoreDllStrlen, &SyntheticDllRuntime::handleStrlen}},
                    {0x0431, {"strtok", Code::CoreDllStrtok, &SyntheticDllRuntime::handleStrtok}},
                    {0x0438, {"_ultow", Code::CoreDllUltow, &SyntheticDllRuntime::handleUltow}},
                    {0x0443, {"toupper", Code::CoreDllToupper, &SyntheticDllRuntime::handleToupper}},
                    {0x0448, {"_snwprintf", Code::CoreDllSnwprintf, &SyntheticDllRuntime::handleWideFormat}},
                    {0x0449, {"swprintf", Code::CoreDllSwprintf, &SyntheticDllRuntime::handleWideFormat}},
                    {0x044B, {"vswprintf", Code::CoreDllVswprintf, &SyntheticDllRuntime::handleWideFormat}},
                    {0x044E, {"printf", Code::CoreDllPrintf, &SyntheticDllRuntime::handleNarrowFormat}},
                    {0x0454, {"fgetc", Code::CoreDllFgetc, &SyntheticDllRuntime::handleFgetc}},
                    {0x0455, {"fgets", Code::CoreDllFgets, &SyntheticDllRuntime::handleFgets}},
                    {0x0459, {"fopen", Code::CoreDllFopen, &SyntheticDllRuntime::handleFopen}},
                    {0x045E, {"fclose", Code::CoreDllFclose, &SyntheticDllRuntime::handleFclose}},
                    {0x0460, {"fread", Code::CoreDllFread, &SyntheticDllRuntime::handleFread}},
                    {0x0461, {"fwrite", Code::CoreDllFwrite, &SyntheticDllRuntime::handleFwrite}},
                    {0x0462, {"fflush", Code::CoreDllFflush, &SyntheticDllRuntime::handleFflush}},
                    {0x0465, {"feof", Code::CoreDllFeof, &SyntheticDllRuntime::handleFeof}},
                    {0x0466, {"ferror", Code::CoreDllFerror, &SyntheticDllRuntime::handleFerror}},
                    {0x046A, {"fseek", Code::CoreDllFseek, &SyntheticDllRuntime::handleFseek}},
                    {0x046B, {"ftell", Code::CoreDllFtell, &SyntheticDllRuntime::handleFtell}},
                    {0x046C, {"_vsnwprintf", Code::CoreDllVsnwprintf, &SyntheticDllRuntime::handleWideFormat}},
                    {0x0479, {"_wfopen", Code::CoreDllWfopen, &SyntheticDllRuntime::handleWfopen}},
                    {0x047A, {"vsprintf", Code::CoreDllVsprintf, &SyntheticDllRuntime::handleNarrowFormat}},
                    {0x04CB, {"GetCRTStorageEx", Code::CoreDllGetCrtStorageEx, &SyntheticDllRuntime::handleGetCrtStorageEx}},
                    {0x04CC, {"GetCRTFlags", Code::CoreDllGetCrtFlags, &SyntheticDllRuntime::handleGetCrtFlags}},
                    {0x0582, {"_stricmp", Code::CoreDllStricmp, &SyntheticDllRuntime::handleStricmp}},
                    {0x0583, {"_strnicmp", Code::CoreDllStrnicmp, &SyntheticDllRuntime::handleStrnicmp}},
                    {0x0628, {"__ehvec_ctor", Code::CoreDllEhvecCtor, &SyntheticDllRuntime::handleEhvecCtor}},
                    {0x07D0, {"_setjmp", Code::CoreDllSetjmp, &SyntheticDllRuntime::handleSetjmp}},
                },
            };
        }
    };

    const CoreDllCrt crt;
    registerHandlers(module, crt.group());
}

bool SyntheticDllRuntime::handleFopen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    const bool wide = code == SyntheticExportCode::CoreDllWfopen;
    const std::string guestPath = wide ? readUtf16(args.a0, 2048) : readAscii(args.a0, 2048);
    const std::string mode = wide ? readUtf16(args.a1, 64) : readAscii(args.a1, 64);
    const std::filesystem::path hostPath = resolveGuestPath(guestPath);
    FILE* host = nullptr;
    if (!hostPath.empty()) {
        std::wstring wideMode(mode.begin(), mode.end());
        _wfopen_s(&host, hostPath.wstring().c_str(), wideMode.c_str());
    }
    if (!host) {
        ret = 0;
        spdlog::warn("{} miss guest=\"{}\" host=\"{}\" mode=\"{}\"",
                     wide ? "_wfopen" : "fopen", guestPath, pathToUtf8(hostPath), mode);
    } else {
        ret = makeGuestHandle({GuestHandle::Kind::HostCrtFile, reinterpret_cast<uintptr_t>(host), 0});
        fileHandleDebugNames_[ret] = pathToUtf8(hostPath);
        spdlog::info("{} hit guest=\"{}\" host=\"{}\" mode=\"{}\" guestHandle=0x{:08x}",
                     wide ? "_wfopen" : "fopen", guestPath, pathToUtf8(hostPath), mode, ret);
    }
    return true;
}

bool SyntheticDllRuntime::handleMemcpy(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    copyGuest(args.a0, args.a1, args.a2);
    ret = args.a0;
    return true;
}

bool SyntheticDllRuntime::handleMemset(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    fillGuest(args.a0, uint8_t(args.a1 & 0xffu), args.a2);
    ret = args.a0;
    return true;
}

bool SyntheticDllRuntime::handleMemcmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = 0;
    for (uint32_t offset = 0; offset < args.a2; ++offset) {
        unsigned char left = 0;
        unsigned char right = 0;
        if (uc_mem_read(uc_, args.a0 + offset, &left, sizeof(left)) != UC_ERR_OK ||
            uc_mem_read(uc_, args.a1 + offset, &right, sizeof(right)) != UC_ERR_OK) {
            ret = 0xffffffffu;
            break;
        }
        if (left != right) {
            ret = uint32_t(int(left) - int(right));
            break;
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleStrlen(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = uint32_t(readAscii(args.a0).size());
    return true;
}

bool SyntheticDllRuntime::handleStrcpy(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t offset = 0;
    for (;; ++offset) {
        char ch = 0;
        uc_mem_read(uc_, args.a1 + offset, &ch, sizeof(ch));
        uc_mem_write(uc_, args.a0 + offset, &ch, sizeof(ch));
        if (!ch) break;
    }
    ret = args.a0;
    return true;
}

bool SyntheticDllRuntime::handleStrcat(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t dstLen = 0;
    for (; dstLen < 0x100000; ++dstLen) {
        char ch = 0;
        if (uc_mem_read(uc_, args.a0 + dstLen, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
    }
    uint32_t offset = 0;
    for (;; ++offset) {
        char ch = 0;
        uc_mem_read(uc_, args.a1 + offset, &ch, sizeof(ch));
        uc_mem_write(uc_, args.a0 + dstLen + offset, &ch, sizeof(ch));
        if (!ch) break;
    }
    ret = args.a0;
    return true;
}

bool SyntheticDllRuntime::handleStrcmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t offset = 0;
    for (;; ++offset) {
        unsigned char left = 0;
        unsigned char right = 0;
        uc_mem_read(uc_, args.a0 + offset, &left, sizeof(left));
        uc_mem_read(uc_, args.a1 + offset, &right, sizeof(right));
        if (left != right || !left || !right) {
            ret = uint32_t(int(left) - int(right));
            break;
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleStrcspn(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string source = readAscii(args.a0);
    const std::string reject = readAscii(args.a1);
    size_t count = 0;
    while (count < source.size() && reject.find(source[count]) == std::string::npos) ++count;
    ret = uint32_t(count);
    return true;
}

bool SyntheticDllRuntime::handleStricmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string left = lowerAscii(readAscii(args.a0));
    const std::string right = lowerAscii(readAscii(args.a1));
    ret = uint32_t(left.compare(right));
    return true;
}

bool SyntheticDllRuntime::handleStrnicmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    std::string left = lowerAscii(readAscii(args.a0));
    std::string right = lowerAscii(readAscii(args.a1));
    left = left.substr(0, args.a2);
    right = right.substr(0, args.a2);
    ret = uint32_t(left.compare(right));
    return true;
}

bool SyntheticDllRuntime::handleStrtok(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string delimiters = readAscii(args.a1, 256);
    uint32_t cursor = args.a0 ? args.a0 : strtokNext_;
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
    return true;
}

bool SyntheticDllRuntime::handleWcschr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    const uint16_t target = uint16_t(args.a1 & 0xffffu);
    uint32_t found = 0;
    for (uint32_t offset = 0; args.a0; offset += 2) {
        uint16_t ch = 0;
        if (uc_mem_read(uc_, args.a0 + offset, &ch, sizeof(ch)) != UC_ERR_OK) break;
        if (ch == target) {
            found = args.a0 + offset;
            if (code != SyntheticExportCode::CoreDllWcsrchr) break;
        }
        if (!ch) break;
    }
    ret = found;
    return true;
}

bool SyntheticDllRuntime::handleWcsstr(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = 0;
    uint16_t needleFirst = 0;
    if (args.a0 && args.a1 && uc_mem_read(uc_, args.a1, &needleFirst, sizeof(needleFirst)) == UC_ERR_OK) {
        if (!needleFirst) {
            ret = args.a0;
        } else {
            for (uint32_t hayOffset = 0; hayOffset < 0x200000; hayOffset += 2) {
                uint16_t hay = 0;
                if (uc_mem_read(uc_, args.a0 + hayOffset, &hay, sizeof(hay)) != UC_ERR_OK || !hay) break;
                if (hay != needleFirst) continue;
                bool match = true;
                for (uint32_t needleOffset = 2; needleOffset < 0x200000; needleOffset += 2) {
                    uint16_t needle = 0;
                    uint16_t candidate = 0;
                    if (uc_mem_read(uc_, args.a1 + needleOffset, &needle, sizeof(needle)) != UC_ERR_OK) {
                        match = false;
                        break;
                    }
                    if (!needle) break;
                    if (uc_mem_read(uc_, args.a0 + hayOffset + needleOffset, &candidate, sizeof(candidate)) != UC_ERR_OK ||
                        candidate != needle) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    ret = args.a0 + hayOffset;
                    break;
                }
            }
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWcslen(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t count = 0;
    for (;; ++count) {
        uint16_t ch = 0;
        if (uc_mem_read(uc_, args.a0 + count * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
    }
    ret = count;
    return true;
}

bool SyntheticDllRuntime::handleWcscpy(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t offset = 0;
    for (;; offset += 2) {
        uint16_t ch = 0;
        uc_mem_read(uc_, args.a1 + offset, &ch, sizeof(ch));
        uc_mem_write(uc_, args.a0 + offset, &ch, sizeof(ch));
        if (!ch) break;
    }
    ret = args.a0;
    return true;
}

bool SyntheticDllRuntime::handleWcscspn(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    uint32_t count = 0;
    for (;; ++count) {
        uint16_t ch = 0;
        if (uc_mem_read(uc_, args.a0 + count * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
        bool rejected = false;
        for (uint32_t offset = 0; args.a1; offset += 2) {
            uint16_t reject = 0;
            if (uc_mem_read(uc_, args.a1 + offset, &reject, sizeof(reject)) != UC_ERR_OK || !reject) break;
            if (ch == reject) {
                rejected = true;
                break;
            }
        }
        if (rejected) break;
    }
    ret = count;
    return true;
}

bool SyntheticDllRuntime::handleWcsncmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    ret = 0;
    for (uint32_t i = 0; i < args.a2; ++i) {
        uint16_t left = 0;
        uint16_t right = 0;
        uc_mem_read(uc_, args.a0 + i * 2, &left, sizeof(left));
        uc_mem_read(uc_, args.a1 + i * 2, &right, sizeof(right));
        if (code == SyntheticExportCode::CoreDllWcsnicmp) {
            if (left >= 'A' && left <= 'Z') left = uint16_t(left - 'A' + 'a');
            if (right >= 'A' && right <= 'Z') right = uint16_t(right - 'A' + 'a');
        }
        if (left != right || !left || !right) {
            ret = uint32_t(int(left) - int(right));
            break;
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWcsicmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = 0;
    for (uint32_t i = 0; i < 0x100000; ++i) {
        uint16_t left = 0;
        uint16_t right = 0;
        uc_mem_read(uc_, args.a0 + i * 2, &left, sizeof(left));
        uc_mem_read(uc_, args.a1 + i * 2, &right, sizeof(right));
        if (left >= 'A' && left <= 'Z') left = uint16_t(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z') right = uint16_t(right - 'A' + 'a');
        if (left != right || !left || !right) {
            ret = uint32_t(int(left) - int(right));
            break;
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWcsdup(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string value = readUtf16(args.a0);
    ret = allocate(uint32_t((value.size() + 1) * 2), false);
    writeUtf16(ret, value, uint32_t(value.size() + 1));
    return true;
}

bool SyntheticDllRuntime::handleWtol(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const std::string value = readUtf16(args.a0, 128);
    char* end = nullptr;
    ret = uint32_t(std::strtol(value.c_str(), &end, 10));
    return true;
}

bool SyntheticDllRuntime::handleUltow(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const uint32_t radix = (args.a2 >= 2 && args.a2 <= 36) ? args.a2 : 10;
    uint32_t value = args.a0;
    std::string digits;
    do {
        const uint32_t digit = value % radix;
        digits.push_back(char(digit < 10 ? ('0' + digit) : ('a' + digit - 10)));
        value /= radix;
    } while (value);
    std::reverse(digits.begin(), digits.end());
    writeUtf16(args.a1, digits, uint32_t(digits.size() + 1));
    ret = args.a1;
    return true;
}

bool SyntheticDllRuntime::handleToupper(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = uint32_t(std::toupper(int(args.a0)));
    return true;
}

bool SyntheticDllRuntime::handleWideFormat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    std::vector<uint32_t> values;
    if (code == SyntheticExportCode::CoreDllVswprintf) {
        values.reserve(16);
        for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(args.a2 + i * 4));
    } else if (code == SyntheticExportCode::CoreDllVsnwprintf) {
        values.reserve(16);
        for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(args.a3 + i * 4));
    } else if (code == SyntheticExportCode::CoreDllSnwprintf) {
        values = {args.a3};
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
    } else {
        values = {args.a2, args.a3};
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
    }
    size_t argIndex = 0;
    auto nextArg = [&]() -> uint32_t {
        return argIndex < values.size() ? values[argIndex++] : 0;
    };
    const bool bounded = code == SyntheticExportCode::CoreDllSnwprintf || code == SyntheticExportCode::CoreDllVsnwprintf;
    const uint32_t formatPtr = bounded ? args.a2 : args.a1;
    const std::string format = readUtf16(formatPtr, 2048);
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
    ret = writeUtf16(args.a0, out, bounded ? args.a1 : uint32_t(out.size() + 1));
    if (out.find(".db") != std::string::npos || out.find(".bin") != std::string::npos ||
        out.find("\\") != std::string::npos || out.find("/") != std::string::npos) {
        spdlog::debug("synthetic coredll.dll!wide-crt formatted \"{}\" -> 0x{:08x}", out, args.a0);
    } else if (out.empty() && (code == SyntheticExportCode::CoreDllWsprintfW || code == SyntheticExportCode::CoreDllSwprintf)) {
        spdlog::debug("synthetic coredll.dll!wide-crt formatted empty format=\"{}\" a2w=\"{}\" a2a=\"{}\" a3=0x{:08x}",
                      format, readUtf16(args.a2, 128), readAscii(args.a2, 128), args.a3);
    }
    return true;
}

bool SyntheticDllRuntime::handleNarrowFormat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    std::vector<uint32_t> values;
    uint32_t formatPtr = args.a0;
    uint32_t destPtr = 0;
    uint32_t capacity = 0;
    bool writesOutput = false;
    bool bounded = false;
    if (code == SyntheticExportCode::CoreDllSprintf) {
        destPtr = args.a0;
        formatPtr = args.a1;
        writesOutput = true;
        values = {args.a2, args.a3};
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
    } else if (code == SyntheticExportCode::CoreDllSnprintf) {
        destPtr = args.a0;
        capacity = args.a1;
        formatPtr = args.a2;
        writesOutput = true;
        bounded = true;
        values = {args.a3};
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
    } else if (code == SyntheticExportCode::CoreDllVsprintf) {
        destPtr = args.a0;
        formatPtr = args.a1;
        writesOutput = true;
        for (uint32_t i = 0; i < 64; ++i) values.push_back(readU32(args.a2 + i * 4));
    } else {
        values = {args.a1, args.a2, args.a3};
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
    }
    size_t argIndex = 0;
    auto nextArg = [&]() -> uint32_t {
        return argIndex < values.size() ? values[argIndex++] : 0;
    };
    auto nextDouble = [&]() -> double {
        const uint32_t low = nextArg();
        const uint32_t high = nextArg();
        const uint64_t bits = (uint64_t(high) << 32) | uint64_t(low);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const std::string format = readAscii(formatPtr, 2048);
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
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
            if (width) {
                std::snprintf(buffer, sizeof(buffer), zeroPad ? "%0*.6g" : "%*.6g", width, nextDouble());
            } else {
                std::snprintf(buffer, sizeof(buffer), "%.6g", nextDouble());
            }
            out += buffer;
            break;
        default:
            out.push_back('%');
            out.push_back(spec);
            break;
        }
    }
    if (writesOutput) {
        if (bounded) {
            if (destPtr && capacity) {
                const uint32_t copy = std::min<uint32_t>(capacity - 1, uint32_t(out.size()));
                if (copy) uc_mem_write(uc_, destPtr, out.data(), copy);
                const char nul = 0;
                uc_mem_write(uc_, destPtr + copy, &nul, sizeof(nul));
            }
        } else {
            writeAscii(destPtr, out);
        }
        if (out.find(".db") != std::string::npos || out.find(".bin") != std::string::npos ||
            out.find("\\") != std::string::npos || out.find("/") != std::string::npos) {
            spdlog::debug("synthetic coredll.dll!narrow-crt formatted \"{}\" -> 0x{:08x}", out, destPtr);
        }
    } else if (!out.empty()) {
        spdlog::info("printf: {}", out);
    }
    ret = uint32_t(out.size());
    return true;
}

bool SyntheticDllRuntime::handleGetCrtFlags(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetCrtStorageEx(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = args.a1 && args.a2 == 0x38 ? 0 : allocate(0x100, true);
    return true;
}

bool SyntheticDllRuntime::handleEhvecCtor(SyntheticExportCode, const GuestCallArgs&, uint32_t& ret) {
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleSetjmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    constexpr uint32_t kJmpBufMagic = 0x4a4d5032u; // "JMP2"
    ret = 0;
    if (args.a0 && isGuestRangeReadable(args.a0, 56)) {
        writeU32(args.a0 + 0, kJmpBufMagic);
        writeU32(args.a0 + 4, args.ra);
        writeU32(args.a0 + 8, reg(UC_MIPS_REG_SP));
        writeU32(args.a0 + 12, reg(UC_MIPS_REG_GP));
        writeU32(args.a0 + 16, reg(UC_MIPS_REG_S0));
        writeU32(args.a0 + 20, reg(UC_MIPS_REG_S1));
        writeU32(args.a0 + 24, reg(UC_MIPS_REG_S2));
        writeU32(args.a0 + 28, reg(UC_MIPS_REG_S3));
        writeU32(args.a0 + 32, reg(UC_MIPS_REG_S4));
        writeU32(args.a0 + 36, reg(UC_MIPS_REG_S5));
        writeU32(args.a0 + 40, reg(UC_MIPS_REG_S6));
        writeU32(args.a0 + 44, reg(UC_MIPS_REG_S7));
        writeU32(args.a0 + 48, reg(UC_MIPS_REG_FP));
        writeU32(args.a0 + 52, 0);
    }
    return true;
}

bool SyntheticDllRuntime::handleLongjmp(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    constexpr uint32_t kJmpBufMagic = 0x4a4d5032u; // "JMP2"
    ret = args.a1 ? args.a1 : 1;
    if (args.a0 && isGuestRangeReadable(args.a0, 56) && readU32(args.a0) == kJmpBufMagic) {
        const uint32_t savedRa = readU32(args.a0 + 4);
        setReg(UC_MIPS_REG_RA, savedRa);
        setReg(UC_MIPS_REG_SP, readU32(args.a0 + 8));
        setReg(UC_MIPS_REG_GP, readU32(args.a0 + 12));
        setReg(UC_MIPS_REG_S0, readU32(args.a0 + 16));
        setReg(UC_MIPS_REG_S1, readU32(args.a0 + 20));
        setReg(UC_MIPS_REG_S2, readU32(args.a0 + 24));
        setReg(UC_MIPS_REG_S3, readU32(args.a0 + 28));
        setReg(UC_MIPS_REG_S4, readU32(args.a0 + 32));
        setReg(UC_MIPS_REG_S5, readU32(args.a0 + 36));
        setReg(UC_MIPS_REG_S6, readU32(args.a0 + 40));
        setReg(UC_MIPS_REG_S7, readU32(args.a0 + 44));
        setReg(UC_MIPS_REG_FP, readU32(args.a0 + 48));
        setReg(UC_MIPS_REG_PC, savedRa);
        spdlog::info("longjmp restored guest context jmpbuf=0x{:08x} pc=0x{:08x} value={}",
                     args.a0, savedRa, ret);
    } else {
        spdlog::warn("longjmp received invalid or uninitialized jmp_buf=0x{:08x}", args.a0);
    }
    return true;
}

bool SyntheticDllRuntime::handleWfopen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleFopen(code, args, ret);
}

bool SyntheticDllRuntime::handleFclose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = guestHandles_.find(args.a0);
    if (it == guestHandles_.end() || it->second.kind != GuestHandle::Kind::HostCrtFile || !it->second.hostValue) {
        ret = 0xffffffffu;
    } else {
        FILE* host = reinterpret_cast<FILE*>(it->second.hostValue);
        ret = uint32_t(std::fclose(host));
        fileHandleDebugNames_.erase(args.a0);
        fileReadCounts_.erase(args.a0);
        guestHandles_.erase(it);
    }
    return true;
}

bool SyntheticDllRuntime::handleFread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a3);
    if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !args.a0 || !args.a1) {
        ret = 0;
    } else {
        const uint64_t total = uint64_t(args.a1) * uint64_t(args.a2);
        std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(total, 0x1000000u)));
        const size_t readBytes = bytes.empty()
            ? 0
            : std::fread(bytes.data(), 1, bytes.size(), reinterpret_cast<FILE*>(handle->hostValue));
        if (readBytes) uc_mem_write(uc_, args.a0, bytes.data(), readBytes);
        ret = uint32_t(readBytes / args.a1);
        const uint32_t readCount = ++fileReadCounts_[args.a3];
        if (readCount <= 32 || readBytes != bytes.size()) {
            auto debugName = fileHandleDebugNames_.find(args.a3);
            const std::string debugPath = debugName == fileHandleDebugNames_.end() ? std::string{} : debugName->second;
            spdlog::debug("fread handle=0x{:08x} path=\"{}\" size={} count={} bytes={} elements={} read#={}",
                          args.a3, debugPath, args.a1, args.a2, readBytes, ret, readCount);
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleFwrite(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a3);
    if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !args.a0 || !args.a1) {
        ret = 0;
    } else {
        const uint64_t total = uint64_t(args.a1) * uint64_t(args.a2);
        std::vector<uint8_t> bytes(size_t(std::min<uint64_t>(total, 0x1000000u)));
        if (!bytes.empty()) uc_mem_read(uc_, args.a0, bytes.data(), bytes.size());
        const size_t writtenBytes = bytes.empty()
            ? 0
            : std::fwrite(bytes.data(), 1, bytes.size(), reinterpret_cast<FILE*>(handle->hostValue));
        ret = uint32_t(writtenBytes / args.a1);
    }
    return true;
}

bool SyntheticDllRuntime::handleFseek(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
        ? uint32_t(std::fseek(reinterpret_cast<FILE*>(handle->hostValue), int32_t(args.a1), int(args.a2)))
        : 0xffffffffu;
    return true;
}

bool SyntheticDllRuntime::handleFtell(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
        ? uint32_t(std::ftell(reinterpret_cast<FILE*>(handle->hostValue)))
        : 0xffffffffu;
    return true;
}

bool SyntheticDllRuntime::handleFflush(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = (!args.a0 || (handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue))
        ? uint32_t(std::fflush(args.a0 ? reinterpret_cast<FILE*>(handle->hostValue) : nullptr))
        : 0xffffffffu;
    return true;
}

bool SyntheticDllRuntime::handleFeof(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
        ? uint32_t(std::feof(reinterpret_cast<FILE*>(handle->hostValue)))
        : 0;
    return true;
}

bool SyntheticDllRuntime::handleFerror(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
        ? uint32_t(std::ferror(reinterpret_cast<FILE*>(handle->hostValue)))
        : 0;
    return true;
}

bool SyntheticDllRuntime::handleFgetc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    ret = handle && handle->kind == GuestHandle::Kind::HostCrtFile && handle->hostValue
        ? uint32_t(std::fgetc(reinterpret_cast<FILE*>(handle->hostValue)))
        : 0xffffffffu;
    return true;
}

bool SyntheticDllRuntime::handleFgets(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a2);
    if (!handle || handle->kind != GuestHandle::Kind::HostCrtFile || !handle->hostValue || !args.a0 || !args.a1) {
        ret = 0;
    } else {
        std::vector<char> bytes(args.a1);
        char* result = std::fgets(bytes.data(), int(args.a1), reinterpret_cast<FILE*>(handle->hostValue));
        if (result) uc_mem_write(uc_, args.a0, bytes.data(), std::strlen(bytes.data()) + 1);
        ret = result ? args.a0 : 0;
    }
    return true;
}
