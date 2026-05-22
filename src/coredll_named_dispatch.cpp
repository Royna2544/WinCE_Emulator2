#include "synthetic_dll.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {
constexpr uint32_t kWindowStyleChild = 0x40000000u;

uint64_t hostTickMilliseconds() {
    return GetTickCount64();
}

uint64_t guestTimerKey(uint32_t hwnd, uint32_t id) {
    return (uint64_t(hwnd) << 32) | id;
}

std::filesystem::path pathFromUtf8(const std::string& text) {
    std::u8string utf8Path;
    utf8Path.resize(text.size());
    std::memcpy(utf8Path.data(), text.data(), text.size());
    return std::filesystem::path(utf8Path);
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8Path = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8Path.data()), utf8Path.size());
}

std::string lowerAscii(std::string text) {
    for (char& ch : text) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return text;
}

uint32_t guestAnsiCodePage(uint32_t codePage) {
    if (codePage == 0 || codePage == 1) return 949;
    return codePage;
}

bool supportedSourceRasterOp(uint32_t rop) {
    switch (rop) {
    case 0x00000042u:
    case 0x00330008u:
    case 0x00550009u:
    case 0x00660046u:
    case 0x008800c6u:
    case 0x00cc0020u:
    case 0x00ee0086u:
    case 0x00ff0062u:
        return true;
    default:
        return false;
    }
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

void ceDefault16BitMasks(uint32_t& redMask, uint32_t& greenMask, uint32_t& blueMask) {
    redMask = 0x0000f800u;
    greenMask = 0x000007e0u;
    blueMask = 0x0000001fu;
}

}

bool SyntheticDllRuntime::dispatchGuestMemoryApi(uint16_t ordinal,
                                                 const GuestCallArgs& args,
                                                 uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    const std::string name = "coredll.ordinal";

    if (ordinal == 0x0385) return handleCreateBitmap(args, ret);
    if (ordinal == 0x0396) return handleGetObjectW(args, ret);
    if (ordinal == 0x0682) return handleSetDIBColorTable(args, ret);
    if (ordinal == 0x06BD) return handleSetBitmapBits(args, ret);
    if (ordinal == 0x06BE) return handleSetDIBitsToDevice(args, ret);

    if (ordinal == 0x00E0 || ordinal == 0x00DD) {
        const bool makeUpper = ordinal == 0x00E0;
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
    } else if (ordinal == 0x03A2) {
        if (!a0) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestPen(readU32(a0), readU32(a0 + 4), readU32(a0 + 12));
            lastError_ = 0;
        }
    } else if (ordinal == 0x005A) {
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
    } else if (ordinal == 0x0386) {
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
    } else if (ordinal == 0x03AB) {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        auto pen = dc ? pens_.find(dc->selectedPen) : pens_.end();
        const bool hasBrush = brush != brushes_.end() && brush->second.colorRef != 0xffffffffu;
        const bool hasPen = pen != pens_.end() && pen->second.style != 5 && pen->second.colorRef != 0xffffffffu;
        if (!dc || !a1 || a2 < 2 || (brush == brushes_.end() && pen == pens_.end())) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            std::vector<std::pair<int32_t, int32_t>> points;
            points.reserve(std::min<uint32_t>(a2, 0x10000));
            for (uint32_t index = 0; index < a2 && index < 0x10000; ++index) {
                const uint32_t point = a1 + index * 8;
                points.emplace_back(int32_t(readU32(point)), int32_t(readU32(point + 4)));
            }
            if (hasBrush) {
                fillDcPolygon(*dc, points, colorRefToPixel(brush->second.colorRef));
            }
            if (hasPen) {
                const uint32_t pixel = colorRefToPixel(pen->second.colorRef);
                for (size_t index = 0; index < points.size(); ++index) {
                    const auto& from = points[index];
                    const auto& to = points[(index + 1) % points.size()];
                    drawDcLine(*dc, from.first, from.second, to.first, to.second, pixel);
                }
            }
            if (!points.empty()) {
                dc->x = points.back().first;
                dc->y = points.back().second;
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x03AC) {
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
                drawDcLine(*dc, prevX, prevY, x, y, pixel);
                prevX = x;
                prevY = y;
            }
            dc->x = prevX;
            dc->y = prevY;
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x0414 || ordinal == 0x0416) {
        copyGuest(a0, a1, a2);
        ret = a0;
    } else if (ordinal == 0x0417) {
        fillGuest(a0, uint8_t(a1 & 0xffu), a2);
        ret = a0;
    } else if (ordinal == 0x0449 || ordinal == 0x0038 || ordinal == 0x044B ||
               ordinal == 0x0448 || ordinal == 0x046C) {
        std::vector<uint32_t> values;
        if (ordinal == 0x044B) {
            values.reserve(16);
            for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(a2 + i * 4));
        } else if (ordinal == 0x046C) {
            values.reserve(16);
            for (uint32_t i = 0; i < 16; ++i) values.push_back(readU32(a3 + i * 4));
        } else if (ordinal == 0x0448) {
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
        const std::string format = readUtf16((ordinal == 0x0448 || ordinal == 0x046C) ? a2 : a1, 2048);
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
        ret = writeUtf16(a0, out, (ordinal == 0x0448 || ordinal == 0x046C) ? a1 : uint32_t(out.size() + 1));
        if (out.find(".db") != std::string::npos || out.find(".bin") != std::string::npos ||
            out.find("\\") != std::string::npos || out.find("/") != std::string::npos) {
            spdlog::info("synthetic coredll.dll!{} formatted \"{}\" -> 0x{:08x}",
                         name, out, a0);
        } else if (out.empty() && (ordinal == 0x0038 || ordinal == 0x0449)) {
            spdlog::info("synthetic coredll.dll!{} formatted empty format=\"{}\" a2w=\"{}\" a2a=\"{}\" a3=0x{:08x}",
                         name, format, readUtf16(a2, 128), readAscii(a2, 128), a3);
        }
    } else if (ordinal == 0x044E || ordinal == 0x02CF || ordinal == 0x02D9) {
        std::vector<uint32_t> values = ordinal == 0x02CF
            ? std::vector<uint32_t>{a2, a3}
            : (ordinal == 0x02D9 ? std::vector<uint32_t>{a3} : std::vector<uint32_t>{a1, a2, a3});
        for (uint32_t i = 4; i < 16; ++i) values.push_back(stackArg(i));
        size_t argIndex = 0;
        auto nextArg = [&]() -> uint32_t {
            return argIndex < values.size() ? values[argIndex++] : 0;
        };
        const std::string format = readAscii(ordinal == 0x02CF ? a1 : (ordinal == 0x02D9 ? a2 : a0), 2048);
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
        if (ordinal == 0x02CF || ordinal == 0x02D9) {
            if (ordinal == 0x02D9) {
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
    } else if (ordinal == 0x004E) {
        const std::string value = readUtf16(a0, 128);
        char* end = nullptr;
        ret = uint32_t(std::strtol(value.c_str(), &end, 10));
    } else if (ordinal == 0x0438) {
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
    } else if (ordinal == 0x02CD) {
        if (a0) {
            writeU32(a0 + 4, 4);
            writeU32(a0 + 8, 20);
            writeU32(a0 + 12, 0);
            writeU32(a0 + 16, 3);
        }
        ret = a0 ? 1 : 0;
    } else if (ordinal == 0x003B || ordinal == 0x0045) {
        const uint16_t target = uint16_t(a1 & 0xffffu);
        uint32_t found = 0;
        for (uint32_t offset = 0; a0; offset += 2) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, a0 + offset, &ch, sizeof(ch)) != UC_ERR_OK) break;
            if (ch == target) {
                found = a0 + offset;
                if (ordinal == 0x003B) break;
            }
            if (!ch) break;
        }
        ret = found;
    } else if (ordinal == 0x0049) {
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
    } else if (ordinal == 0x003F) {
        uint32_t count = 0;
        for (;; ++count) {
            uint16_t ch = 0;
            if (uc_mem_read(uc_, a0 + count * 2, &ch, sizeof(ch)) != UC_ERR_OK || !ch) break;
        }
        ret = count;
    } else if (ordinal == 0x003D) {
        uint32_t offset = 0;
        for (;; offset += 2) {
            uint16_t ch = 0;
            uc_mem_read(uc_, a1 + offset, &ch, sizeof(ch));
            uc_mem_write(uc_, a0 + offset, &ch, sizeof(ch));
            if (!ch) break;
        }
        ret = a0;
    } else if (ordinal == 0x003E) {
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
    } else if (ordinal == 0x0041 || ordinal == 0x00E5) {
        ret = 0;
        for (uint32_t i = 0; i < a2; ++i) {
            uint16_t left = 0;
            uint16_t right = 0;
            uc_mem_read(uc_, a0 + i * 2, &left, sizeof(left));
            uc_mem_read(uc_, a1 + i * 2, &right, sizeof(right));
            if (ordinal == 0x00E5) {
                if (left >= 'A' && left <= 'Z') left = uint16_t(left - 'A' + 'a');
                if (right >= 'A' && right <= 'Z') right = uint16_t(right - 'A' + 'a');
            }
            if (left != right || !left || !right) {
                ret = uint32_t(int(left) - int(right));
                break;
            }
        }
    } else if (ordinal == 0x00E6) {
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
    } else if (ordinal == 0x004A) {
        const std::string value = readUtf16(a0);
        ret = allocate(uint32_t((value.size() + 1) * 2), false);
        writeUtf16(ret, value, uint32_t(value.size() + 1));
    } else if (ordinal == 0x042C) {
        ret = uint32_t(readAscii(a0).size());
    } else if (ordinal == 0x042A) {
        uint32_t offset = 0;
        for (;; ++offset) {
            char ch = 0;
            uc_mem_read(uc_, a1 + offset, &ch, sizeof(ch));
            uc_mem_write(uc_, a0 + offset, &ch, sizeof(ch));
            if (!ch) break;
        }
        ret = a0;
    } else if (ordinal == 0x0427) {
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
    } else if (ordinal == 0x0431) {
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
    } else if (ordinal == 0x0413) {
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
    } else if (ordinal == 0x0429) {
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
    } else if (ordinal == 0x042B) {
        const std::string source = readAscii(a0);
        const std::string reject = readAscii(a1);
        size_t count = 0;
        while (count < source.size() && reject.find(source[count]) == std::string::npos) ++count;
        ret = uint32_t(count);
    } else if (ordinal == 0x0582 || ordinal == 0x0583) {
        std::string left = lowerAscii(readAscii(a0));
        std::string right = lowerAscii(readAscii(a1));
        if (ordinal == 0x0583) {
            left = left.substr(0, a2);
            right = right.substr(0, a2);
        }
        ret = uint32_t(left.compare(right));
    } else if (ordinal == 0x002C) {
        if (!a0 && !a1 && !a2 && a3 == 0x1c) {
            ret = allocate(0x38, true);
        } else {
            ret = 0;
        }
    } else if (ordinal == 0x0009) {
        uint32_t old = 0;
        uc_mem_read(uc_, a0, &old, sizeof(old));
        if (old == a1) uc_mem_write(uc_, a0, &a2, sizeof(a2));
        ret = old;
    } else if (ordinal == 0x00C0) {
        const uint32_t codePage = guestAnsiCodePage(a0);
        const uint8_t ch = uint8_t(a1);
        ret = (codePage == 949 && ch >= 0x81 && ch <= 0xfe) ? 1 : 0;
    } else if (ordinal == 0x00C1) {
        ret = 0;
    } else if (ordinal == 0x00C4) {
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
    } else if (ordinal == 0x0443) {
        ret = uint32_t(std::toupper(int(a0)));
    } else if (ordinal == 0x0628) {
        // Full vector construction needs repeated guest callback transfers. The
        // current callers survive a fail-closed no-construction result.
        ret = 0;
    } else if (ordinal == 0x04CC) {
        ret = 0;
    } else if (ordinal == 0x04CB) {
        ret = a1 && a2 == 0x38 ? 0 : allocate(0x100, true);
    } else if (ordinal == 0x07D0) {
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
    } else if (ordinal == 0x040C) {
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


bool SyntheticDllRuntime::dispatchSimpleHostWin32(uint16_t ordinal,
                                                  const GuestCallArgs& args,
                                                  uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const std::string name = "coredll.ordinal";

    if (ordinal == 0x036B) {
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
    } else {
        return false;
    }
    return true;
}


bool SyntheticDllRuntime::dispatchHostWin32(uint16_t ordinal,
                                            const GuestCallArgs& args,
                                            uint32_t& ret) {
    if (dispatchSimpleHostWin32(ordinal, args, ret)) return true;

    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    const uint32_t ra = args.ra;
    const std::string name = "coredll.ordinal";
    if (ordinal == 0x0059) {
        ret = handleSystemParametersInfoW(a0, a1, a2, a3);
        return true;
    }
    if (ordinal == 0x0394) {
        ret = handleGetDeviceCaps(a0, a1);
        return true;
    }
    if (ordinal == 0x00B3) {
        ret = dispatchDeviceIoControl(a0, a1, a2, a3);
        return true;
    }
    if (ordinal == 0x00C5) {
        ret = handleWideCharToMultiByte(a0, a1, a2, a3);
        return true;
    }
    if (ordinal == 0x0224) {
        ret = handleCreateFileMappingW(a0, a1, a2, a3);
        return true;
    }
    if (ordinal == 0x0225) {
        ret = handleMapViewOfFile(a0, a1, a2, a3);
        return true;
    }
    if (ordinal == 0x0226) {
        ret = handleUnmapViewOfFile(a0);
        return true;
    }
    if (ordinal == 0x0227) {
        ret = handleFlushViewOfFile(a0, a1);
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
            current = (it->second.style & kWindowStyleChild) ? it->second.parent : 0;
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

    if (ordinal == 0x02BA) {
        // CE/MFC uses this in PreTranslateMessage. The emulator queues and
        // dispatches messages itself, so only report that the message was not
        // consumed by dialog navigation.
        ret = 0;
        lastError_ = windows_.count(a0) || !a0 ? 0 : 1400;
        return true;
    }
    if (ordinal == 0x022D) {
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
    } else if (ordinal == 0x05D1) {
        lastError_ = 120;
        ret = 0;
    } else if (ordinal == 0x0058) {
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
    } else if (ordinal == 0x0229) {
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
    } else if (ordinal == 0x00B4) {
        ret = closeGuestHandle(a0);
    } else if (ordinal == 0x01F1) {
        auto* handle = lookupGuestHandle(a0);
        if (handle && handle->kind == GuestHandle::Kind::GuestThread) {
            auto thread = guestThreads_.find(a0);
            if (thread == guestThreads_.end() || thread->second.state == GuestThreadRunState::Terminated) {
                ret = 0;
            } else {
                ret = a1 == 0 ? 0x00000102u : 0x00000102u; // WAIT_TIMEOUT until cooperative scheduling runs it.
            }
            lastError_ = 0;
        } else
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
    } else if (ordinal == 0x01ED) {
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
    } else if (ordinal == 0x0219) {
        ret = writeUtf16(a1, mainModulePath_, a2);
        lastError_ = ret ? 0 : 122;
        spdlog::info("GetModuleFileNameW module=0x{:08x} path=\"{}\" chars={} lastError={}",
                     a0, mainModulePath_, ret, lastError_);
    } else if (ordinal == 0x021D) {
        spdlog::info("OutputDebugStringW: {}", readUtf16(a0));
        ret = 0;
    } else if (ordinal == 0x00EA) {
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
    } else if (ordinal == 0x0210 || ordinal == 0x0499) {
        const std::string requested = readUtf16(a0);
        const std::string pathKey = lowerAscii(requested);
        const std::string nameKey = lowerAscii(pathToUtf8(pathFromUtf8(requested).filename()));
        if (requested.empty()) {
            ret = mainModuleBase_;
        } else if (auto it = loadedModulesByPath_.find(pathKey); it != loadedModulesByPath_.end()) {
            ret = it->second.base;
        } else if (auto it = loadedModulesByName_.find(nameKey); it != loadedModulesByName_.end()) {
            ret = it->second.base;
        } else if (ordinal == 0x0210) {
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
    } else if (ordinal == 0x04CE || ordinal == 0x0212) {
        ret = 0;
        auto module = loadedModulesByBase_.find(a0);
        if (module != loadedModulesByBase_.end()) {
            if (a1 < 0x10000) {
                auto ordinal = module->second.exportsByOrdinal.find(uint16_t(a1));
                if (ordinal != module->second.exportsByOrdinal.end()) ret = module->second.base + ordinal->second;
                spdlog::info("{} module=0x{:08x} ordinal={} -> 0x{:08x}",
                             name, a0, a1, ret);
            } else {
                const std::string proc = ordinal == 0x0212 ? readUtf16(a1) : readAscii(a1, 256);
                auto exported = module->second.exportsByName.find(lowerAscii(proc));
                if (exported != module->second.exportsByName.end()) ret = module->second.base + exported->second;
                spdlog::info("{} module=0x{:08x} proc=\"{}\" -> 0x{:08x}",
                             name, a0, proc, ret);
            }
        }
        lastError_ = ret ? 0 : 127;
    } else if (ordinal == 0x005F) {
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
    } else if (ordinal == 0x036E) {
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
    } else if (ordinal == 0x011E) {
        ret = 0;
        const std::string className = a0 < 0x10000
            ? (windowClassNamesByAtom_.count(uint16_t(a0)) ? windowClassNamesByAtom_[uint16_t(a0)] : std::string{})
            : lowerAscii(readUtf16(a0));
        const std::string title = readUtf16(a1);
        for (const auto& [hwnd, window] : windows_) {
            if (!window.destroyed && (!a0 || window.className == className) && (!a1 || window.title == title)) {
                ret = hwnd;
                break;
            }
        }
        lastError_ = ret ? 0 : 1400;
    } else if (ordinal == 0x00F6) {
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
        auto normalizeSize = [](uint32_t value, int32_t fallback, bool topLevel) -> int32_t {
            const int32_t signedValue = int32_t(value);
            if (topLevel && (value == 0x80000000u || signedValue <= 0)) return fallback;
            if (value == 0x80000000u) return 0;
            return std::max<int32_t>(0, signedValue);
        };
        const bool topLevel = parent == 0;
        const uint32_t rawX = stackArg(4);
        const uint32_t rawY = stackArg(5);
        const uint32_t rawWidth = stackArg(6);
        const uint32_t rawHeight = stackArg(7);
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
        window.x = normalizePos(rawX);
        window.y = normalizePos(rawY);
        window.width = normalizeSize(rawWidth, framebufferWidth_ > 0 ? framebufferWidth_ : 800, topLevel);
        window.height = normalizeSize(rawHeight, framebufferHeight_ > 0 ? framebufferHeight_ : 480, topLevel);
        window.visible = (a3 & 0x10000000u) != 0; // WS_VISIBLE
        spdlog::info("CreateWindowExW guest=0x{:08x} class=\"{}\" title=\"{}\" parent=0x{:08x} id/menu=0x{:08x} "
                     "style=0x{:08x} ex=0x{:08x} wndproc=0x{:08x} param=0x{:08x} rect={},{} {}x{} raw=0x{:08x},0x{:08x} 0x{:08x}x0x{:08x}",
                     ret, className, window.title, parent, menu, window.style, window.exStyle,
                     wndProc, param, window.x, window.y, window.width, window.height,
                     rawX, rawY, rawWidth, rawHeight);
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
    } else if (ordinal == 0x00F8) {
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
    } else if (ordinal == 0x00F9) {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            writeGuestRect(a1, 0, 0, it->second.width, it->second.height);
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x00FA) {
        const uint32_t target = a0 ? a0 : firstWindow();
        if (!target || !windows_.count(target)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            queueGuestPaint(target, a2 != 0);
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x00FF) {
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
    } else if (ordinal == 0x036C) {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            timers_.erase(guestTimerKey(a0, a1));
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x039E) {
        ret = makeGuestPen(a0, a1, a2);
        lastError_ = 0;
    } else if (ordinal == 0x03A3) {
        ret = makeGuestBrush(a0);
        lastError_ = 0;
    } else if (ordinal == 0x03D4) {
#if defined(_WIN32)
        HRGN region = CreateRectRgn(int(a0), int(a1), int(a2), int(a3));
        ret = region ? makeGuestHandle({GuestHandle::Kind::HostRegion, reinterpret_cast<uintptr_t>(region), 0}) : 0;
        lastError_ = ret ? 0 : GetLastError();
#else
        ret = makeGuestHandle({GuestHandle::Kind::HostRegion, 0, 0});
        lastError_ = ret ? 0 : 8;
#endif
    } else if (ordinal == 0x03C8) {
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
    } else if (ordinal == 0x037F) {
        std::array<uint8_t, 92> font{};
        if (!a0 || uc_mem_read(uc_, a0, font.data(), font.size()) != UC_ERR_OK) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestFont(font);
            lastError_ = 0;
        }
    } else if (ordinal == 0x0397) {
        ret = makeStockObject(int32_t(a0));
        lastError_ = ret ? 0 : 87;
    } else if (ordinal == 0x0399) {
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
    } else if (ordinal == 0x0390) {
        auto object = guestHandles_.find(a0);
        if (object == guestHandles_.end()) {
            lastError_ = 6;
            ret = 0;
        } else if (object->second.filePointer && object->second.kind != GuestHandle::Kind::HostBitmap) {
            ret = 0;
            lastError_ = 6;
        } else if (object->second.kind == GuestHandle::Kind::GuestBrush) {
            auto brush = brushes_.find(a0);
            if (brush != brushes_.end() && brush->second.stock) {
                ret = 1;
                lastError_ = 0;
                return true;
            }
            brushes_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestPen) {
            auto pen = pens_.find(a0);
            if (pen != pens_.end() && pen->second.stock) {
                ret = 1;
                lastError_ = 0;
                return true;
            }
            pens_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestFont) {
            auto font = fonts_.find(a0);
            if (font != fonts_.end() && font->second.stock) {
                ret = 1;
                lastError_ = 0;
                return true;
            }
            fonts_.erase(a0);
            guestHandles_.erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostBitmap) {
            auto bitmap = bitmaps_.find(a0);
            if (bitmap != bitmaps_.end() && bitmap->second.stock) {
                ret = 1;
                lastError_ = 0;
                return true;
            }
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
    } else if (ordinal == 0x039A || ordinal == 0x039C || ordinal == 0x039B || ordinal == 0x0676) {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else if (ordinal == 0x039A) {
            ret = dc->bkColor;
            dc->bkColor = a1;
            lastError_ = 0;
        } else if (ordinal == 0x039C) {
            ret = dc->textColor;
            dc->textColor = a1;
            lastError_ = 0;
        } else if (ordinal == 0x039B) {
            ret = dc->bkMode;
            dc->bkMode = a1;
            lastError_ = 0;
        } else {
            ret = dc->textAlign;
            dc->textAlign = a1;
            lastError_ = 0;
        }
    } else if (ordinal == 0x03A7) {
        GuestDc* dc = lookupGuestDc(a0);
        int32_t left = 0, top = 0, right = 0, bottom = 0;
        auto brush = brushes_.find(a2);
        if (!dc || !readGuestRect(a1, left, top, right, bottom) || brush == brushes_.end()) {
            lastError_ = 87;
            ret = 0;
        } else {
            if (brush->second.colorRef != 0xffffffffu) {
                fillDcRect(*dc, left, top, right, bottom, colorRefToPixel(brush->second.colorRef));
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x03AA) {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        const uint32_t rop = stackArg(5);
        if (!dc || brush == brushes_.end() || rop != 0x00f00021u) {
            lastError_ = dc ? 120 : 6;
            ret = 0;
        } else {
            if (brush->second.colorRef != 0xffffffffu) {
                fillDcRect(*dc, int32_t(a1), int32_t(a2),
                           int32_t(a1) + int32_t(a3),
                           int32_t(a2) + int32_t(stackArg(4)),
                           colorRefToPixel(brush->second.colorRef));
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x03AD) {
        GuestDc* dc = lookupGuestDc(a0);
        auto brush = dc ? brushes_.find(dc->selectedBrush) : brushes_.end();
        auto pen = dc ? pens_.find(dc->selectedPen) : pens_.end();
        if (!dc || (brush == brushes_.end() && pen == pens_.end())) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            const int32_t left = int32_t(a1);
            const int32_t top = int32_t(a2);
            const int32_t right = int32_t(a3);
            const int32_t bottom = int32_t(stackArg(4));
            if (brush != brushes_.end() && brush->second.colorRef != 0xffffffffu) {
                fillDcRect(*dc, left, top, right, bottom, colorRefToPixel(brush->second.colorRef));
            }
            if (pen != pens_.end() && pen->second.style != 5 && pen->second.colorRef != 0xffffffffu) {
                const uint32_t pixel = colorRefToPixel(pen->second.colorRef);
                drawDcLine(*dc, left, top, right - 1, top, pixel);
                drawDcLine(*dc, left, bottom - 1, right - 1, bottom - 1, pixel);
                drawDcLine(*dc, left, top, left, bottom - 1, pixel);
                drawDcLine(*dc, right - 1, top, right - 1, bottom - 1, pixel);
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x0673) {
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
    } else if (ordinal == 0x0674) {
        GuestDc* dc = lookupGuestDc(a0);
        auto pen = dc ? pens_.find(dc->selectedPen) : pens_.end();
        if (!dc || pen == pens_.end()) {
            lastError_ = dc ? 87 : 6;
            ret = 0;
        } else {
            if (pen->second.style != 5 && pen->second.colorRef != 0xffffffffu) {
                drawDcLine(*dc, dc->x, dc->y, int32_t(a1), int32_t(a2),
                           colorRefToPixel(pen->second.colorRef));
            }
            dc->x = int32_t(a1);
            dc->y = int32_t(a2);
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x0683) {
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
    } else if (ordinal == 0x038A) {
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
            const int32_t srcBitmapHeight = std::abs(srcBitmap->second.heightRaw);
            const int32_t dstBitmapHeight = std::abs(dstBitmap->second.heightRaw);
            const bool interesting =
                srcBitmap->second.width >= 100 || srcBitmapHeight >= 40 ||
                dstBitmap->second.width >= 100 || dstBitmapHeight >= 40 ||
                dstDc->hwnd || srcDc->hwnd;
            if (ok && interesting && blitProbeLogCounter_ < 320) {
                ++blitProbeLogCounter_;
                const BitmapProbeStats srcStats =
                    bitmapProbeStats(srcBitmap->second, srcX, srcY, srcW, srcH);
                const BitmapProbeStats dstStats =
                    bitmapProbeStats(dstBitmap->second, int32_t(a1), int32_t(a2), int32_t(a3), dstH);
                spdlog::info("TransparentImage probe#{} memory dstDc=0x{:08x} dstHwnd=0x{:08x} "
                             "dstBitmap=0x{:08x} dstSize={}x{} dstRect={},{} {}x{} "
                             "dstStats sampled={} nonBlack={} unique~{} first=0x{:08x} last=0x{:08x} "
                             "srcDc=0x{:08x} srcHwnd=0x{:08x} srcBitmap=0x{:08x} srcSize={}x{} "
                             "srcRect={},{} {}x{} srcStats sampled={} nonBlack={} unique~{} "
                             "first=0x{:08x} last=0x{:08x} color=0x{:08x}",
                             blitProbeLogCounter_, a0, dstDc->hwnd, dstDc->selectedBitmap,
                             dstBitmap->second.width, dstBitmap->second.heightRaw,
                             int32_t(a1), int32_t(a2), int32_t(a3), dstH,
                             dstStats.sampled, dstStats.nonBlack, dstStats.uniqueApprox,
                             dstStats.firstPixel, dstStats.lastPixel, stackArg(5), srcDc->hwnd,
                             srcDc->selectedBitmap, srcBitmap->second.width, srcBitmap->second.heightRaw,
                             srcX, srcY, srcW, srcH, srcStats.sampled, srcStats.nonBlack,
                             srcStats.uniqueApprox, srcStats.firstPixel, srcStats.lastPixel,
                             transparentColor);
            }
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
            const int32_t srcBitmapHeight = std::abs(srcBitmap->second.heightRaw);
            const bool interesting =
                srcBitmap->second.width >= 100 || srcBitmapHeight >= 40 ||
                std::abs(int32_t(a3)) >= 100 || std::abs(dstH) >= 40 || dstDc->hwnd;
            if (ok && interesting && blitProbeLogCounter_ < 320) {
                ++blitProbeLogCounter_;
                const BitmapProbeStats srcStats =
                    bitmapProbeStats(srcBitmap->second, srcX, srcY, srcW, srcH);
                spdlog::debug("TransparentImage probe#{} framebuffer dstDc=0x{:08x} dstHwnd=0x{:08x} "
                             "dstRect={},{} {}x{} srcDc=0x{:08x} srcHwnd=0x{:08x} "
                             "srcBitmap=0x{:08x} srcSize={}x{} srcRect={},{} {}x{} "
                             "srcStats sampled={} nonBlack={} unique~{} first=0x{:08x} "
                             "last=0x{:08x} color=0x{:08x}",
                             blitProbeLogCounter_, a0, dstDc->hwnd, int32_t(a1), int32_t(a2),
                             int32_t(a3), dstH, stackArg(5), srcDc->hwnd, srcDc->selectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, transparentColor);
                if (blitProbeDumpCounter_ < 12 && std::abs(int32_t(a3)) >= 100 && std::abs(dstH) >= 40) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "transparent_present_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcDc->selectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpFramebufferPpm(tag);
                }
            }
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
    } else if (ordinal == 0x0380 || ordinal == 0x03B1) {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0;
        } else {
            const bool drawTextCall = ordinal == 0x03B1;
            const bool ok = drawTextCall
                ? drawHostTextToDc(*dc, 0, 0, 0, a2, a1, int32_t(a3), stackArg(4), true)
                : drawHostTextToDc(*dc, int32_t(a1), int32_t(a2), a3, stackArg(4),
                                   stackArg(5), int32_t(stackArg(6)), 0, false);
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
    } else if (ordinal == 0x038E) {
        ret = makeGuestDc(0);
        if (GuestDc* dc = lookupGuestDc(ret)) {
            dc->selectedBitmap = makeStockObject(21); // DEFAULT_BITMAP
        }
        lastError_ = ret ? 0 : 8;
    } else if (ordinal == 0x038F) {
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
    } else if (ordinal == 0x0387 || ordinal == 0x0389) {
        GuestDc* dstDc = lookupGuestDc(a0);
        GuestDc* srcDc = lookupGuestDc(stackArg(5));
        const bool stretch = ordinal == 0x0389;
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
            const bool interesting =
                srcBitmap->second.width >= 100 || srcBitmapHeight >= 40 ||
                dstBitmap->second.width >= 100 || dstBitmapHeight >= 40 ||
                dstDc->hwnd || srcDc->hwnd;
            if (ok && interesting && blitProbeLogCounter_ < 320) {
                ++blitProbeLogCounter_;
                const BitmapProbeStats srcStats =
                    bitmapProbeStats(srcBitmap->second, srcX, srcY, srcW, srcH);
                const BitmapProbeStats dstStats =
                    bitmapProbeStats(dstBitmap->second, int32_t(a1), int32_t(a2), dstW, dstH);
                spdlog::debug("{} probe#{} memory dstDc=0x{:08x} dstHwnd=0x{:08x} "
                             "dstBitmap=0x{:08x} dstSize={}x{} dstRect={},{} {}x{} "
                             "dstStats sampled={} nonBlack={} unique~{} first=0x{:08x} last=0x{:08x} "
                             "srcDc=0x{:08x} srcHwnd=0x{:08x} srcBitmap=0x{:08x} srcSize={}x{} "
                             "srcRect={},{} {}x{} srcStats sampled={} nonBlack={} unique~{} "
                             "first=0x{:08x} last=0x{:08x} rop=0x{:08x}",
                             name, blitProbeLogCounter_, a0, dstDc->hwnd, dstDc->selectedBitmap,
                             dstBitmap->second.width, dstBitmap->second.heightRaw,
                             int32_t(a1), int32_t(a2), dstW, dstH, dstStats.sampled,
                             dstStats.nonBlack, dstStats.uniqueApprox, dstStats.firstPixel,
                             dstStats.lastPixel, stackArg(5), srcDc->hwnd, srcDc->selectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, rop);
                if (blitProbeDumpCounter_ < 12 &&
                    (std::abs(dstW) >= 100 || std::abs(dstH) >= 40 || dstBitmap->second.width >= 700)) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "memory_blit_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcDc->selectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpGuestBitmapPpm(dstDc->selectedBitmap, dstBitmap->second, std::string(tag) + "_result");
                }
            }
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
            const int32_t srcBitmapHeight = std::abs(srcBitmap->second.heightRaw);
            const bool interesting =
                srcBitmap->second.width >= 100 || srcBitmapHeight >= 40 ||
                std::abs(dstW) >= 100 || std::abs(dstH) >= 40 || dstDc->hwnd;
            if (ok && interesting && blitProbeLogCounter_ < 320) {
                ++blitProbeLogCounter_;
                const BitmapProbeStats srcStats =
                    bitmapProbeStats(srcBitmap->second, srcX, srcY, srcW, srcH);
                spdlog::debug("{} probe#{} framebuffer dstDc=0x{:08x} dstHwnd=0x{:08x} "
                             "dstRect={},{} {}x{} srcDc=0x{:08x} srcHwnd=0x{:08x} "
                             "srcBitmap=0x{:08x} srcSize={}x{} srcRect={},{} {}x{} "
                             "srcStats sampled={} nonBlack={} unique~{} first=0x{:08x} "
                             "last=0x{:08x} rop=0x{:08x}",
                             name, blitProbeLogCounter_, a0, dstDc->hwnd, int32_t(a1), int32_t(a2),
                             dstW, dstH, stackArg(5), srcDc->hwnd, srcDc->selectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, rop);
                if (blitProbeDumpCounter_ < 12 && std::abs(dstW) >= 100 && std::abs(dstH) >= 40) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "framebuffer_blit_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcDc->selectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpFramebufferPpm(tag);
                }
            }
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
    } else if (ordinal == 0x0103) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = getWindowLongValue(it->second, int32_t(a1));
        }
    } else if (ordinal == 0x0102) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = setWindowLongValue(it->second, int32_t(a1), a2);
        }
    } else if (ordinal == 0x010D) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = it->second.parent;
        }
    } else if (ordinal == 0x00FB) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else if (a1 == 5) {
            ret = 0;
            for (const auto& [hwnd, window] : windows_) {
                if (!window.destroyed && window.parent == a0 &&
                    (window.style & kWindowStyleChild)) {
                    ret = hwnd;
                    break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 4) {
            const auto owner = windows_.find(it->second.parent);
            ret = !(it->second.style & kWindowStyleChild) &&
                  owner != windows_.end() && !owner->second.destroyed ? it->second.parent : 0;
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 2 || a1 == 3) {
            std::vector<uint32_t> siblings;
            for (const auto& [hwnd, window] : windows_) {
                if (!window.destroyed && window.parent == it->second.parent &&
                    ((window.style & kWindowStyleChild) == (it->second.style & kWindowStyleChild))) {
                    siblings.push_back(hwnd);
                }
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
                if (!window.destroyed && window.parent == it->second.parent &&
                    ((window.style & kWindowStyleChild) == (it->second.style & kWindowStyleChild))) {
                    ret = hwnd;
                    if (a1 == 0) break;
                }
            }
            lastError_ = ret ? 0 : 1400;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
    } else if (ordinal == 0x0110) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool sizeChanged = it->second.width != int32_t(a3) ||
                                     it->second.height != int32_t(stackArg(4));
            it->second.x = int32_t(a1);
            it->second.y = int32_t(a2);
            it->second.width = int32_t(a3);
            it->second.height = int32_t(stackArg(4));
            if (sizeChanged) it->second.paintBoundsValid = false;
ensureHostWindow(a0, it->second);
            syncHostWindowPlacement(it->second, stackArg(5) != 0);
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
    } else if (ordinal == 0x00F7) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
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
                it->second.paintBoundsValid = false;
            }
            if (flags & 0x0040u) it->second.visible = true;  // SWP_SHOWWINDOW
            if (flags & 0x0080u) {
                it->second.visible = false; // SWP_HIDEWINDOW
                it->second.paintBoundsValid = false;
            }
ensureHostWindow(a0, it->second);
            syncHostWindowPlacement(it->second, true);
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
                if (!it->second.visible && it->second.parent) {
                    eraseGuestWindowArea(a0, it->second);
                    spdlog::info("SetWindowPos invalidating parent=0x{:08x} after hiding child=0x{:08x}",
                                 it->second.parent, a0);
                    queueGuestPaint(it->second.parent, true);
                }
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x0576) {
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
    } else if (ordinal == 0x0577) {
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
    } else if (ordinal == 0x0109) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool wasVisible = it->second.visible;
            const uint32_t parent = it->second.parent;
            for (auto timer = timers_.begin(); timer != timers_.end();) {
                if (timer->second.hwnd == a0) timer = timers_.erase(timer);
                else ++timer;
            }
            if (focusedWindow_ == a0) focusedWindow_ = 0;
            if (capturedWindow_ == a0) capturedWindow_ = 0;
            if (hostPointerCaptureWindow_ == a0) hostPointerCaptureWindow_ = 0;
            if (wasVisible) eraseGuestWindowArea(a0, it->second);
            it->second.visible = false;
            it->second.destroyed = true;
            it->second.paintBoundsValid = false;
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
            if (wasVisible && parent) {
                spdlog::info("DestroyWindow invalidating parent=0x{:08x} after child=0x{:08x}", parent, a0);
                queueGuestPaint(parent, true);
            }
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x010A) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool wasVisible = it->second.visible;
            it->second.visible = a1 != 0;
            if (!it->second.visible) it->second.paintBoundsValid = false;
            spdlog::info("ShowWindow guest=0x{:08x} cmd={} oldVisible={} newVisible={}",
                         a0, int32_t(a1), wasVisible ? 1 : 0, it->second.visible ? 1 : 0);
            ensureHostWindow(a0, it->second);
#if defined(_WIN32)
            if (it->second.hostHwnd) {
                HWND hwnd = reinterpret_cast<HWND>(it->second.hostHwnd);
                ShowWindow(hwnd, it->second.visible ? SW_SHOWNORMAL : SW_HIDE);
                presentHostWindows(true);
            }
#endif
            if (it->second.visible != wasVisible) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0018; // WM_SHOWWINDOW
                message.wParam = it->second.visible ? 1 : 0;
                message.time = uint32_t(++tick_ * 16);
                guestMessages_.push_back(message);
                if (!it->second.visible && it->second.parent) {
                    eraseGuestWindowArea(a0, it->second);
                    spdlog::info("ShowWindow invalidating parent=0x{:08x} after hiding child=0x{:08x}",
                                 it->second.parent, a0);
                    queueGuestPaint(it->second.parent, true);
                }
            }
            lastError_ = 0;
            ret = wasVisible ? 1 : 0;
        }
    } else if (ordinal == 0x010B) {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            queueGuestPaint(a0, true);
            ensureHostWindow(a0, it->second);
            lastError_ = 0;
            ret = 1;
        }
    } else if (ordinal == 0x0108) {
        if (a1 == 0x0081) { // WM_NCCREATE defaults to TRUE.
            ret = 1;
        } else if (a1 == 0x000c) { // WM_SETTEXT
            auto it = windows_.find(a0);
            if (it == windows_.end() || it->second.destroyed) {
                lastError_ = 1400;
                ret = 0;
            } else {
                it->second.title = a3 ? readUtf16(a3) : std::string{};
                queueGuestPaint(a0, true);
                lastError_ = 0;
                ret = 1;
            }
        } else if (a1 == 0x000d) { // WM_GETTEXT
            auto it = windows_.find(a0);
            if (it == windows_.end() || it->second.destroyed) {
                lastError_ = 1400;
                ret = 0;
            } else if (!a2 || !a3) {
                lastError_ = 0;
                ret = 0;
            } else {
                ret = writeUtf16(a3, it->second.title, a2);
                lastError_ = 0;
            }
        } else if (a1 == 0x000e) { // WM_GETTEXTLENGTH
            auto it = windows_.find(a0);
            ret = it == windows_.end() || it->second.destroyed ? 0 : uint32_t(it->second.title.size());
            lastError_ = it == windows_.end() || it->second.destroyed ? 1400 : 0;
        } else {
            ret = 0;
        }
    } else if (ordinal == 0x035E) {
        ret = 0;
    } else if (ordinal == 0x0366) {
        ret = a0 ? 1 : 0;
    } else if (ordinal == 0x0362) {
        quitPosted_ = true;
        GuestMessage message{};
        message.message = 0x0012; // WM_QUIT
        message.wParam = a0;
        message.time = uint32_t(++tick_ * 16);
        guestMessages_.push_back(message);
        ret = 0;
    } else if (ordinal == 0x0361) {
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
    } else if (ordinal == 0x035D || ordinal == 0x035F || ordinal == 0x0360) {
        const bool peek = ordinal == 0x0360 || ordinal == 0x035F;
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
        if (!haveMessage && !peek && !quitPosted_ && !timers_.empty() && !hasHostWindows()) {
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
    } else if (ordinal == 0x021E) {
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
    } else if (ordinal == 0x0143) {
        if (a0) {
            writeU32(a0, 64u * 1024u * 1024u);
            writeU32(a0 + 4, 32u * 1024u * 1024u);
        }
        ret = a0 ? 1 : 0;
    } else if (ordinal == 0x05EF) {
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
    } else if (ordinal == 0x05F1) {
        const std::string atomName = lowerAscii(readUtf16(a0));
        auto it = atomsByName_.find(atomName);
        ret = it == atomsByName_.end() ? 0 : it->second;
        lastError_ = ret ? 0 : 2;
    } else if (ordinal == 0x05F0) {
        auto it = atomNames_.find(uint16_t(a0));
        if (it != atomNames_.end()) {
            atomsByName_.erase(it->second);
            atomNames_.erase(it);
        }
        ret = 0;
        lastError_ = 0;
    } else if (ordinal == 0x01C3) {
        ret = handleWNetGetUserW(a0, a1, a2);
    } else if (ordinal == 0x01C2 || ordinal == 0x01BE) {
        lastError_ = 1200;
        ret = 1200;
    } else {
        return false;
    }

    return true;
}
