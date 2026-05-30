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
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace {
constexpr uint32_t kWindowStyleChild = 0x40000000u;
constexpr uint32_t kWmCopyData = 0x004au;
constexpr uint32_t kMaxCrossProcessCopyData = 0x100000u;
constexpr int32_t kStockDefaultBitmap = 21;

bool isHostProcessAlive(uint32_t processId) {
    if (!processId) {
        return false;
    }
#if defined(_WIN32)
    if (processId == GetCurrentProcessId()) {
        return true;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return false;
    }
    DWORD exitCode = 0;
    const bool alive = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    return true;
#endif
}

#if defined(_WIN32)
std::wstring crossProcessMutexName(const wchar_t* prefix, const std::filesystem::path& path) {
    const std::wstring text = std::filesystem::absolute(path).wstring();
    return std::wstring(L"Local\\INAVI_EMU_") + prefix + L"_" +
           std::to_wstring(std::hash<std::wstring>{}(text));
}

class ScopedCrossProcessMutex {
public:
    ScopedCrossProcessMutex(const wchar_t* prefix, const std::filesystem::path& path) {
        handle_ = CreateMutexW(nullptr, FALSE, crossProcessMutexName(prefix, path).c_str());
        if (!handle_) return;
        const DWORD wait = WaitForSingleObject(handle_, 5000);
        locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        if (!locked_) {
            spdlog::warn("cross-process mutex wait failed error={}", GetLastError());
        }
    }

    ~ScopedCrossProcessMutex() {
        if (locked_) ReleaseMutex(handle_);
        if (handle_) CloseHandle(handle_);
    }

    bool locked() const { return locked_; }

private:
    HANDLE handle_{};
    bool locked_{};
};
#endif

enum class CoredllOrdinal : uint16_t {
    GetAPIAddress = 0x002C,
    GlobalMemoryStatus = 0x0058,
    SystemParametersInfoW = 0x0059,
    CreateDIBSection = 0x005A,
    RegisterClassW = 0x005F,
    DeviceIoControl = 0x00B3,
    IsDBCSLeadByteEx = 0x00C0,
    IsWctype = 0x00C1,
    MultiByteToWideChar = 0x00C4,
    WideCharToMultiByte = 0x00C5,
    CharLowerW = 0x00DD,
    CharUpperW = 0x00E0,
    FormatMessageW = 0x00EA,
    CreateWindowExW = 0x00F6,
    SetWindowPos = 0x00F7,
    GetWindowRect = 0x00F8,
    GetClientRect = 0x00F9,
    InvalidateRect = 0x00FA,
    GetWindow = 0x00FB,
    ClientToScreen = 0x00FE,
    ScreenToClient = 0x00FF,
    SetWindowTextW = 0x0100,
    SetWindowLongW = 0x0102,
    GetWindowLongW = 0x0103,
    DefWindowProcW = 0x0108,
    DestroyWindow = 0x0109,
    ShowWindow = 0x010A,
    UpdateWindow = 0x010B,
    GetParent = 0x010D,
    IsWindow = 0x010F,
    MoveWindow = 0x0110,
    FindWindowW = 0x011E,
    GetStoreInformation = 0x0143,
    WNetConnectionDialog1W = 0x01BE,
    WNetGetUniversalNameW = 0x01C2,
    WNetGetUserW = 0x01C3,
    CreateProcessW = 0x01ED,
    WaitForSingleObject = 0x01F1,
    LoadLibraryW = 0x0210,
    FreeLibrary = 0x0211,
    GetProcAddressW = 0x0212,
    GetModuleFileNameW = 0x0219,
    OutputDebugStringW = 0x021D,
    GetSystemInfo = 0x021E,
    CreateFileMappingW = 0x0224,
    MapViewOfFile = 0x0225,
    UnmapViewOfFile = 0x0226,
    FlushViewOfFile = 0x0227,
    CloseHandle = 0x0229,
    KernelIoControl = 0x022D,
    CreateDialogIndirectParamW = 0x02B0,
    IsDialogMessageW = 0x02BA,
    GetVersionExW = 0x02CD,
    DispatchMessageW = 0x035B,
    GetMessageW = 0x035D,
    GetMessagePos = 0x035E,
    GetMessageWNoWait = 0x035F,
    PeekMessageW = 0x0360,
    PostMessageW = 0x0361,
    PostQuitMessage = 0x0362,
    TranslateMessage = 0x0366,
    SetTimer = 0x036B,
    KillTimer = 0x036C,
    GetClassInfoW = 0x036E,
    RegisterTaskBar = 0x037C,
    CreateFontIndirectW = 0x037F,
    ExtTextOutW = 0x0380,
    CreateBitmap = 0x0385,
    CreateCompatibleBitmap = 0x0386,
    BitBlt = 0x0387,
    StretchBlt = 0x0389,
    TransparentImage = 0x038A,
    CreateCompatibleDC = 0x038E,
    DeleteDC = 0x038F,
    DeleteObject = 0x0390,
    GetDeviceCaps = 0x0394,
    GetObjectW = 0x0396,
    GetStockObject = 0x0397,
    SelectObject = 0x0399,
    SetBkColor = 0x039A,
    SetBkMode = 0x039B,
    SetTextColor = 0x039C,
    CreatePatternBrush = 0x039D,
    CreatePen = 0x039E,
    CreatePenIndirect = 0x03A2,
    CreateSolidBrush = 0x03A3,
    Ellipse = 0x03A6,
    FillRect = 0x03A7,
    PatBlt = 0x03AA,
    Polygon = 0x03AB,
    Polyline = 0x03AC,
    Rectangle = 0x03AD,
    SetBrushOrgEx = 0x03AF,
    DrawTextW = 0x03B1,
    CombineRgn = 0x03C8,
    GetClipBox = 0x03CB,
    CreateRectRgn = 0x03D4,
    GetModuleHandleW = 0x0499,
    GetProcAddressA = 0x04CE,
    KernelLibIoControl = 0x05D1,
    RegisterDesktop = 0x05E3,
    GlobalAddAtomW = 0x05EF,
    GlobalDeleteAtom = 0x05F0,
    GlobalFindAtomW = 0x05F1,
    SetWindowRgn = 0x0576,
    GetWindowRgn = 0x0577,
    MoveToEx = 0x0673,
    LineTo = 0x0674,
    SetTextAlign = 0x0676,
    SetDIBColorTable = 0x0682,
    StretchDIBits = 0x0683,
    SetBitmapBits = 0x06BD,
    SetDIBitsToDevice = 0x06BE,
};

constexpr uint16_t ord(CoredllOrdinal ordinal) {
    return static_cast<uint16_t>(ordinal);
}

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

std::wstring widenUtf8Lossy(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    if (required > 0) {
        std::wstring wide(size_t(required), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), required);
        return wide;
    }
    std::wstring wide;
    wide.reserve(text.size());
    for (unsigned char ch : text) wide.push_back(wchar_t(ch));
    return wide;
}

std::wstring quoteCommandArg(std::wstring_view arg) {
    if (arg.empty()) return L"\"\"";
    const bool needsQuotes = arg.find_first_of(L" \t\n\v\"") != std::wstring_view::npos;
    if (!needsQuotes) return std::wstring(arg);

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(ch);
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring buildCommandLine(const std::vector<std::wstring>& args) {
    std::wstring commandLine;
    for (const std::wstring& arg : args) {
        if (!commandLine.empty()) commandLine.push_back(L' ');
        commandLine += quoteCommandArg(arg);
    }
    return commandLine;
}

std::wstring sanitizeLogFileStem(std::wstring value) {
    if (value.empty()) return L"child";
    for (wchar_t& ch : value) {
        const bool ok = (ch >= L'0' && ch <= L'9') ||
                        (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') ||
                        ch == L'_' || ch == L'-' || ch == L'.';
        if (!ok) ch = L'_';
    }
    return value;
}

std::vector<wchar_t> buildQuietChildEnvironmentBlock(bool verboseChildLog,
                                                     const std::vector<std::wstring>& extraEntries = {}) {
    std::vector<std::wstring> extraNames;
    extraNames.reserve(extraEntries.size());
    for (const std::wstring& extra : extraEntries) {
        const size_t equals = extra.find(L'=');
        if (equals != std::wstring::npos && equals > 0) {
            extraNames.push_back(extra.substr(0, equals));
        }
    }
    std::vector<std::wstring> entries;
#if defined(_WIN32)
    LPWCH rawEnvironment = GetEnvironmentStringsW();
    if (rawEnvironment) {
        for (const wchar_t* entry = rawEnvironment; *entry;) {
            const std::wstring value(entry);
            entry += value.size() + 1;
            const size_t equals = value.find(L'=');
            if (equals == std::wstring::npos || equals == 0) {
                entries.push_back(value);
                continue;
            }
            const std::wstring name = value.substr(0, equals);
            if (_wcsicmp(name.c_str(), L"INAVI_EMU_LOG") == 0 ||
                _wcsicmp(name.c_str(), L"INAVI_EMU_DUMPS") == 0) {
                continue;
            }
            const bool replacedByExtra = std::any_of(extraNames.begin(), extraNames.end(),
                                                     [&](const std::wstring& extraName) {
                                                         return _wcsicmp(name.c_str(), extraName.c_str()) == 0;
                                                     });
            if (replacedByExtra) {
                continue;
            }
            entries.push_back(value);
        }
        FreeEnvironmentStringsW(rawEnvironment);
    }
#endif
    entries.emplace_back(verboseChildLog ? L"INAVI_EMU_LOG=info" : L"INAVI_EMU_LOG=error");
    entries.emplace_back(L"INAVI_EMU_DUMPS=0");
    entries.insert(entries.end(), extraEntries.begin(), extraEntries.end());
    std::sort(entries.begin(), entries.end(), [](const std::wstring& lhs, const std::wstring& rhs) {
        return _wcsicmp(lhs.c_str(), rhs.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const std::wstring& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

std::filesystem::path childLogDirectoryFromEnvironment() {
    wchar_t buffer[32768]{};
    const DWORD chars = GetEnvironmentVariableW(L"INAVI_EMU_CHILD_LOG_DIR", buffer, DWORD(std::size(buffer)));
    if (!chars || chars >= DWORD(std::size(buffer))) return {};
    return std::filesystem::path(buffer);
}

bool realChildProcessesEnabled() {
    char value[32]{};
    const DWORD chars = GetEnvironmentVariableA("INAVI_EMU_REAL_CHILD_PROCESS", value, DWORD(std::size(value)));
    if (!chars) return true;
    if (chars >= DWORD(std::size(value))) return true;
    return std::strcmp(value, "0") != 0 &&
           _stricmp(value, "false") != 0 &&
           _stricmp(value, "no") != 0 &&
           _stricmp(value, "off") != 0;
}

bool inRuntimeChildProcessesEnabled() {
    char value[32]{};
    const DWORD chars = GetEnvironmentVariableA("INAVI_EMU_INPROC_CHILD_PROCESS", value, DWORD(std::size(value)));
    if (!chars || chars >= DWORD(std::size(value))) return false;
    return std::strcmp(value, "1") == 0 ||
           _stricmp(value, "true") == 0 ||
           _stricmp(value, "yes") == 0 ||
           _stricmp(value, "on") == 0;
}

uint32_t hostProcessId() {
#if defined(_WIN32)
    return uint32_t(GetCurrentProcessId());
#else
    return 0;
#endif
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

    switch (ordinal) {
    case ord(CoredllOrdinal::CreateBitmap):
        return handleCreateBitmap(args, ret);
    case ord(CoredllOrdinal::GetObjectW):
        return handleGetObjectW(args, ret);
    case ord(CoredllOrdinal::SetDIBColorTable):
        return handleSetDIBColorTable(args, ret);
    case ord(CoredllOrdinal::SetBitmapBits):
        return handleSetBitmapBits(args, ret);
    case ord(CoredllOrdinal::SetDIBitsToDevice):
        return handleSetDIBitsToDevice(args, ret);

    case ord(CoredllOrdinal::CharUpperW):
    case ord(CoredllOrdinal::CharLowerW):
    {
        bool makeUpper = false;
        switch (ordinal) {
        case ord(CoredllOrdinal::CharUpperW): makeUpper = true; break;
        default: break;
        }
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
        break;
    }
    case ord(CoredllOrdinal::CreatePenIndirect):
    {
        if (!a0) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestPen(readU32(a0), readU32(a0 + 4), readU32(a0 + 12));
            lastError_ = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::CreateDIBSection):
    {
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
                mirrorMgdiBitmap(ret, bitmaps_[ret]);
            }
            spdlog::debug("CreateDIBSection {}x{} bpp={} compression={} masks={:08x}/{:08x}/{:08x} stride={} bits=0x{:08x} bitmap=0x{:08x}",
                          width, heightRaw, bitsPerPixel, compression,
                          ret && bitmaps_.count(ret) ? bitmaps_[ret].redMask : 0,
                          ret && bitmaps_.count(ret) ? bitmaps_[ret].greenMask : 0,
                          ret && bitmaps_.count(ret) ? bitmaps_[ret].blueMask : 0,
                          stride, bits, ret);
        }
        break;
    }
    case ord(CoredllOrdinal::CreateCompatibleBitmap):
    {
        const uint32_t width = std::max<uint32_t>(a1, 1);
        const uint32_t height = std::max<uint32_t>(a2, 1);
        const uint32_t bpp = 32;
        const uint32_t stride = ((width * bpp + 31) / 32) * 4;
        const uint32_t bits = allocate(std::max<uint32_t>(stride * height, 4), true);
        ret = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, bits});
        if (ret) {
            bitmaps_[ret] = GuestBitmap{int32_t(width), -int32_t(height), uint16_t(bpp), stride, bits,
                                        0, 0, 0, {}};
            mirrorMgdiBitmap(ret, bitmaps_[ret]);
        }
        lastError_ = ret ? 0 : 8;
        spdlog::info("CreateCompatibleBitmap {}x{} bits=0x{:08x} bitmap=0x{:08x}",
                     width, height, bits, ret);
        break;
    }
    case ord(CoredllOrdinal::Polygon):
    {
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
            static uint32_t polygonDebugCount = 0;
            if (!points.empty() && polygonDebugCount < 96) {
                ++polygonDebugCount;
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
                spdlog::debug("Polygon probe#{} dc=0x{:08x} hwnd=0x{:08x} bitmap=0x{:08x} count={} bounds={},{}..{},{} "
                              "brush=0x{:08x} brushColor=0x{:08x} pattern=0x{:08x} pen=0x{:08x} penColor=0x{:08x}",
                              polygonDebugCount, a0, dc->hwnd, dc->selectedBitmap, a2,
                              minX, minY, maxX, maxY, dc->selectedBrush,
                              hasBrush ? brush->second.colorRef : 0xffffffffu,
                              hasBrush ? brush->second.patternBitmap : 0,
                              dc->selectedPen, hasPen ? pen->second.colorRef : 0xffffffffu);
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
        break;
    }
    case ord(CoredllOrdinal::Polyline):
    {
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
        break;
    }
    case ord(CoredllOrdinal::GetVersionExW):
    {
        if (a0) {
            writeU32(a0 + 4, 4);
            writeU32(a0 + 8, 20);
            writeU32(a0 + 12, 0);
            writeU32(a0 + 16, 3);
        }
        ret = a0 ? 1 : 0;
        break;
    }
    case ord(CoredllOrdinal::GetAPIAddress):
    {
        if (!a0 && !a1 && !a2 && a3 == 0x1c) {
            ret = allocate(0x38, true);
        } else {
            ret = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::IsDBCSLeadByteEx):
    {
        const uint32_t codePage = guestAnsiCodePage(a0);
        const uint8_t ch = uint8_t(a1);
        ret = (codePage == 949 && ch >= 0x81 && ch <= 0xfe) ? 1 : 0;
        break;
    }
    case ord(CoredllOrdinal::IsWctype):
    {
        ret = 0;
        break;
    }
    case ord(CoredllOrdinal::MultiByteToWideChar):
    {
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
        break;
    }
    default:
        return false;
    }

    return true;
}


std::filesystem::path SyntheticDllRuntime::ensureCrossProcessWindowRegistryPath() {
    if (!crossProcessWindowRegistryPath_.empty()) {
        return crossProcessWindowRegistryPath_;
    }
#if defined(_WIN32)
    wchar_t buffer[32768]{};
    const DWORD chars = GetEnvironmentVariableW(L"INAVI_EMU_WINDOW_REGISTRY", buffer, DWORD(std::size(buffer)));
    if (chars && chars < DWORD(std::size(buffer))) {
        crossProcessWindowRegistryPath_ = std::filesystem::path(buffer);
        return crossProcessWindowRegistryPath_;
    }
#endif
    std::filesystem::path directory = childLogDirectoryFromEnvironment();
    if (directory.empty()) {
        std::error_code ignored;
        directory = std::filesystem::temp_directory_path(ignored);
    }
    if (directory.empty()) {
        return {};
    }
    crossProcessWindowRegistryPath_ =
        directory / ("inavi_emu_windows_" + std::to_string(hostProcessId()) + ".json");
#if defined(_WIN32)
    SetEnvironmentVariableW(L"INAVI_EMU_WINDOW_REGISTRY",
                            crossProcessWindowRegistryPath_.wstring().c_str());
#endif
    return crossProcessWindowRegistryPath_;
}

void SyntheticDllRuntime::publishGuestWindowState(uint32_t hwnd) {
    auto windowIt = windows_.find(hwnd);
    if (windowIt == windows_.end()) {
        return;
    }
    auto absoluteOrigin = [&](uint32_t target) {
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
    for (const auto& [candidateHwnd, candidate] : windows_) {
        if (candidate.externalProcess) continue;
        if (candidate.destroyed) {
            ceGwe_.unregisterWindow(candidateHwnd);
            ceMgdi_.destroyWindowBitmap(candidateHwnd);
            continue;
        }
        const auto [absoluteX, absoluteY] = absoluteOrigin(candidateHwnd);
        ceGwe_.updateWindowState(candidateHwnd,
                                 candidate.ownerThread,
                                 candidate.parent,
                                 candidate.style,
                                 candidate.exStyle,
                                 absoluteX,
                                 absoluteY,
                                 candidate.width,
                                 candidate.height,
                                 candidate.visible,
                                 candidate.destroyed);
        const CeMgdi::Rect windowRect{absoluteX,
                                      absoluteY,
                                      absoluteX + candidate.width,
                                      absoluteY + candidate.height};
        const std::optional<CeMgdi::Rect> systemClip =
            candidate.visible ? std::optional<CeMgdi::Rect>{windowRect} : std::nullopt;
        ceMgdi_.updateWindowBitmap(candidateHwnd, windowRect, systemClip);
    }
    if (windowIt->second.externalProcess) {
        return;
    }
    const std::filesystem::path registryPath = ensureCrossProcessWindowRegistryPath();
    if (registryPath.empty()) {
        return;
    }
#if defined(_WIN32)
    ScopedCrossProcessMutex registryLock(L"WINDOW_REGISTRY", registryPath);
#endif

    nlohmann::json registry = nlohmann::json::object();
    {
        std::ifstream input(registryPath);
        if (input.good()) {
            try {
                input >> registry;
            } catch (const std::exception&) {
                registry = nlohmann::json::object();
            }
        }
    }
    registry["version"] = 1;
    if (!registry["windows"].is_array()) {
        registry["windows"] = nlohmann::json::array();
    }

    const uint32_t processId = hostProcessId();
    auto& windowsJson = registry["windows"];
    for (auto it = windowsJson.begin(); it != windowsJson.end();) {
        const uint32_t entryPid = it->value("processId", 0u);
        const uint32_t entryHwnd = it->value("hwnd", 0u);
        if (entryPid == processId && entryHwnd == hwnd) {
            it = windowsJson.erase(it);
        } else {
            ++it;
        }
    }

    const GuestWindow& window = windowIt->second;
    windowsJson.push_back({
        {"processId", processId},
        {"hwnd", hwnd},
        {"class", window.className},
        {"title", window.title},
        {"style", window.style},
        {"exStyle", window.exStyle},
        {"parent", window.parent},
        {"visible", window.visible},
        {"destroyed", window.destroyed},
        {"x", window.x},
        {"y", window.y},
        {"width", window.width},
        {"height", window.height},
        {"tick", window.zOrder},
    });

    std::error_code ignored;
    std::filesystem::create_directories(registryPath.parent_path(), ignored);
    const std::filesystem::path tempPath = registryPath.string() + ".tmp." + std::to_string(processId);
    {
        std::ofstream output(tempPath, std::ios::trunc);
        if (!output.good()) {
            return;
        }
        output << registry.dump(2);
    }
    std::filesystem::rename(tempPath, registryPath, ignored);
    if (ignored) {
        std::filesystem::copy_file(tempPath, registryPath,
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        std::filesystem::remove(tempPath, ignored);
    }
}

std::optional<uint32_t> SyntheticDllRuntime::findExternalGuestWindow(const std::string& className,
                                                                     const std::string& title,
                                                                     bool matchClass,
                                                                     bool matchTitle) {
    const std::filesystem::path registryPath = ensureCrossProcessWindowRegistryPath();
    if (registryPath.empty()) {
        return std::nullopt;
    }
#if defined(_WIN32)
    ScopedCrossProcessMutex registryLock(L"WINDOW_REGISTRY", registryPath);
#endif

    nlohmann::json registry;
    {
        std::ifstream input(registryPath);
        if (!input.good()) {
            return std::nullopt;
        }
        try {
            input >> registry;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (registry.value("version", 0) != 1 || !registry["windows"].is_array()) {
        return std::nullopt;
    }

    const uint32_t currentPid = hostProcessId();
    const auto& windowsJson = registry["windows"];
    for (auto it = windowsJson.rbegin(); it != windowsJson.rend(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        const uint32_t processId = it->value("processId", 0u);
        const uint32_t externalHwnd = it->value("hwnd", 0u);
        if (!processId || !externalHwnd || processId == currentPid || it->value("destroyed", false)) {
            continue;
        }
        if (!isHostProcessAlive(processId)) {
            spdlog::info("FindWindowW skipped stale external guest window remotePid={} remoteHwnd=0x{:08x}",
                         processId, externalHwnd);
            continue;
        }
        const std::string entryClass = lowerAscii(it->value("class", std::string{}));
        const std::string entryTitle = it->value("title", std::string{});
        if (matchClass && entryClass != className) {
            continue;
        }
        if (matchTitle && entryTitle != title) {
            continue;
        }

        const auto key = std::make_pair(processId, externalHwnd);
        auto existing = externalWindowHandles_.find(key);
        if (existing != externalWindowHandles_.end()) {
            auto existingWindow = windows_.find(existing->second);
            if (existingWindow != windows_.end() && !existingWindow->second.destroyed) {
                existingWindow->second.className = entryClass;
                existingWindow->second.title = entryTitle;
                existingWindow->second.visible = it->value("visible", false);
                existingWindow->second.zOrder = std::max<uint64_t>(existingWindow->second.zOrder,
                                                                   it->value("tick", 0ull));
                return existing->second;
            }
        }

        const uint32_t hwnd = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
        GuestWindow window{};
        window.hwnd = hwnd;
        window.className = entryClass;
        window.title = entryTitle;
        window.style = it->value("style", 0u);
        window.exStyle = it->value("exStyle", 0u);
        window.parent = 0;
        window.x = it->value("x", 0);
        window.y = it->value("y", 0);
        window.width = it->value("width", 0);
        window.height = it->value("height", 0);
        window.visible = it->value("visible", false);
        window.zOrder = it->value("tick", 0ull);
        window.enabled = true;
        window.externalProcess = true;
        window.externalProcessId = processId;
        window.externalHwnd = externalHwnd;
        windows_[hwnd] = window;
        ceGwe_.registerWindowOwner(hwnd, window.ownerThread);
        externalWindowHandles_[key] = hwnd;
        spdlog::info("FindWindowW imported external guest window hwnd=0x{:08x} remotePid={} remoteHwnd=0x{:08x} class=\"{}\" title=\"{}\"",
                     hwnd, processId, externalHwnd, entryClass, entryTitle);
        return hwnd;
    }
    return std::nullopt;
}

std::filesystem::path SyntheticDllRuntime::crossProcessMessageQueuePath() {
    if (!crossProcessMessageQueuePath_.empty()) {
        return crossProcessMessageQueuePath_;
    }
    const std::filesystem::path registryPath = ensureCrossProcessWindowRegistryPath();
    if (registryPath.empty()) {
        return {};
    }
    crossProcessMessageQueuePath_ = std::filesystem::path(registryPath.string() + ".messages.json");
    return crossProcessMessageQueuePath_;
}

bool SyntheticDllRuntime::postCrossProcessGuestMessage(uint32_t processId,
                                                       uint32_t hwnd,
                                                       uint32_t message,
                                                       uint32_t wParam,
                                                       uint32_t lParam) {
    if (!processId || !hwnd || processId == hostProcessId()) {
        return false;
    }
    if (!isHostProcessAlive(processId)) {
        spdlog::info("cross-process message skipped stale target remotePid={} remoteHwnd=0x{:08x} msg=0x{:08x}",
                     processId, hwnd, message);
        return false;
    }
    const std::filesystem::path queuePath = crossProcessMessageQueuePath();
    if (queuePath.empty()) {
        return false;
    }
    syncNamedMappedViews();
#if defined(_WIN32)
    ScopedCrossProcessMutex queueLock(L"MESSAGE_QUEUE", queuePath);
#endif

    nlohmann::json queue = nlohmann::json::object();
    {
        std::ifstream input(queuePath);
        if (input.good()) {
            try {
                input >> queue;
            } catch (const std::exception&) {
                queue = nlohmann::json::object();
            }
        }
    }
    queue["version"] = 1;
    if (!queue["messages"].is_array()) {
        queue["messages"] = nlohmann::json::array();
    }

    nlohmann::json messageJson = {
        {"sourceProcessId", hostProcessId()},
        {"targetProcessId", processId},
        {"targetHwnd", hwnd},
        {"message", message},
        {"wParam", wParam},
        {"lParam", lParam},
        {"time", uint32_t(++tick_ * 16)},
    };
    if (message == kWmCopyData && lParam && isGuestRangeReadable(lParam, 12)) {
        const uint32_t dwData = readU32(lParam);
        const uint32_t cbData = readU32(lParam + 4);
        const uint32_t lpData = readU32(lParam + 8);
        if (cbData <= kMaxCrossProcessCopyData &&
            (cbData == 0 || (lpData && isGuestRangeReadable(lpData, cbData)))) {
            nlohmann::json bytes = nlohmann::json::array();
            bool payloadReadable = true;
            if (cbData) {
                std::vector<uint8_t> payload(cbData);
                if (uc_mem_read(uc_, lpData, payload.data(), payload.size()) == UC_ERR_OK) {
                    for (uint8_t byte : payload) bytes.push_back(byte);
                } else {
                    payloadReadable = false;
                }
            }
            if (payloadReadable) {
                messageJson["copyData"] = {
                    {"dwData", dwData},
                    {"cbData", cbData},
                    {"bytes", std::move(bytes)},
                };
                spdlog::info("cross-process WM_COPYDATA marshalled targetPid={} targetHwnd=0x{:08x} dwData=0x{:08x} cbData={}",
                             processId, hwnd, dwData, cbData);
            } else {
                spdlog::warn("cross-process WM_COPYDATA payload read failed lParam=0x{:08x} cbData={} lpData=0x{:08x}",
                             lParam, cbData, lpData);
            }
        } else {
            spdlog::warn("cross-process WM_COPYDATA payload unreadable lParam=0x{:08x} cbData={} lpData=0x{:08x}",
                         lParam, cbData, lpData);
        }
    }
    queue["messages"].push_back(std::move(messageJson));

    std::error_code ignored;
    std::filesystem::create_directories(queuePath.parent_path(), ignored);
    const std::filesystem::path tempPath = queuePath.string() + ".tmp." + std::to_string(hostProcessId());
    {
        std::ofstream output(tempPath, std::ios::trunc);
        if (!output.good()) {
            return false;
        }
        output << queue.dump(2);
    }
    std::filesystem::rename(tempPath, queuePath, ignored);
    if (ignored) {
        std::filesystem::copy_file(tempPath, queuePath,
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        std::filesystem::remove(tempPath, ignored);
    }
    return true;
}

bool SyntheticDllRuntime::postCrossProcessBroadcastMessage(uint32_t message,
                                                           uint32_t wParam,
                                                           uint32_t lParam) {
    const std::filesystem::path registryPath = ensureCrossProcessWindowRegistryPath();
    if (registryPath.empty()) {
        return false;
    }
#if defined(_WIN32)
    ScopedCrossProcessMutex registryLock(L"WINDOW_REGISTRY", registryPath);
#endif

    nlohmann::json registry;
    {
        std::ifstream input(registryPath);
        if (!input.good()) {
            return false;
        }
        try {
            input >> registry;
        } catch (const std::exception&) {
            return false;
        }
    }
    if (registry.value("version", 0) != 1 || !registry["windows"].is_array()) {
        return false;
    }

    const uint32_t currentPid = hostProcessId();
    bool posted = false;
    for (const auto& entry : registry["windows"]) {
        if (!entry.is_object()) {
            continue;
        }
        const uint32_t processId = entry.value("processId", 0u);
        const uint32_t hwnd = entry.value("hwnd", 0u);
        if (!processId || !hwnd || processId == currentPid ||
            entry.value("destroyed", false) || entry.value("parent", 0u) != 0) {
            continue;
        }
        posted = postCrossProcessGuestMessage(processId, hwnd, message, wParam, lParam) || posted;
    }
    return posted;
}

void SyntheticDllRuntime::pollCrossProcessGuestMessages() {
    const std::filesystem::path queuePath = crossProcessMessageQueuePath();
    if (queuePath.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (lastCrossProcessMessagePollAt_ != std::chrono::steady_clock::time_point{} &&
        now - lastCrossProcessMessagePollAt_ < std::chrono::milliseconds(2)) {
        return;
    }
    lastCrossProcessMessagePollAt_ = now;

    std::error_code statError;
    const auto currentWrite = std::filesystem::last_write_time(queuePath, statError);
    if (statError) {
        hasCrossProcessMessageQueueStat_ = false;
        return;
    }
    const std::uintmax_t currentSize = std::filesystem::file_size(queuePath, statError);
    if (statError) {
        hasCrossProcessMessageQueueStat_ = false;
        return;
    }
    if (hasCrossProcessMessageQueueStat_ &&
        currentWrite == lastCrossProcessMessageQueueWrite_ &&
        currentSize == lastCrossProcessMessageQueueSize_) {
        return;
    }
#if defined(_WIN32)
    ScopedCrossProcessMutex queueLock(L"MESSAGE_QUEUE", queuePath);
#endif

    nlohmann::json queue;
    {
        std::ifstream input(queuePath);
        if (!input.good()) {
            return;
        }
        try {
            input >> queue;
        } catch (const std::exception&) {
            return;
        }
    }
    if (queue.value("version", 0) != 1 || !queue["messages"].is_array()) {
        hasCrossProcessMessageQueueStat_ = true;
        lastCrossProcessMessageQueueWrite_ = currentWrite;
        lastCrossProcessMessageQueueSize_ = currentSize;
        return;
    }
    syncNamedMappedViews();

    const uint32_t currentPid = hostProcessId();
    auto importExternalWindow = [&](uint32_t processId, uint32_t externalHwnd) -> uint32_t {
        if (!processId || !externalHwnd || processId == currentPid || !isHostProcessAlive(processId)) {
            return 0;
        }
        const auto key = std::make_pair(processId, externalHwnd);
        auto existing = externalWindowHandles_.find(key);
        if (existing != externalWindowHandles_.end()) {
            auto existingWindow = windows_.find(existing->second);
            if (existingWindow != windows_.end() && !existingWindow->second.destroyed) {
                return existing->second;
            }
        }

        const std::filesystem::path registryPath = ensureCrossProcessWindowRegistryPath();
        if (registryPath.empty()) return 0;
#if defined(_WIN32)
        ScopedCrossProcessMutex registryLock(L"WINDOW_REGISTRY", registryPath);
#endif
        nlohmann::json registry;
        {
            std::ifstream input(registryPath);
            if (!input.good()) return 0;
            try {
                input >> registry;
            } catch (const std::exception&) {
                return 0;
            }
        }
        if (registry.value("version", 0) != 1 || !registry["windows"].is_array()) {
            return 0;
        }
        for (const auto& entry : registry["windows"]) {
            if (!entry.is_object() ||
                entry.value("processId", 0u) != processId ||
                entry.value("hwnd", 0u) != externalHwnd ||
                entry.value("destroyed", false)) {
                continue;
            }

            const uint32_t hwnd = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
            GuestWindow window{};
            window.hwnd = hwnd;
            window.className = lowerAscii(entry.value("class", std::string{}));
            window.title = entry.value("title", std::string{});
            window.style = entry.value("style", 0u);
            window.exStyle = entry.value("exStyle", 0u);
            window.parent = 0;
            window.x = entry.value("x", 0);
            window.y = entry.value("y", 0);
            window.width = entry.value("width", 0);
            window.height = entry.value("height", 0);
            window.visible = entry.value("visible", false);
            window.zOrder = entry.value("tick", 0ull);
            window.enabled = true;
            window.externalProcess = true;
            window.externalProcessId = processId;
            window.externalHwnd = externalHwnd;
            windows_[hwnd] = window;
            ceGwe_.registerWindowOwner(hwnd, window.ownerThread);
            externalWindowHandles_[key] = hwnd;
            spdlog::info("imported external guest window hwnd=0x{:08x} remotePid={} remoteHwnd=0x{:08x} class=\"{}\" title=\"{}\"",
                         hwnd, processId, externalHwnd, window.className, window.title);
            return hwnd;
        }
        return 0;
    };
    bool changed = false;
    for (auto it = queue["messages"].begin(); it != queue["messages"].end();) {
        if (!it->is_object() || it->value("targetProcessId", 0u) != currentPid) {
            ++it;
            continue;
        }

        const uint32_t hwnd = it->value("targetHwnd", 0u);
        auto windowIt = windows_.find(hwnd);
        if (windowIt != windows_.end() && !windowIt->second.destroyed) {
            GuestMessage guestMessage{};
            guestMessage.hwnd = hwnd;
            guestMessage.message = it->value("message", 0u);
            guestMessage.wParam = it->value("wParam", 0u);
            guestMessage.lParam = it->value("lParam", 0u);
            guestMessage.time = it->value("time", uint32_t(++tick_ * 16));
            guestMessage.crossProcess = true;
            if (guestMessage.message == kWmCopyData && it->contains("copyData") &&
                (*it)["copyData"].is_object()) {
                const auto& copyData = (*it)["copyData"];
                const uint32_t sourcePid = it->value("sourceProcessId", 0u);
                const uint32_t sourceHwnd = guestMessage.wParam;
                const uint32_t importedSource = importExternalWindow(sourcePid, sourceHwnd);
                if (importedSource) guestMessage.wParam = importedSource;

                const uint32_t dwData = copyData.value("dwData", 0u);
                const uint32_t cbData = copyData.value("cbData", 0u);
                const auto bytesIt = copyData.find("bytes");
                if (cbData <= kMaxCrossProcessCopyData &&
                    bytesIt != copyData.end() &&
                    bytesIt->is_array() &&
                    bytesIt->size() == cbData) {
                    uint32_t payloadPtr = 0;
                    bool payloadValid = true;
                    if (cbData) {
                        std::vector<uint8_t> payload;
                        payload.reserve(cbData);
                        for (const auto& byte : *bytesIt) {
                            if (!byte.is_number_unsigned()) {
                                payloadValid = false;
                                break;
                            }
                            payload.push_back(uint8_t(byte.get<uint32_t>() & 0xffu));
                        }
                        if (payloadValid) {
                            payloadPtr = allocate(cbData, false);
                        }
                        if (payloadPtr) {
                            uc_mem_write(uc_, payloadPtr, payload.data(), payload.size());
                        } else {
                            payloadValid = false;
                        }
                    }
                    const uint32_t copyDataPtr = payloadValid ? allocate(12, true) : 0;
                    if (copyDataPtr) {
                        writeU32(copyDataPtr, dwData);
                        writeU32(copyDataPtr + 4, cbData);
                        writeU32(copyDataPtr + 8, payloadPtr);
                        guestMessage.lParam = copyDataPtr;
                        spdlog::info("cross-process WM_COPYDATA received hwnd=0x{:08x} sourcePid={} sourceHwnd=0x{:08x} localSource=0x{:08x} dwData=0x{:08x} cbData={}",
                                     hwnd, sourcePid, sourceHwnd, guestMessage.wParam, dwData, cbData);
                    } else {
                        spdlog::warn("cross-process WM_COPYDATA import failed hwnd=0x{:08x} sourcePid={} cbData={}",
                                     hwnd, sourcePid, cbData);
                    }
                } else {
                    spdlog::warn("cross-process WM_COPYDATA malformed hwnd=0x{:08x} sourcePid={} cbData={}",
                                 hwnd, sourcePid, cbData);
                }
            }
            ceGwe_.postPostedMessage(guestMessage);
            spdlog::info("cross-process message received hwnd=0x{:08x} msg=0x{:08x} fromPid={} queued={}",
                         hwnd,
                         guestMessage.message,
                         it->value("sourceProcessId", 0u),
                         ceGwe_.messageCount());
            wakeGuestThreadsWaitingForMessage();
        } else {
            spdlog::info("cross-process message dropped missing hwnd=0x{:08x} msg=0x{:08x} fromPid={}",
                         hwnd,
                         it->value("message", 0u),
                         it->value("sourceProcessId", 0u));
        }
        it = queue["messages"].erase(it);
        changed = true;
    }

    if (!changed) {
        hasCrossProcessMessageQueueStat_ = true;
        lastCrossProcessMessageQueueWrite_ = currentWrite;
        lastCrossProcessMessageQueueSize_ = currentSize;
        return;
    }
    std::error_code ignored;
    const std::filesystem::path tempPath = queuePath.string() + ".tmp." + std::to_string(currentPid);
    {
        std::ofstream output(tempPath, std::ios::trunc);
        if (!output.good()) {
            return;
        }
        output << queue.dump(2);
    }
    std::filesystem::rename(tempPath, queuePath, ignored);
    if (ignored) {
        std::filesystem::copy_file(tempPath, queuePath,
                                   std::filesystem::copy_options::overwrite_existing, ignored);
        std::filesystem::remove(tempPath, ignored);
    }
    std::error_code refreshError;
    const auto refreshedWrite = std::filesystem::last_write_time(queuePath, refreshError);
    const std::uintmax_t refreshedSize = refreshError ? 0 : std::filesystem::file_size(queuePath, refreshError);
    hasCrossProcessMessageQueueStat_ = !refreshError;
    if (hasCrossProcessMessageQueueStat_) {
        lastCrossProcessMessageQueueWrite_ = refreshedWrite;
        lastCrossProcessMessageQueueSize_ = refreshedSize;
    }
}

bool SyntheticDllRuntime::handleHostSetTimer(const GuestCallArgs& args,
                                             uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;

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
    return true;
}


bool SyntheticDllRuntime::dispatchHostWin32(uint16_t ordinal,
                                            const GuestCallArgs& args,
                                            uint32_t& ret) {
    switch (ordinal) {
    case ord(CoredllOrdinal::SetTimer): return handleHostSetTimer(args, ret);
    case ord(CoredllOrdinal::SystemParametersInfoW):
        ret = handleSystemParametersInfoW(args.a0, args.a1, args.a2, args.a3);
        return true;
    case ord(CoredllOrdinal::GetDeviceCaps):
        ret = handleGetDeviceCaps(args.a0, args.a1);
        return true;
    case ord(CoredllOrdinal::DeviceIoControl):
        ret = dispatchDeviceIoControl(args.a0, args.a1, args.a2, args.a3);
        return true;
    case ord(CoredllOrdinal::WideCharToMultiByte):
        ret = handleWideCharToMultiByte(args.a0, args.a1, args.a2, args.a3);
        return true;
    case ord(CoredllOrdinal::CreateFileMappingW):
        ret = handleCreateFileMappingW(args.a0, args.a1, args.a2, args.a3);
        return true;
    case ord(CoredllOrdinal::MapViewOfFile):
        ret = handleMapViewOfFile(args.a0, args.a1, args.a2, args.a3);
        return true;
    case ord(CoredllOrdinal::UnmapViewOfFile):
        ret = handleUnmapViewOfFile(args.a0);
        return true;
    case ord(CoredllOrdinal::FlushViewOfFile):
        ret = handleFlushViewOfFile(args.a0, args.a1);
        return true;
    default:
        break;
    }

    struct HeavyHostWin32Handler {
        uint16_t ordinal;
        bool invoke(SyntheticDllRuntime& runtime,
                    const GuestCallArgs& args,
                    uint32_t& ret) const {
            return runtime.dispatchLargeHostWin32(ordinal, args, ret);
        }
    };
    static constexpr HeavyHostWin32Handler heavyHandlers[] = {
        {ord(CoredllOrdinal::BitBlt)},
        {ord(CoredllOrdinal::StretchBlt)},
        {ord(CoredllOrdinal::GetMessageW)},
        {ord(CoredllOrdinal::GetMessageWNoWait)},
        {ord(CoredllOrdinal::PeekMessageW)},
        {ord(CoredllOrdinal::PostMessageW)},
        {ord(CoredllOrdinal::CreateWindowExW)},
        {ord(CoredllOrdinal::SetWindowPos)},
        {ord(CoredllOrdinal::ShowWindow)},
        {ord(CoredllOrdinal::TransparentImage)},
        {ord(CoredllOrdinal::StretchDIBits)},
        {ord(CoredllOrdinal::CreateProcessW)},
        {ord(CoredllOrdinal::WaitForSingleObject)},
        {ord(CoredllOrdinal::CreateDialogIndirectParamW)},
        {ord(CoredllOrdinal::KernelIoControl)},
        {ord(CoredllOrdinal::GlobalMemoryStatus)},
        {ord(CoredllOrdinal::CloseHandle)},
        {ord(CoredllOrdinal::GetModuleFileNameW)},
        {ord(CoredllOrdinal::OutputDebugStringW)},
        {ord(CoredllOrdinal::FormatMessageW)},
        {ord(CoredllOrdinal::LoadLibraryW)},
        {ord(CoredllOrdinal::GetModuleHandleW)},
        {ord(CoredllOrdinal::FreeLibrary)},
        {ord(CoredllOrdinal::GetProcAddressA)},
        {ord(CoredllOrdinal::GetProcAddressW)},
        {ord(CoredllOrdinal::RegisterClassW)},
        {ord(CoredllOrdinal::GetClassInfoW)},
        {ord(CoredllOrdinal::FindWindowW)},
        {ord(CoredllOrdinal::GetWindowRect)},
        {ord(CoredllOrdinal::GetClientRect)},
        {ord(CoredllOrdinal::InvalidateRect)},
        {ord(CoredllOrdinal::ClientToScreen)},
        {ord(CoredllOrdinal::ScreenToClient)},
        {ord(CoredllOrdinal::KillTimer)},
        {ord(CoredllOrdinal::CreatePen)},
        {ord(CoredllOrdinal::CreateSolidBrush)},
        {ord(CoredllOrdinal::CreateRectRgn)},
        {ord(CoredllOrdinal::CombineRgn)},
        {ord(CoredllOrdinal::CreateFontIndirectW)},
        {ord(CoredllOrdinal::GetStockObject)},
        {ord(CoredllOrdinal::SelectObject)},
        {ord(CoredllOrdinal::DeleteObject)},
        {ord(CoredllOrdinal::SetBkColor)},
        {ord(CoredllOrdinal::SetTextColor)},
        {ord(CoredllOrdinal::SetBkMode)},
        {ord(CoredllOrdinal::SetTextAlign)},
        {ord(CoredllOrdinal::FillRect)},
        {ord(CoredllOrdinal::PatBlt)},
        {ord(CoredllOrdinal::Ellipse)},
        {ord(CoredllOrdinal::Rectangle)},
        {ord(CoredllOrdinal::MoveToEx)},
        {ord(CoredllOrdinal::LineTo)},
        {ord(CoredllOrdinal::ExtTextOutW)},
        {ord(CoredllOrdinal::DrawTextW)},
        {ord(CoredllOrdinal::CreateCompatibleDC)},
        {ord(CoredllOrdinal::DeleteDC)},
        {ord(CoredllOrdinal::GetWindowLongW)},
        {ord(CoredllOrdinal::SetWindowLongW)},
        {ord(CoredllOrdinal::GetParent)},
        {ord(CoredllOrdinal::IsWindow)},
        {ord(CoredllOrdinal::GetWindow)},
        {ord(CoredllOrdinal::MoveWindow)},
        {ord(CoredllOrdinal::SetWindowRgn)},
        {ord(CoredllOrdinal::GetWindowRgn)},
        {ord(CoredllOrdinal::DestroyWindow)},
        {ord(CoredllOrdinal::UpdateWindow)},
        {ord(CoredllOrdinal::DefWindowProcW)},
        {ord(CoredllOrdinal::GetMessagePos)},
        {ord(CoredllOrdinal::TranslateMessage)},
        {ord(CoredllOrdinal::PostQuitMessage)},
        {ord(CoredllOrdinal::GetSystemInfo)},
        {ord(CoredllOrdinal::GetStoreInformation)},
        {ord(CoredllOrdinal::GlobalAddAtomW)},
        {ord(CoredllOrdinal::GlobalFindAtomW)},
        {ord(CoredllOrdinal::GlobalDeleteAtom)},
        {ord(CoredllOrdinal::WNetGetUserW)},
        {ord(CoredllOrdinal::WNetGetUniversalNameW)},
        {ord(CoredllOrdinal::WNetConnectionDialog1W)},
        {ord(CoredllOrdinal::IsDialogMessageW)},
        {ord(CoredllOrdinal::KernelLibIoControl)},
    };
    for (const HeavyHostWin32Handler& handler : heavyHandlers) {
        if (handler.ordinal == ordinal) return handler.invoke(*this, args, ret);
    }
    return false;
}


bool SyntheticDllRuntime::dispatchLargeHostWin32(uint16_t ordinal,
                                                 const GuestCallArgs& args,
                                                 uint32_t& ret) {

    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    const uint32_t ra = args.ra;
    const std::string name = "coredll.ordinal";
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
    switch (ordinal) {
    case ord(CoredllOrdinal::CreateDialogIndirectParamW):
    {
        auto read16 = [&](uint32_t address) -> uint16_t {
            uint16_t value = 0;
            if (address) uc_mem_read(uc_, address, &value, sizeof(value));
            return value;
        };
        auto readI16 = [&](uint32_t address) -> int16_t {
            return int16_t(read16(address));
        };
        auto skipTemplateString = [&](uint32_t address) -> uint32_t {
            const uint16_t first = read16(address);
            if (first == 0xffffu) return address + 4;
            uint32_t cursor = address;
            for (uint32_t i = 0; i < 512; ++i, cursor += 2) {
                if (read16(cursor) == 0) return cursor + 2;
            }
            return cursor;
        };
        auto readTemplateString = [&](uint32_t address) -> std::string {
            const uint16_t first = read16(address);
            if (first == 0xffffu) {
                std::ostringstream oss;
                oss << "#" << read16(address + 2);
                return oss.str();
            }
            std::string result;
            uint32_t cursor = address;
            for (uint32_t i = 0; i < 512; ++i, cursor += 2) {
                const uint16_t ch = read16(cursor);
                if (!ch) break;
                result.push_back(ch < 0x80 ? char(ch) : '?');
            }
            return result;
        };

        const uint32_t instance = a0;
        const uint32_t templ = a1;
        const uint32_t parent = a2;
        const uint32_t dlgProc = a3;
        const uint32_t initParam = stackArg(4);
        if (!templ || !isGuestRangeReadable(templ, 18)) {
            ret = 0;
            lastError_ = 87;
            return true;
        }

        const uint32_t style = readU32(templ);
        const uint32_t exStyle = readU32(templ + 4);
        const uint16_t itemCount = read16(templ + 8);
        const int32_t x = readI16(templ + 10);
        const int32_t y = readI16(templ + 12);
        int32_t width = readI16(templ + 14);
        int32_t height = readI16(templ + 16);
        uint32_t cursor = templ + 18;
        const std::string menuName = readTemplateString(cursor);
        cursor = skipTemplateString(cursor);
        const std::string dialogClass = readTemplateString(cursor);
        cursor = skipTemplateString(cursor);
        const std::string title = readTemplateString(cursor);
        if (width <= 0) width = framebufferWidth_ > 0 ? framebufferWidth_ : 800;
        if (height <= 0) height = framebufferHeight_ > 0 ? framebufferHeight_ : 480;

        ret = makeGuestHandle({GuestHandle::Kind::GuestWindow, 0, 0});
        GuestWindow window{};
        window.hwnd = ret;
        window.className = dialogClass.empty() || dialogClass == "#0" ? "#32770" : lowerAscii(dialogClass);
        window.title = title;
        window.style = style;
        window.exStyle = exStyle;
        window.parent = parent;
        window.instance = instance;
        window.param = initParam;
        // A dialog proc is not a normal window proc; dispatching directly into
        // MFC's dialog thunk without CE dialog-manager state can corrupt the
        // caller. Keep the HWND real and visible to FindWindow/message code,
        // then let later evidence drive a fuller dialog-manager shim.
        window.wndProc = 0;
        window.ownerThread = ceKernel_.activeGuestThread() ? ceKernel_.activeGuestThread() : ceKernel_.mainThreadPseudoHandle();
        window.zOrder = nextWindowZOrder();
        window.x = x;
        window.y = y;
        window.width = width;
        window.height = height;
        window.visible = (style & 0x10000000u) != 0;
        windows_[ret] = window;
        ceGwe_.registerWindowOwner(ret, window.ownerThread);
        ensureHostWindow(ret, windows_[ret]);
        publishGuestWindowState(ret);

        GuestMessage size{};
        size.hwnd = ret;
        size.message = 0x0005; // WM_SIZE
        size.lParam = (uint32_t(uint16_t(height)) << 16) | uint16_t(width);
        size.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(size);
        lastError_ = 0;
        spdlog::info("CreateDialogIndirectParamW guest=0x{:08x} class=\"{}\" title=\"{}\" parent=0x{:08x} "
                     "style=0x{:08x} ex=0x{:08x} dlgproc=0x{:08x} init=0x{:08x} rect={},{} {}x{} items={} menu=\"{}\"",
                     ret, windows_[ret].className, title, parent, style, exStyle, dlgProc, initParam,
                     x, y, width, height, itemCount, menuName);
        return true;
    }
    case ord(CoredllOrdinal::IsDialogMessageW):
    {
        // CE/MFC uses this in PreTranslateMessage. The emulator queues and
        // dispatches messages itself, so only report that the message was not
        // consumed by dialog navigation.
        ret = 0;
        lastError_ = windows_.count(a0) || !a0 ? 0 : 1400;
        return true;
    }
    case ord(CoredllOrdinal::KernelIoControl):
    {
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
        break;
    }
    case ord(CoredllOrdinal::KernelLibIoControl):
    {
        lastError_ = 120;
        ret = 0;
        break;
    }
    case ord(CoredllOrdinal::GlobalMemoryStatus):
    {
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
        break;
    }
    case ord(CoredllOrdinal::CloseHandle):
    {
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
        break;
    }
    case ord(CoredllOrdinal::WaitForSingleObject):
    {
        auto* handle = lookupGuestHandle(a0);
        const auto isHostWaitable = [](const GuestHandle& guestHandle) {
            if (!guestHandle.hostValue) return false;
            return guestHandle.kind == GuestHandle::Kind::HostEvent ||
                   guestHandle.kind == GuestHandle::Kind::HostMutex ||
                   guestHandle.kind == GuestHandle::Kind::GuestProcess ||
                   guestHandle.kind == GuestHandle::Kind::GuestThread;
        };
        const auto handleKindName = [](GuestHandle::Kind kind) -> const char* {
            switch (kind) {
            case GuestHandle::Kind::HostEvent: return "event";
            case GuestHandle::Kind::HostMutex: return "mutex";
            case GuestHandle::Kind::GuestProcess: return "process";
            case GuestHandle::Kind::GuestThread: return "thread";
            default: return "handle";
            }
        };
#if defined(_WIN32)
        if (handle && isHostWaitable(*handle) && a1 != 0) {
            const uint64_t start = hostTickMilliseconds();
            const bool infinite = a1 == INFINITE;
            auto hostWaitProbe = [](const GuestHandle& guestHandle) {
                const DWORD wait = ::WaitForSingleObject(reinterpret_cast<HANDLE>(guestHandle.hostValue), 0);
                if (wait == WAIT_OBJECT_0) return CeKernel::HostWaitResult{true, false, 0};
                if (wait == WAIT_TIMEOUT) return CeKernel::HostWaitResult{false, false, 0};
                return CeKernel::HostWaitResult{false, true, GetLastError()};
            };
            for (;;) {
                refreshCompletedHostWaveBuffers();
                const CeKernel::WaitQueryResult wait =
                    ceKernel_.queryWaitObject(a0, hostWaitProbe, true);
                if (wait.result != CeKernel::kWaitTimeout) {
                    ret = wait.result;
                    lastError_ = wait.error;
                    break;
                }
                const uint64_t elapsed = hostTickMilliseconds() - start;
                if (!infinite && elapsed >= a1) {
                    ret = CeKernel::kWaitTimeout;
                    lastError_ = 0;
                    break;
                }
                pumpHostMessages();
                const DWORD sleepMs = infinite
                    ? 5u
                    : DWORD(std::max<uint64_t>(1, std::min<uint64_t>(5, a1 - elapsed)));
                ::Sleep(sleepMs);
            }
            const uint64_t elapsed = hostTickMilliseconds() - start;
            if (elapsed >= 250) {
                spdlog::info("WaitForSingleObject cooperative host wait handle=0x{:08x} kind={} timeout=0x{:08x} -> 0x{:08x} elapsedMs={}",
                             a0, handleKindName(handle->kind), a1, ret, elapsed);
            }
        } else
#endif
        if (handle && (handle->kind == GuestHandle::Kind::GuestThread ||
                       (handle->kind == GuestHandle::Kind::GuestProcess && !handle->hostValue))) {
            const CeKernel::WaitQueryResult wait = ceKernel_.queryWaitObject(a0, {}, true);
            ret = wait.result;
            lastError_ = wait.error;
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
        break;
    }
    case ord(CoredllOrdinal::CreateProcessW):
    {
        const std::string application = readUtf16(a0, 2048);
        const std::string commandLine = readUtf16(a1, 4096);
        const uint32_t processInfo = stackArg(9);
        const std::filesystem::path hostApplication = application.empty()
            ? std::filesystem::path{}
            : resolveGuestPath(application);
        spdlog::info("CreateProcessW callsite ra=0x{:08x} app=\"{}\" cmd=\"{}\" pi=0x{:08x}",
                     args.ra, application, commandLine, processInfo);
        const bool shouldRunInRuntime = inRuntimeChildProcessesEnabled() && !hostApplication.empty();
        if (!application.empty() && (hostApplication.empty() || !std::filesystem::exists(hostApplication))) {
            lastError_ = 2;
            ret = 0;
        } else {
            bool launchedInRuntime = false;
            if (guestProcessLauncher_ && shouldRunInRuntime) {
                GuestProcessLaunch launch;
                launch.hostApplication = hostApplication;
                launch.guestApplication = application;
                launch.commandLine = commandLine;
                if (guestProcessLauncher_(launch)) {
                    if (processInfo) {
                        writeU32(processInfo, launch.processHandle);
                        writeU32(processInfo + 4, launch.threadHandle);
                        writeU32(processInfo + 8, launch.processId);
                        writeU32(processInfo + 12, launch.threadId);
                    }
                    lastError_ = 0;
                    ret = 1;
                    launchedInRuntime = true;
                } else {
                    spdlog::info("CreateProcessW shared guest launch unavailable app=\"{}\" host=\"{}\"; falling back to separate process",
                                 application, pathToUtf8(hostApplication));
                }
            }
            if (!launchedInRuntime && !realChildProcessesEnabled()) {
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
                spdlog::info("CreateProcessW synthetic child placeholder app=\"{}\" process=0x{:08x} thread=0x{:08x}",
                             application, processHandle, threadHandle);
            } else if (!launchedInRuntime) {
#if defined(_WIN32)
            wchar_t emulatorPath[MAX_PATH]{};
            const DWORD emulatorPathChars = GetModuleFileNameW(nullptr, emulatorPath, DWORD(std::size(emulatorPath)));
            if (!emulatorPathChars || emulatorPathChars >= DWORD(std::size(emulatorPath))) {
                lastError_ = GetLastError();
                ret = 0;
            } else {
                std::vector<std::wstring> childArgs;
                childArgs.emplace_back(emulatorPath);
                childArgs.push_back(hostApplication.wstring());
                if (!sdmmcHostRoot_.empty()) {
                    childArgs.emplace_back(L"--sdmmc-path");
                    childArgs.push_back(std::filesystem::absolute(sdmmcHostRoot_).wstring());
                }
                if (!registryPath_.empty()) {
                    childArgs.emplace_back(L"--registry");
                    childArgs.push_back(std::filesystem::absolute(registryPath_).wstring());
                }
                if (!serialDeviceMapPath_.empty()) {
                    childArgs.emplace_back(L"--serial-map");
                    childArgs.push_back(std::filesystem::absolute(serialDeviceMapPath_).wstring());
                }
                if (!commandLine.empty()) {
                    childArgs.emplace_back(L"--guest-command-line");
                    childArgs.push_back(widenUtf8Lossy(commandLine));
                }
                childArgs.emplace_back(L"--instructions");
                childArgs.emplace_back(L"250000000");
                childArgs.emplace_back(L"--headless");

                std::wstring hostCommandLine = buildCommandLine(childArgs);
                const std::filesystem::path childLogDir = childLogDirectoryFromEnvironment();
                const bool captureChildLog = !childLogDir.empty();
                std::vector<std::wstring> childEnvironmentEntries;
                const std::filesystem::path windowRegistryPath = ensureCrossProcessWindowRegistryPath();
                if (!windowRegistryPath.empty()) {
                    childEnvironmentEntries.push_back(L"INAVI_EMU_WINDOW_REGISTRY=" +
                                                      windowRegistryPath.wstring());
                }
                std::vector<wchar_t> childEnvironment =
                    buildQuietChildEnvironmentBlock(captureChildLog, childEnvironmentEntries);
                STARTUPINFOW startup{};
                startup.cb = sizeof(startup);
                startup.dwFlags = STARTF_USESHOWWINDOW;
                startup.wShowWindow = SW_HIDE;
                HANDLE childStdin = nullptr;
                HANDLE childStdout = nullptr;
                HANDLE childStderr = nullptr;
                if (captureChildLog) {
                    std::error_code ignored;
                    std::filesystem::create_directories(childLogDir, ignored);
                    SECURITY_ATTRIBUTES sa{};
                    sa.nLength = sizeof(sa);
                    sa.bInheritHandle = TRUE;
                    static uint32_t childLogSequence = 0;
                    const uint32_t sequence = ++childLogSequence;
                    const std::wstring stem = sanitizeLogFileStem(hostApplication.filename().wstring());
                    const std::wstring prefix = L"child_" + std::to_wstring(GetCurrentProcessId()) +
                                                L"_" + std::to_wstring(sequence) + L"_" + stem;
                    const std::filesystem::path stdoutPath = childLogDir / (prefix + L".stdout.log");
                    const std::filesystem::path stderrPath = childLogDir / (prefix + L".stderr.log");
                    childStdin = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                             &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    childStdout = CreateFileW(stdoutPath.wstring().c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    childStderr = CreateFileW(stderrPath.wstring().c_str(), GENERIC_WRITE,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (childStdin == INVALID_HANDLE_VALUE) childStdin = nullptr;
                    if (childStdout == INVALID_HANDLE_VALUE) childStdout = nullptr;
                    if (childStderr == INVALID_HANDLE_VALUE) childStderr = nullptr;
                    if (childStdout || childStderr) {
                        startup.dwFlags |= STARTF_USESTDHANDLES;
                        startup.hStdInput = childStdin ? childStdin : GetStdHandle(STD_INPUT_HANDLE);
                        startup.hStdOutput = childStdout ? childStdout : GetStdHandle(STD_OUTPUT_HANDLE);
                        startup.hStdError = childStderr ? childStderr : GetStdHandle(STD_ERROR_HANDLE);
                        spdlog::info("CreateProcessW child log capture app=\"{}\" stdout=\"{}\" stderr=\"{}\"",
                                     application, pathToUtf8(stdoutPath), pathToUtf8(stderrPath));
                    }
                }
                PROCESS_INFORMATION hostPi{};
                std::wstring workingDirectory = hostApplication.parent_path().wstring();
                BOOL ok = CreateProcessW(emulatorPath,
                                         hostCommandLine.data(),
                                         nullptr,
                                         nullptr,
                                         captureChildLog && (childStdout || childStderr),
                                         CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                         childEnvironment.empty() ? nullptr : childEnvironment.data(),
                                          workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                                          &startup,
                                          &hostPi);
                if (childStdin) CloseHandle(childStdin);
                if (childStdout) CloseHandle(childStdout);
                if (childStderr) CloseHandle(childStderr);
                if (!ok) {
                    lastError_ = GetLastError();
                    ret = 0;
                    spdlog::warn("CreateProcessW child emulator launch failed app=\"{}\" host=\"{}\" lastError={}",
                                 application, pathToUtf8(hostApplication), lastError_);
                } else {
                    const uint32_t processHandle = makeGuestHandle({
                        GuestHandle::Kind::GuestProcess,
                        reinterpret_cast<uintptr_t>(hostPi.hProcess),
                        hostPi.dwProcessId,
                    });
                    const uint32_t threadHandle = makeGuestHandle({
                        GuestHandle::Kind::GuestThread,
                        reinterpret_cast<uintptr_t>(hostPi.hThread),
                        hostPi.dwThreadId,
                    });
                    if (processInfo) {
                        writeU32(processInfo, processHandle);
                        writeU32(processInfo + 4, threadHandle);
                        writeU32(processInfo + 8, processHandle);
                        writeU32(processInfo + 12, threadHandle);
                    }
                    lastError_ = 0;
                    ret = 1;
                    spdlog::info("CreateProcessW launched child emulator app=\"{}\" pid={} tid={} process=0x{:08x} thread=0x{:08x}",
                                 application, hostPi.dwProcessId, hostPi.dwThreadId, processHandle, threadHandle);
                }
            }
#else
            lastError_ = 120;
            ret = 0;
#endif
#if !defined(_WIN32)
            (void)processInfo;
#endif
            }
        }
        if (!ret && processInfo) {
            writeU32(processInfo, 0);
            writeU32(processInfo + 4, 0);
            writeU32(processInfo + 8, 0);
            writeU32(processInfo + 12, 0);
        }
        spdlog::info("CreateProcessW app=\"{}\" host=\"{}\" cmd=\"{}\" pi=0x{:08x} -> {} lastError={}",
                     application, pathToUtf8(hostApplication), commandLine, processInfo, ret, lastError_);
        break;
    }
    case ord(CoredllOrdinal::GetModuleFileNameW):
    {
        std::string modulePath = currentProcessModulePath();
        if (a0) {
            if (a0 == currentProcessModuleBase()) {
                modulePath = currentProcessModulePath();
            } else {
                const auto module = loadedModulesByBase_.find(a0);
                modulePath = module != loadedModulesByBase_.end() ? pathToUtf8(module->second.path) : mainModulePath_;
            }
        }
        ret = writeUtf16(a1, modulePath, a2);
        lastError_ = ret ? 0 : 122;
        spdlog::info("GetModuleFileNameW module=0x{:08x} path=\"{}\" chars={} lastError={}",
                     a0, modulePath, ret, lastError_);
        break;
    }
    case ord(CoredllOrdinal::OutputDebugStringW):
    {
        spdlog::info("OutputDebugStringW: {}", readUtf16(a0));
        ret = 0;
        break;
    }
    case ord(CoredllOrdinal::FormatMessageW):
    {
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
        break;
    }
    case ord(CoredllOrdinal::LoadLibraryW):
    case ord(CoredllOrdinal::GetModuleHandleW):
    {
        bool loadLibraryCall = false;
        switch (ordinal) {
        case ord(CoredllOrdinal::LoadLibraryW): loadLibraryCall = true; break;
        default: break;
        }
        const std::string requested = readUtf16(a0);
        const std::string pathKey = lowerAscii(requested);
        const std::string nameKey = lowerAscii(pathToUtf8(pathFromUtf8(requested).filename()));
        if (requested.empty()) {
            ret = currentProcessModuleBase();
        } else if (auto it = loadedModulesByPath_.find(pathKey); it != loadedModulesByPath_.end()) {
            ret = it->second.base;
        } else if (auto it = loadedModulesByName_.find(nameKey); it != loadedModulesByName_.end()) {
            ret = it->second.base;
        } else if (loadLibraryCall) {
            if (auto syntheticModule = createModule(nameKey)) {
                registerLoadedModule(syntheticModule->moduleName,
                                     std::filesystem::path("[synthetic]") / syntheticModule->moduleName,
                                     syntheticModule->imageBase,
                                     syntheticModule->imageSize,
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
        break;
    }
    case ord(CoredllOrdinal::FreeLibrary):
    {
        // Keep loaded images resident for the emulator lifetime. CE callers use
        // FreeLibrary on modules they dynamically probe; success here preserves
        // loader semantics without invalidating already-bound guest code.
        ret = loadedModulesByBase_.count(a0) ? 1 : 0;
        lastError_ = ret ? 0 : 126;
        spdlog::info("FreeLibrary module=0x{:08x} -> {}", a0, ret);
        break;
    }
    case ord(CoredllOrdinal::GetProcAddressA):
    case ord(CoredllOrdinal::GetProcAddressW):
    {
        bool wideProcName = false;
        switch (ordinal) {
        case ord(CoredllOrdinal::GetProcAddressW): wideProcName = true; break;
        default: break;
        }
        ret = 0;
        auto module = loadedModulesByBase_.find(a0);
        if (module != loadedModulesByBase_.end()) {
            if (a1 < 0x10000) {
                auto ordinal = module->second.exportsByOrdinal.find(uint16_t(a1));
                if (ordinal != module->second.exportsByOrdinal.end()) ret = module->second.base + ordinal->second;
                spdlog::info("{} module=0x{:08x} ordinal={} -> 0x{:08x}",
                             name, a0, a1, ret);
            } else {
                const std::string proc = wideProcName ? readUtf16(a1) : readAscii(a1, 256);
                auto exported = module->second.exportsByName.find(lowerAscii(proc));
                if (exported != module->second.exportsByName.end()) ret = module->second.base + exported->second;
                spdlog::info("{} module=0x{:08x} proc=\"{}\" -> 0x{:08x}",
                             name, a0, proc, ret);
            }
        }
        lastError_ = ret ? 0 : 127;
        break;
    }
    case ord(CoredllOrdinal::RegisterClassW):
    {
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
        break;
    }
    case ord(CoredllOrdinal::GetClassInfoW):
    {
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
        break;
    }
    case ord(CoredllOrdinal::FindWindowW):
    {
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
        if (!ret) {
            if (auto external = findExternalGuestWindow(className, title, a0 != 0, a1 != 0)) {
                ret = *external;
            }
        }
        lastError_ = ret ? 0 : 1400;
        spdlog::info("FindWindowW class=\"{}\" title=\"{}\" -> 0x{:08x} lastError={}",
                     a0 ? className : std::string{}, a1 ? title : std::string{}, ret, lastError_);
        break;
    }
    case ord(CoredllOrdinal::CreateWindowExW):
    {
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
        window.ownerThread = ceKernel_.activeGuestThread() ? ceKernel_.activeGuestThread() : ceKernel_.mainThreadPseudoHandle();
        window.zOrder = nextWindowZOrder();
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
        ceGwe_.registerWindowOwner(ret, window.ownerThread);
        ensureHostWindow(ret, windows_[ret]);
        publishGuestWindowState(ret);
        GuestMessage size{};
        size.hwnd = ret;
        size.message = 0x0005; // WM_SIZE
        size.lParam = (uint32_t(uint16_t(window.height)) << 16) | uint16_t(window.width);
        size.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(size);
        lastError_ = 0;
        break;
    }
    case ord(CoredllOrdinal::GetWindowRect):
    {
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
        break;
    }
    case ord(CoredllOrdinal::GetClientRect):
    {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            writeGuestRect(a1, 0, 0, it->second.width, it->second.height);
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::InvalidateRect):
    {
        const uint32_t target = a0 ? a0 : firstWindow();
        if (!target || !windows_.count(target)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            if (a1) {
                int32_t left = 0;
                int32_t top = 0;
                int32_t right = 0;
                int32_t bottom = 0;
                if (readGuestRect(a1, left, top, right, bottom)) {
                    ceGwe_.invalidateWindow(target, CeGwe::Rect{left, top, right, bottom});
                } else {
                    ceGwe_.invalidateWindow(target);
                }
            } else {
                ceGwe_.invalidateWindow(target);
            }
            queueGuestPaint(target, a2 != 0);
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::ClientToScreen):
    case ord(CoredllOrdinal::ScreenToClient):
    {
        auto it = windows_.find(a0);
        if (!a1 || it == windows_.end()) {
            lastError_ = it == windows_.end() ? 1400 : 87;
            ret = 0;
        } else {
            int32_t x = int32_t(readU32(a1));
            int32_t y = int32_t(readU32(a1 + 4));
            const auto [originX, originY] = windowOrigin(a0);
            switch (ordinal) {
            case ord(CoredllOrdinal::ClientToScreen):
                writeU32(a1, uint32_t(x + originX));
                writeU32(a1 + 4, uint32_t(y + originY));
                break;
            default:
                writeU32(a1, uint32_t(x - originX));
                writeU32(a1 + 4, uint32_t(y - originY));
                break;
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::KillTimer):
    {
        if (a0 && !windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
        } else {
            timers_.erase(guestTimerKey(a0, a1));
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::CreatePen):
    {
        ret = makeGuestPen(a0, a1, a2);
        lastError_ = 0;
        break;
    }
    case ord(CoredllOrdinal::CreateSolidBrush):
    {
        ret = makeGuestBrush(a0);
        lastError_ = 0;
        break;
    }
    case ord(CoredllOrdinal::CreateRectRgn):
    {
#if defined(_WIN32)
        HRGN region = CreateRectRgn(int(a0), int(a1), int(a2), int(a3));
        ret = region ? makeGuestHandle({GuestHandle::Kind::HostRegion, reinterpret_cast<uintptr_t>(region), 0}) : 0;
        lastError_ = ret ? 0 : GetLastError();
#else
        ret = makeGuestHandle({GuestHandle::Kind::HostRegion, 0, 0});
        lastError_ = ret ? 0 : 8;
#endif
        break;
    }
    case ord(CoredllOrdinal::CombineRgn):
    {
#if defined(_WIN32)
        auto dest = ceKernel_.handles().find(a0);
        auto src1 = ceKernel_.handles().find(a1);
        auto src2 = ceKernel_.handles().find(a2);
        const bool needSrc2 = a3 != RGN_COPY;
        if (dest == ceKernel_.handles().end() || src1 == ceKernel_.handles().end() ||
            dest->second.kind != GuestHandle::Kind::HostRegion ||
            src1->second.kind != GuestHandle::Kind::HostRegion ||
            !dest->second.hostValue || !src1->second.hostValue ||
            (needSrc2 && (src2 == ceKernel_.handles().end() ||
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
        break;
    }
    case ord(CoredllOrdinal::CreateFontIndirectW):
    {
        std::array<uint8_t, 92> font{};
        if (!a0 || uc_mem_read(uc_, a0, font.data(), font.size()) != UC_ERR_OK) {
            lastError_ = 87;
            ret = 0;
        } else {
            ret = makeGuestFont(font);
            lastError_ = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::GetStockObject):
    {
        ret = makeStockObject(int32_t(a0));
        lastError_ = ret ? 0 : 87;
        break;
    }
    case ord(CoredllOrdinal::SelectObject):
    {
        GuestDc* dc = lookupGuestDc(a0);
        auto object = ceKernel_.handles().find(a1);
        if (!dc || object == ceKernel_.handles().end()) {
            lastError_ = 6;
            ret = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestBrush) {
            ret = dc->selectedBrush;
            dc->selectedBrush = a1;
            ceMgdi_.setSelectedBrush(a0, a1);
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestPen) {
            ret = dc->selectedPen;
            dc->selectedPen = a1;
            ceMgdi_.setSelectedPen(a0, a1);
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::GuestFont) {
            ret = dc->selectedFont;
            dc->selectedFont = a1;
            ceMgdi_.setSelectedFont(a0, a1);
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostBitmap && ceMgdi_.bitmapState(a1)) {
            ret = dc->selectedBitmap;
            dc->selectedBitmap = a1;
            ceMgdi_.setSelectedBitmap(a0, a1);
            lastError_ = 0;
        } else {
            lastError_ = 6;
            ret = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::DeleteObject):
    {
        auto object = ceKernel_.handles().find(a0);
        if (object == ceKernel_.handles().end()) {
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
            ceKernel_.handles().erase(object);
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
            ceKernel_.handles().erase(object);
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
            ceKernel_.handles().erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostBitmap) {
            const CeMgdi::BitmapState* bitmapState = ceMgdi_.bitmapState(a0);
            if (bitmapState && bitmapState->stock) {
                ret = 1;
                lastError_ = 0;
                return true;
            }
#if defined(_WIN32)
            if (object->second.hostValue) DeleteObject(reinterpret_cast<HGDIOBJ>(object->second.hostValue));
#endif
            ceMgdi_.destroyBitmap(a0);
            bitmaps_.erase(a0);
            if (object->second.filePointer) releaseAllocation(object->second.filePointer);
            ceKernel_.handles().erase(object);
            ret = 1;
            lastError_ = 0;
        } else if (object->second.kind == GuestHandle::Kind::HostRegion) {
#if defined(_WIN32)
            if (object->second.hostValue) DeleteObject(reinterpret_cast<HRGN>(object->second.hostValue));
#endif
            ceKernel_.handles().erase(object);
            ret = 1;
            lastError_ = 0;
        } else {
            lastError_ = 6;
            ret = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::SetBkColor):
    case ord(CoredllOrdinal::SetTextColor):
    case ord(CoredllOrdinal::SetBkMode):
    case ord(CoredllOrdinal::SetTextAlign):
    {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else {
            switch (ordinal) {
            case ord(CoredllOrdinal::SetBkColor):
                ret = dc->bkColor;
                dc->bkColor = a1;
                ceMgdi_.setBkColor(a0, a1);
                break;
            case ord(CoredllOrdinal::SetTextColor):
                ret = dc->textColor;
                dc->textColor = a1;
                ceMgdi_.setTextColor(a0, a1);
                break;
            case ord(CoredllOrdinal::SetBkMode):
                ret = dc->bkMode;
                dc->bkMode = a1;
                ceMgdi_.setBkMode(a0, a1);
                break;
            default:
                ret = dc->textAlign;
                dc->textAlign = a1;
                ceMgdi_.setTextAlign(a0, a1);
                break;
            }
            lastError_ = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::Ellipse):
    {
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
            const int32_t width = right - left;
            const int32_t height = bottom - top;
            if (width <= 0 || height <= 0) {
                lastError_ = 0;
                ret = 1;
                break;
            }

            constexpr int32_t kEllipseSegments = 72;
            constexpr double kTwoPi = 6.28318530717958647692;
            const double centerX = (double(left) + double(right - 1)) * 0.5;
            const double centerY = (double(top) + double(bottom - 1)) * 0.5;
            const double radiusX = double(width) * 0.5;
            const double radiusY = double(height) * 0.5;
            std::vector<std::pair<int32_t, int32_t>> points;
            points.reserve(kEllipseSegments);
            for (int32_t i = 0; i < kEllipseSegments; ++i) {
                const double angle = kTwoPi * double(i) / double(kEllipseSegments);
                points.emplace_back(int32_t(std::lround(centerX + std::cos(angle) * radiusX)),
                                    int32_t(std::lround(centerY + std::sin(angle) * radiusY)));
            }

            if (brush != brushes_.end() && brush->second.colorRef != 0xffffffffu) {
                fillDcPolygon(*dc, points, colorRefToPixel(brush->second.colorRef));
            }
            if (pen != pens_.end() && pen->second.style != 5 && pen->second.colorRef != 0xffffffffu) {
                const uint32_t pixel = colorRefToPixel(pen->second.colorRef);
                for (size_t index = 0; index < points.size(); ++index) {
                    const auto& from = points[index];
                    const auto& to = points[(index + 1) % points.size()];
                    drawDcLine(*dc, from.first, from.second, to.first, to.second, pixel);
                }
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::FillRect):
    {
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
            if (std::abs(right - left) >= 200 && std::abs(bottom - top) >= 120) {
                spdlog::info("FillRect large dc=0x{:08x} hwnd=0x{:08x} bitmap=0x{:08x} rect={},{}..{},{} color=0x{:08x}",
                             a0, dc->hwnd, dc->selectedBitmap, left, top, right, bottom, brush->second.colorRef);
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::PatBlt):
    {
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
            if (std::abs(int32_t(a3)) >= 200 && std::abs(int32_t(stackArg(4))) >= 120) {
                spdlog::info("PatBlt large dc=0x{:08x} hwnd=0x{:08x} bitmap=0x{:08x} rect={},{} {}x{} color=0x{:08x} rop=0x{:08x}",
                             a0, dc->hwnd, dc->selectedBitmap, int32_t(a1), int32_t(a2),
                             int32_t(a3), int32_t(stackArg(4)), brush->second.colorRef, rop);
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::Rectangle):
    {
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
        break;
    }
    case ord(CoredllOrdinal::MoveToEx):
    {
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
            ceMgdi_.setCurrentPosition(a0, dc->x, dc->y);
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::LineTo):
    {
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
            ceMgdi_.setCurrentPosition(a0, dc->x, dc->y);
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::StretchDIBits):
    {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0xffffffffu;
        } else {
            const bool supported = stackArg(11) == 0 && stackArg(12) == 0x00cc0020u;
            const CeMgdi::DcState* dcState = ceMgdi_.dcState(a0);
            const uint32_t selectedBitmap = dcState ? dcState->selectedBitmap : dc->selectedBitmap;
            auto dstBitmap = bitmaps_.find(selectedBitmap);
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
                if (std::abs(int32_t(a3)) >= 200 || std::abs(int32_t(stackArg(4))) >= 120 || dc->hwnd) {
                    spdlog::info("StretchDIBits ok dst=0x{:08x} hwnd=0x{:08x} dstBitmap=0x{:08x} dst={},{} {}x{} srcOrigin={},{} src={}x{} bits=0x{:08x} info=0x{:08x}",
                                 a0, dc->hwnd, selectedBitmap, int32_t(a1), int32_t(a2),
                                 int32_t(a3), int32_t(stackArg(4)), int32_t(stackArg(5)),
                                 int32_t(stackArg(6)), int32_t(stackArg(7)), int32_t(stackArg(8)),
                                 stackArg(9), stackArg(10));
                }
                ret = uint32_t(std::abs(int32_t(stackArg(8))));
                lastError_ = 0;
            } else {
                spdlog::info("StretchDIBits failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dst={}x{} src={}x{} srcOrigin={},{} bits=0x{:08x} info=0x{:08x} "
                             "usage={} rop=0x{:08x}",
                             a0, selectedBitmap, int32_t(a3), int32_t(stackArg(4)),
                             int32_t(stackArg(7)), int32_t(stackArg(8)), int32_t(stackArg(5)),
                             int32_t(stackArg(6)), stackArg(9), stackArg(10), stackArg(11), stackArg(12));
                lastError_ = 120;
                ret = 0xffffffffu;
            }
        }
        break;
    }
    case ord(CoredllOrdinal::TransparentImage):
    {
        GuestDc* dstDc = lookupGuestDc(a0);
        GuestDc* srcDc = lookupGuestDc(stackArg(5));
        const CeMgdi::DcState* dstDcState = dstDc ? ceMgdi_.dcState(a0) : nullptr;
        const CeMgdi::DcState* srcDcState = srcDc ? ceMgdi_.dcState(stackArg(5)) : nullptr;
        const uint32_t dstSelectedBitmap = dstDcState ? dstDcState->selectedBitmap
                                                      : (dstDc ? dstDc->selectedBitmap : 0);
        const uint32_t srcSelectedBitmap = srcDcState ? srcDcState->selectedBitmap
                                                      : (srcDc ? srcDc->selectedBitmap : 0);
        auto srcBitmap = srcDc ? bitmaps_.find(srcSelectedBitmap) : bitmaps_.end();
        auto dstBitmap = dstDc ? bitmaps_.find(dstSelectedBitmap) : bitmaps_.end();
        const int32_t dstH = int32_t(stackArg(4));
        const int32_t srcX = int32_t(stackArg(6));
        const int32_t srcY = int32_t(stackArg(7));
        const int32_t srcW = int32_t(stackArg(8));
        const int32_t srcH = int32_t(stackArg(9));
        const uint32_t transparentColor = stackArg(10);
        if (!dstDc || !srcDc || srcBitmap == bitmaps_.end()) {
            spdlog::info("TransparentImage unsupported dst=0x{:08x} dstBitmap=0x{:08x} src=0x{:08x} "
                         "srcBitmap=0x{:08x} dst={}x{} src={}x{} srcOrigin={},{} color=0x{:08x}",
                         a0, dstSelectedBitmap, stackArg(5),
                         srcSelectedBitmap, int32_t(a3), dstH,
                         srcW, srcH, srcX, srcY, transparentColor);
            lastError_ = dstDc && srcDc ? 120 : 6;
            ret = 0;
        } else if (int32_t(a3) == 0 || dstH == 0 || srcW == 0 || srcH == 0) {
            lastError_ = 0;
            ret = 1;
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
                spdlog::debug("TransparentImage probe#{} memory dstDc=0x{:08x} dstHwnd=0x{:08x} "
                             "dstBitmap=0x{:08x} dstSize={}x{} dstRect={},{} {}x{} "
                             "dstStats sampled={} nonBlack={} unique~{} first=0x{:08x} last=0x{:08x} "
                             "srcDc=0x{:08x} srcHwnd=0x{:08x} srcBitmap=0x{:08x} srcSize={}x{} "
                             "srcRect={},{} {}x{} srcStats sampled={} nonBlack={} unique~{} "
                             "first=0x{:08x} last=0x{:08x} color=0x{:08x}",
                             blitProbeLogCounter_, a0, dstDc->hwnd, dstSelectedBitmap,
                             dstBitmap->second.width, dstBitmap->second.heightRaw,
                             int32_t(a1), int32_t(a2), int32_t(a3), dstH,
                             dstStats.sampled, dstStats.nonBlack, dstStats.uniqueApprox,
                             dstStats.firstPixel, dstStats.lastPixel, stackArg(5), srcDc->hwnd,
                             srcSelectedBitmap, srcBitmap->second.width, srcBitmap->second.heightRaw,
                             srcX, srcY, srcW, srcH, srcStats.sampled, srcStats.nonBlack,
                             srcStats.uniqueApprox, srcStats.firstPixel, srcStats.lastPixel,
                             transparentColor);
            }
            if (!ok) {
                spdlog::info("TransparentImage bitmap blit failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dstBits=0x{:08x} dstSize={}x{} dstBpp={} src=0x{:08x} srcBitmap=0x{:08x} "
                             "srcBits=0x{:08x} srcSize={}x{} srcBpp={} dst={}x{} src={}x{} "
                             "srcOrigin={},{} color=0x{:08x}",
                             a0, dstSelectedBitmap, dstBitmap->second.bits, dstBitmap->second.width,
                             dstBitmap->second.heightRaw, dstBitmap->second.bpp, stackArg(5),
                             srcSelectedBitmap, srcBitmap->second.bits, srcBitmap->second.width,
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
                             int32_t(a3), dstH, stackArg(5), srcDc->hwnd, srcSelectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, transparentColor);
                if (blitProbeDumpCounter_ < 12 && std::abs(int32_t(a3)) >= 100 && std::abs(dstH) >= 40) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "transparent_present_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcSelectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpFramebufferPpm(tag);
                }
            }
            if (!ok) {
                spdlog::info("TransparentImage framebuffer blit failed dst=0x{:08x} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} color=0x{:08x}",
                             a0, stackArg(5), srcSelectedBitmap, srcBitmap->second.bits,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcBitmap->second.bpp,
                             int32_t(a3), dstH, srcW, srcH, srcX, srcY, transparentColor);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
        break;
    }
    case ord(CoredllOrdinal::ExtTextOutW):
    case ord(CoredllOrdinal::DrawTextW):
    {
        GuestDc* dc = lookupGuestDc(a0);
        if (!dc) {
            lastError_ = 6;
            ret = 0;
        } else {
            bool drawTextCall = false;
            switch (ordinal) {
            case ord(CoredllOrdinal::DrawTextW): drawTextCall = true; break;
            default: break;
            }
            const bool ok = drawTextCall
                ? drawHostTextToDc(*dc, 0, 0, 0, a3, a1, int32_t(a2), stackArg(4), true)
                : drawHostTextToDc(*dc, int32_t(a1), int32_t(a2), a3, stackArg(4),
                                   stackArg(5), int32_t(stackArg(6)), 0, false);
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
        break;
    }
    case ord(CoredllOrdinal::CreateCompatibleDC):
    {
        ret = makeGuestDc(0);
        if (GuestDc* dc = lookupGuestDc(ret)) {
            dc->selectedBitmap = makeStockObject(kStockDefaultBitmap);
            ceMgdi_.setSelectedBitmap(ret, dc->selectedBitmap);
        }
        lastError_ = ret ? 0 : 8;
        break;
    }
    case ord(CoredllOrdinal::DeleteDC):
    {
        auto handle = ceKernel_.handles().find(a0);
        if (handle == ceKernel_.handles().end() || handle->second.kind != GuestHandle::Kind::GuestDc) {
            lastError_ = 6;
            ret = 0;
        } else {
            ceMgdi_.destroyDc(a0);
            dcs_.erase(a0);
            ceKernel_.handles().erase(handle);
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::BitBlt):
    case ord(CoredllOrdinal::StretchBlt):
    {
        GuestDc* dstDc = lookupGuestDc(a0);
        GuestDc* srcDc = lookupGuestDc(stackArg(5));
        bool stretch = false;
        switch (ordinal) {
        case ord(CoredllOrdinal::StretchBlt): stretch = true; break;
        default: break;
        }
        const uint32_t rop = stretch ? stackArg(10) : stackArg(8);
        const int32_t dstW = int32_t(a3);
        const int32_t dstH = int32_t(stackArg(4));
        const int32_t srcX = int32_t(stackArg(6));
        const int32_t srcY = int32_t(stackArg(7));
        const int32_t srcW = stretch ? int32_t(stackArg(8)) : dstW;
        const int32_t srcH = stretch ? int32_t(stackArg(9)) : dstH;
        const CeMgdi::DcState* dstDcState = dstDc ? ceMgdi_.dcState(a0) : nullptr;
        const CeMgdi::DcState* srcDcState = srcDc ? ceMgdi_.dcState(stackArg(5)) : nullptr;
        const uint32_t dstSelectedBitmap = dstDcState ? dstDcState->selectedBitmap
                                                      : (dstDc ? dstDc->selectedBitmap : 0);
        const uint32_t srcSelectedBitmap = srcDcState ? srcDcState->selectedBitmap
                                                      : (srcDc ? srcDc->selectedBitmap : 0);
        auto srcBitmap = srcDc ? bitmaps_.find(srcSelectedBitmap) : bitmaps_.end();
        auto dstBitmap = dstDc ? bitmaps_.find(dstSelectedBitmap) : bitmaps_.end();
        if (!dstDc || !srcDc || srcBitmap == bitmaps_.end() || !supportedSourceRasterOp(rop)) {
            spdlog::info("{} unsupported dst=0x{:08x} dstBitmap=0x{:08x} src=0x{:08x} srcBitmap=0x{:08x} "
                         "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                         name, a0, dstSelectedBitmap, stackArg(5),
                         srcSelectedBitmap, dstW, dstH, srcW, srcH,
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
                             name, blitProbeLogCounter_, a0, dstDc->hwnd, dstSelectedBitmap,
                             dstBitmap->second.width, dstBitmap->second.heightRaw,
                             int32_t(a1), int32_t(a2), dstW, dstH, dstStats.sampled,
                             dstStats.nonBlack, dstStats.uniqueApprox, dstStats.firstPixel,
                             dstStats.lastPixel, stackArg(5), srcDc->hwnd, srcSelectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, rop);
                if (blitProbeDumpCounter_ < 12 &&
                    (std::abs(dstW) >= 100 || std::abs(dstH) >= 40 || dstBitmap->second.width >= 700)) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "memory_blit_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcSelectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpGuestBitmapPpm(dstSelectedBitmap, dstBitmap->second, std::string(tag) + "_result");
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
                splashCompositeBitmap_ = dstSelectedBitmap;
                dumpGuestBitmapPpm(srcSelectedBitmap, srcBitmap->second, splashTag + "_source");
                dumpGuestBitmapPpm(dstSelectedBitmap, dstBitmap->second, splashTag + "_result");
                spdlog::debug("{} splash probe dstDc=0x{:08x} dstBitmap=0x{:08x} dstSize={}x{} "
                             "srcDc=0x{:08x} srcBitmap=0x{:08x} srcSize={}x{} dst={}x{} "
                             "src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, dstSelectedBitmap, dstBitmap->second.width,
                             dstBitmap->second.heightRaw, stackArg(5), srcSelectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, dstW, dstH,
                             srcW, srcH, srcX, srcY, rop);
            }
            if (!ok) {
                spdlog::info("{} bitmap blit failed dst=0x{:08x} dstBitmap=0x{:08x} "
                             "dstBits=0x{:08x} dstSize={}x{} dstBpp={} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, dstSelectedBitmap, dstBitmap->second.bits,
                             dstBitmap->second.width, dstBitmap->second.heightRaw, dstBitmap->second.bpp,
                             stackArg(5), srcSelectedBitmap, srcBitmap->second.bits,
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
                             dstW, dstH, stackArg(5), srcDc->hwnd, srcSelectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcX, srcY,
                             srcW, srcH, srcStats.sampled, srcStats.nonBlack, srcStats.uniqueApprox,
                             srcStats.firstPixel, srcStats.lastPixel, rop);
                if (blitProbeDumpCounter_ < 12 && std::abs(dstW) >= 100 && std::abs(dstH) >= 40) {
                    ++blitProbeDumpCounter_;
                    char tag[80]{};
                    std::snprintf(tag, sizeof(tag), "framebuffer_blit_%02u", blitProbeDumpCounter_);
                    dumpGuestBitmapPpm(srcSelectedBitmap, srcBitmap->second, std::string(tag) + "_source");
                    dumpFramebufferPpm(tag);
                }
            }
            const bool splashFrame =
                ok && !splashFramebufferDumped_ && srcSelectedBitmap == splashCompositeBitmap_ &&
                srcBitmap->second.width == 800 && std::abs(srcBitmap->second.heightRaw) == 480 &&
                std::abs(dstW) == 800 && std::abs(dstH) == 480 &&
                std::abs(srcW) == 800 && std::abs(srcH) == 480;
            if (splashFrame) {
                splashFramebufferDumped_ = true;
                const std::string tag = name + "_splash_framebuffer";
                dumpGuestBitmapPpm(srcSelectedBitmap, srcBitmap->second, tag + "_source");
                dumpFramebufferPpm(tag);
                spdlog::debug("{} splash probe dstDc=0x{:08x} framebuffer srcDc=0x{:08x} "
                             "srcBitmap=0x{:08x} srcSize={}x{} dst={}x{} src={}x{} "
                             "srcOrigin={},{} rop=0x{:08x}",
                             name, a0, stackArg(5), srcSelectedBitmap,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, dstW, dstH,
                             srcW, srcH, srcX, srcY, rop);
            }
            if (!ok) {
                spdlog::info("{} framebuffer blit failed dst=0x{:08x} src=0x{:08x} "
                             "srcBitmap=0x{:08x} srcBits=0x{:08x} srcSize={}x{} srcBpp={} "
                             "dst={}x{} src={}x{} srcOrigin={},{} rop=0x{:08x}",
                             name, a0, stackArg(5), srcSelectedBitmap, srcBitmap->second.bits,
                             srcBitmap->second.width, srcBitmap->second.heightRaw, srcBitmap->second.bpp,
                             dstW, dstH, srcW, srcH, srcX, srcY, rop);
            }
            lastError_ = ok ? 0 : 120;
            ret = ok ? 1 : 0;
        }
        break;
    }
    case ord(CoredllOrdinal::GetWindowLongW):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = getWindowLongValue(it->second, int32_t(a1));
            if (int32_t(a1) == -4 || int32_t(a1) == -21 ||
                it->second.className.rfind("afx", 0) == 0) {
                spdlog::info("GetWindowLongW hwnd=0x{:08x} class=\"{}\" index={} -> 0x{:08x}",
                             a0, it->second.className, int32_t(a1), ret);
            }
        }
        break;
    }
    case ord(CoredllOrdinal::SetWindowLongW):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = setWindowLongValue(it->second, int32_t(a1), a2);
            if (int32_t(a1) == -4 || int32_t(a1) == -21 ||
                it->second.className.rfind("afx", 0) == 0) {
                spdlog::info("SetWindowLongW hwnd=0x{:08x} class=\"{}\" index={} value=0x{:08x} previous=0x{:08x}",
                             a0, it->second.className, int32_t(a1), a2, ret);
            }
        }
        break;
    }
    case ord(CoredllOrdinal::GetParent):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            lastError_ = 0;
            ret = it->second.parent;
        }
        break;
    }
    case ord(CoredllOrdinal::IsWindow):
    {
        auto it = windows_.find(a0);
        ret = it != windows_.end() && !it->second.destroyed ? 1 : 0;
        lastError_ = 0;
        break;
    }
    case ord(CoredllOrdinal::GetWindow):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else if (a1 == 5) {
            const auto children = orderedSiblingWindows(a0, true);
            ret = children.empty() ? 0 : children.front();
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 4) {
            const auto owner = windows_.find(it->second.parent);
            ret = !(it->second.style & kWindowStyleChild) &&
                  owner != windows_.end() && !owner->second.destroyed ? it->second.parent : 0;
            lastError_ = ret ? 0 : 1400;
        } else if (a1 == 2 || a1 == 3) {
            const bool childWindow = (it->second.style & kWindowStyleChild) != 0;
            std::vector<uint32_t> siblings = orderedSiblingWindows(it->second.parent, childWindow);
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
            const bool childWindow = (it->second.style & kWindowStyleChild) != 0;
            std::vector<uint32_t> siblings = orderedSiblingWindows(it->second.parent, childWindow);
            ret = siblings.empty() ? 0 : (a1 == 0 ? siblings.front() : siblings.back());
            lastError_ = ret ? 0 : 1400;
        } else {
            lastError_ = 1400;
            ret = 0;
        }
        break;
    }
    case ord(CoredllOrdinal::MoveWindow):
    {
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
            publishGuestWindowState(a0);
            if (sizeChanged) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0005; // WM_SIZE
                message.lParam = (uint32_t(uint16_t(it->second.height)) << 16) | uint16_t(it->second.width);
                message.time = uint32_t(++tick_ * 16);
                ceGwe_.postPostedMessage(message);
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::SetWindowPos):
    {
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
            if (!(flags & 0x0004u)) { // SWP_NOZORDER
                if (a1 == 1) { // HWND_BOTTOM
                    uint64_t bottom = it->second.zOrder;
                    const bool childWindow = (it->second.style & kWindowStyleChild) != 0;
                    for (uint32_t siblingHwnd : orderedSiblingWindows(it->second.parent, childWindow)) {
                        auto sibling = windows_.find(siblingHwnd);
                        if (sibling != windows_.end()) bottom = std::min(bottom, sibling->second.zOrder);
                    }
                    it->second.zOrder = bottom ? bottom - 1 : 0;
                } else {
                    it->second.zOrder = nextWindowZOrder();
                }
            } else if (!oldVisible && it->second.visible) {
                it->second.zOrder = nextWindowZOrder();
            }
            ensureHostWindow(a0, it->second);
            syncHostWindowPlacement(it->second, true);
            publishGuestWindowState(a0);
            if (sizeChanged) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0005; // WM_SIZE
                message.lParam = (uint32_t(uint16_t(it->second.height)) << 16) | uint16_t(it->second.width);
                message.time = uint32_t(++tick_ * 16);
                ceGwe_.postPostedMessage(message);
            }
            if (it->second.visible != oldVisible) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0018; // WM_SHOWWINDOW
                message.wParam = it->second.visible ? 1 : 0;
                message.time = uint32_t(++tick_ * 16);
                ceGwe_.postPostedMessage(message);
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
        break;
    }
    case ord(CoredllOrdinal::SetWindowRgn):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
#if defined(_WIN32)
            bool hostOk = true;
            if (it->second.hostHwnd) {
                HRGN region = nullptr;
                auto regionHandle = ceKernel_.handles().find(a1);
                if (a1) {
                    if (regionHandle == ceKernel_.handles().end() ||
                        regionHandle->second.kind != GuestHandle::Kind::HostRegion) {
                        lastError_ = 6;
                        ret = 0;
                        return true;
                    }
                    region = reinterpret_cast<HRGN>(regionHandle->second.hostValue);
                }
                hostOk = SetWindowRgn(reinterpret_cast<HWND>(it->second.hostHwnd), region, a2 != 0) != 0;
                if (hostOk && regionHandle != ceKernel_.handles().end()) {
                    regionHandle->second.hostValue = 0;
                    ceKernel_.handles().erase(regionHandle);
                }
            }
            ret = hostOk ? 1 : 0;
            lastError_ = ret ? 0 : GetLastError();
#else
            ret = 1;
            lastError_ = 0;
#endif
        }
        break;
    }
    case ord(CoredllOrdinal::GetWindowRgn):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end()) {
            lastError_ = 1400;
            ret = 0;
        } else {
#if defined(_WIN32)
            auto region = ceKernel_.handles().find(a1);
            if (it->second.hostHwnd && region != ceKernel_.handles().end() &&
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
        break;
    }
    case ord(CoredllOrdinal::DestroyWindow):
    {
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
            ceGwe_.unregisterWindow(a0);
            publishGuestWindowState(a0);
            GuestMessage destroy{};
            destroy.hwnd = a0;
            destroy.message = 0x0002; // WM_DESTROY
            destroy.time = uint32_t(++tick_ * 16);
            ceGwe_.postPostedMessage(destroy);
            GuestMessage ncDestroy{};
            ncDestroy.hwnd = a0;
            ncDestroy.message = 0x0082; // WM_NCDESTROY
            ncDestroy.time = uint32_t(++tick_ * 16);
            ceGwe_.postPostedMessage(ncDestroy);
            destroyHostWindow(it->second);
            if (wasVisible && parent) {
                spdlog::info("DestroyWindow invalidating parent=0x{:08x} after child=0x{:08x}", parent, a0);
                queueGuestPaint(parent, true);
            }
            lastError_ = 0;
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::ShowWindow):
    {
        auto it = windows_.find(a0);
        if (it == windows_.end() || it->second.destroyed) {
            lastError_ = 1400;
            ret = 0;
        } else {
            const bool wasVisible = it->second.visible;
            it->second.visible = a1 != 0;
            if (!wasVisible && it->second.visible) {
                it->second.zOrder = nextWindowZOrder();
            }
            size_t discardedInput = 0;
            if (!it->second.visible) {
                it->second.paintBoundsValid = false;
                auto isSameOrDescendant = [&](uint32_t hwnd) {
                    for (uint32_t current = hwnd; current;) {
                        if (current == a0) return true;
                        auto window = windows_.find(current);
                        if (window == windows_.end()) break;
                        current = window->second.parent;
                    }
                    return false;
                };
                discardedInput = ceGwe_.eraseIf([&](const GuestMessage& message) {
                    return message.message >= 0x0200 && message.message <= 0x0202 &&
                           isSameOrDescendant(message.hwnd);
                });
                if (isSameOrDescendant(capturedWindow_)) capturedWindow_ = 0;
                if (isSameOrDescendant(hostPointerCaptureWindow_)) hostPointerCaptureWindow_ = 0;
                if (isSameOrDescendant(pendingSyntheticChildButtonUpWindow_)) {
                    pendingSyntheticChildButtonUpWindow_ = 0;
                }
            }
            spdlog::info("ShowWindow guest=0x{:08x} cmd={} oldVisible={} newVisible={}",
                         a0, int32_t(a1), wasVisible ? 1 : 0, it->second.visible ? 1 : 0);
            if (discardedInput) {
                spdlog::info("ShowWindow discarded {} queued pointer messages for hidden guest=0x{:08x}",
                             discardedInput, a0);
            }
            ensureHostWindow(a0, it->second);
#if defined(_WIN32)
            if (it->second.hostHwnd) {
                HWND hwnd = reinterpret_cast<HWND>(it->second.hostHwnd);
                ShowWindow(hwnd, it->second.visible ? SW_SHOWNORMAL : SW_HIDE);
                presentHostWindows(true);
            }
#endif
            publishGuestWindowState(a0);
            if (it->second.visible != wasVisible) {
                GuestMessage message{};
                message.hwnd = a0;
                message.message = 0x0018; // WM_SHOWWINDOW
                message.wParam = it->second.visible ? 1 : 0;
                message.time = uint32_t(++tick_ * 16);
                ceGwe_.postPostedMessage(message);
                if (!it->second.visible && it->second.parent) {
                    const bool exposesCoveredWindows =
                        wasVisible && isOwnedPopupWindow(a0) && guestWindowCoversFramebuffer(a0);
                    eraseGuestWindowArea(a0, it->second);
                    spdlog::info("ShowWindow invalidating parent=0x{:08x} after hiding child=0x{:08x}",
                                 it->second.parent, a0);
                    queueGuestPaint(it->second.parent, true);
                    if (exposesCoveredWindows) {
                        size_t exposed = 0;
                        for (const auto& [otherHwnd, window] : windows_) {
                            if (otherHwnd == a0 || window.destroyed || !window.visible) continue;
                            queueGuestPaint(otherHwnd, true);
                            ++exposed;
                        }
                        spdlog::info("ShowWindow exposed full-screen popup hwnd=0x{:08x}; queued repaint for {} visible windows",
                                     a0, exposed);
                    }
                }
            }
            lastError_ = 0;
            ret = wasVisible ? 1 : 0;
        }
        break;
    }
    case ord(CoredllOrdinal::UpdateWindow):
    {
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
        break;
    }
    case ord(CoredllOrdinal::DefWindowProcW):
    {
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
                publishGuestWindowState(a0);
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
        break;
    }
    case ord(CoredllOrdinal::GetMessagePos):
    {
        ret = lastMessagePos_;
        break;
    }
    case ord(CoredllOrdinal::TranslateMessage):
    {
        ret = a0 ? 1 : 0;
        break;
    }
    case ord(CoredllOrdinal::PostQuitMessage):
    {
        quitPosted_ = true;
        std::string visibleRoots;
        for (const auto& [hwnd, window] : windows_) {
            if (window.destroyed || !window.visible || window.parent) {
                continue;
            }
            if (!visibleRoots.empty()) visibleRoots += ", ";
            visibleRoots += fmt::format("0x{:08x}:{}", hwnd, window.title);
        }
        spdlog::warn("PostQuitMessage exitCode=0x{:08x} ra=0x{:08x} activeThread=0x{:08x} queued={} visibleRoots=[{}]",
                     a0, args.ra, ceKernel_.activeGuestThread(), ceGwe_.messageCount(), visibleRoots);
        GuestMessage message{};
        message.message = 0x0012; // WM_QUIT
        message.wParam = a0;
        message.time = uint32_t(++tick_ * 16);
        ceGwe_.postPostedMessage(message);
        wakeGuestThreadsWaitingForMessage();
        ret = 0;
        break;
    }
    case ord(CoredllOrdinal::PostMessageW):
    {
        auto logPostMessagePointer = [&](const char* label, uint32_t ptr) {
            if (!ptr) return;
            uint32_t readablePtr = ptr;
            const uint32_t slotNormalizedPtr = ptr & (0x02000000u - 1u);
            bool slotNormalized = false;
            if (!isGuestRangeReadable(readablePtr, 4) &&
                slotNormalizedPtr != ptr &&
                isGuestRangeReadable(slotNormalizedPtr, 4)) {
                readablePtr = slotNormalizedPtr;
                slotNormalized = true;
            }
            if (!isGuestRangeReadable(readablePtr, 4)) {
                spdlog::info("PostMessageW ptr {}=0x{:08x} msg=0x{:08x} unreadable slotNormalized=0x{:08x}",
                             label, ptr, a1, slotNormalizedPtr);
                return;
            }
            std::array<uint8_t, 64> bytes{};
            size_t byteCount = 0;
            if (uc_mem_read(uc_, readablePtr, bytes.data(), bytes.size()) == UC_ERR_OK) {
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
            spdlog::info("PostMessageW ptr {}=0x{:08x} readable=0x{:08x} slotNormalized={} msg=0x{:08x} ascii=\"{}\" utf16=\"{}\" bytes={}",
                         label, ptr, readablePtr, slotNormalized, a1,
                         readAscii(readablePtr, 128), readUtf16(readablePtr, 128), hex);
        };
        const bool tracePostMessage =
            a0 == 0xffff || a1 == 0x0401 ||
            (a1 >= 0x0600 && a1 <= 0x07ff) ||
            (a1 >= 0x5700 && a1 <= 0x58ff);
        if (tracePostMessage) {
            spdlog::info("PostMessageW call hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x}",
                         a0, a1, a2, a3);
            logPostMessagePointer("wparam", a2);
            logPostMessagePointer("lparam", a3);
        }
        if (a0 == 0xffff) {
            bool posted = false;
            for (const auto& [hwnd, window] : windows_) {
                if (window.destroyed || window.parent) {
                    continue;
                }
                if (window.externalProcess) {
                    continue;
                }
                GuestMessage message{};
                message.hwnd = hwnd;
                message.message = a1;
                message.wParam = a2;
                message.lParam = a3;
                message.time = uint32_t(++tick_ * 16);
                ceGwe_.postPostedMessage(message);
                posted = true;
            }
            posted = postCrossProcessBroadcastMessage(a1, a2, a3) || posted;
            lastError_ = posted ? 0 : 1400;
            ret = posted ? 1 : 0;
            if (tracePostMessage) {
                spdlog::info("PostMessageW broadcast msg=0x{:08x} posted={} queued={}",
                             a1, posted, ceGwe_.messageCount());
            }
        } else if (a0 == 0) {
            GuestMessage message{};
            message.hwnd = 0;
            message.message = a1;
            message.wParam = a2;
            message.lParam = a3;
            message.time = uint32_t(++tick_ * 16);
            const uint32_t ownerThread = ceKernel_.activeGuestThread()
                ? ceKernel_.activeGuestThread()
                : ceKernel_.mainThreadPseudoHandle();
            ceGwe_.postThreadMessage(message, ownerThread);
            lastError_ = 0;
            ret = 1;
            if (tracePostMessage) {
                spdlog::info("PostMessageW thread msg=0x{:08x} queued={}", a1, ceGwe_.messageCount());
            }
        } else if (!windows_.count(a0)) {
            lastError_ = 1400;
            ret = 0;
            if (tracePostMessage) {
                spdlog::info("PostMessageW target missing hwnd=0x{:08x} msg=0x{:08x}", a0, a1);
            }
        } else {
            auto postedWindow = windows_.find(a0);
            if (postedWindow != windows_.end() && postedWindow->second.externalProcess) {
                const bool delivered = postCrossProcessGuestMessage(postedWindow->second.externalProcessId,
                                                                    postedWindow->second.externalHwnd,
                                                                    a1,
                                                                    a2,
                                                                    a3);
                lastError_ = delivered ? 0 : 1400;
                ret = delivered ? 1 : 0;
                if (tracePostMessage) {
                    spdlog::info("PostMessageW delivered external hwnd=0x{:08x} remotePid={} remoteHwnd=0x{:08x} class=\"{}\" title=\"{}\" msg=0x{:08x} ok={}",
                                 a0,
                                 postedWindow->second.externalProcessId,
                                 postedWindow->second.externalHwnd,
                                 postedWindow->second.className,
                                 postedWindow->second.title,
                                 a1,
                                 delivered);
                }
                return true;
            }
            GuestMessage message{};
            message.hwnd = a0;
            message.message = a1;
            message.wParam = a2;
            message.lParam = a3;
            message.time = uint32_t(++tick_ * 16);
            ceGwe_.postPostedMessage(message);
            lastError_ = 0;
            ret = 1;
            if (tracePostMessage) {
                const std::string className = postedWindow == windows_.end() ? std::string{} : postedWindow->second.className;
                const std::string title = postedWindow == windows_.end() ? std::string{} : postedWindow->second.title;
                spdlog::info("PostMessageW target hwnd=0x{:08x} class=\"{}\" title=\"{}\" msg=0x{:08x} queued={}",
                             a0, className, title, a1, ceGwe_.messageCount());
            }
        }
        if (ret) wakeGuestThreadsWaitingForMessage();
        break;
    }
    case ord(CoredllOrdinal::GetMessageW):
    case ord(CoredllOrdinal::GetMessageWNoWait):
    case ord(CoredllOrdinal::PeekMessageW):
    {
        bool peek = false;
        switch (ordinal) {
        case ord(CoredllOrdinal::PeekMessageW):
        case ord(CoredllOrdinal::GetMessageWNoWait):
            peek = true;
            break;
        default:
            break;
        }
        constexpr uint32_t kThreadMessageFilterHwnd = 0xffffffffu;
        constexpr uint32_t kRemoveMessageFlag = 0x0001;
        const uint32_t removeFlags = peek ? stackArg(4) : kRemoveMessageFlag;
        pollCrossProcessGuestMessages();
        enqueueDueTimers();
        GuestMessage message{};
        bool haveMessage = false;
        const uint32_t currentQueueOwner = ceKernel_.activeGuestThread()
            ? ceKernel_.activeGuestThread()
            : ceKernel_.mainThreadPseudoHandle();
        auto takeMessage = [&]() {
            auto matchesFilter = [&](const GuestMessage& candidate) {
                if (a1 == kThreadMessageFilterHwnd) {
                    if (candidate.hwnd != 0) return false;
                } else if (a1 != 0 && candidate.hwnd != a1 && !isWindowOrDescendant(candidate.hwnd, a1)) {
                    if (!candidate.crossProcess) return false;
                    const auto candidateWindow = windows_.find(candidate.hwnd);
                    const auto filterWindow = windows_.find(a1);
                    if (candidateWindow == windows_.end() || filterWindow == windows_.end()) {
                        return false;
                    }
                    if (candidateWindow->second.ownerThread != filterWindow->second.ownerThread) {
                        return false;
                    }
                }
                if (a2 || a3) {
                    if (candidate.message < a2 || candidate.message > a3) return false;
                }
                if (!candidate.synchronousSender && coveringFullScreenOwnedPopup(candidate.hwnd)) {
                    return false;
                }
                return true;
            };
            auto queued = ceGwe_.firstMatchingForOwner(currentQueueOwner,
                                                       matchesFilter,
                                                       !peek || (removeFlags & kRemoveMessageFlag));
            if (!queued) return false;
            message = *queued;
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
        if (!haveMessage && !peek && !quitPosted_ && hasHostWindows()) {
            pumpHostMessages();
#if defined(_WIN32)
            const DWORD waitMs = std::max<DWORD>(1, std::min<DWORD>(16, timerWaitMilliseconds()));
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
#endif
            pumpHostMessages();
            enqueueDueTimers();
            haveMessage = takeMessage();
        }
        if (!haveMessage) {
            ret = 0;
            if (!peek && !quitPosted_) {
                spdlog::debug("synthetic coredll.dll!GetMessageW blocking with empty guest queue");
                uc_emu_stop(uc_);
            }
        } else if (message.message == 0x0012) {
            spdlog::info("{} retrieved input hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x} peek={} remove={} queued={}",
                         name, message.hwnd, message.message, message.wParam, message.lParam,
                         peek ? 1 : 0, (!peek || (removeFlags & kRemoveMessageFlag)) ? 1 : 0, ceGwe_.messageCount());
            lastMessagePos_ = uint32_t(uint16_t(message.x) | (uint32_t(uint16_t(message.y)) << 16));
            lastMessageTime_ = message.time;
            writeGuestMessage(a0, message);
            if ((!peek || (removeFlags & kRemoveMessageFlag)) && a0) {
                if (message.synchronousSender) {
                    retrievedSyncSendersByMsgPtr_[a0] = message.synchronousSender;
                } else {
                    retrievedSyncSendersByMsgPtr_.erase(a0);
                }
            }
            ret = 0;
        } else {
            if (message.message == 0x0007 || message.message == 0x0008 ||
                (message.message >= 0x0200 && message.message <= 0x0202) ||
                message.message == 0x032f0 ||
                message.message == 0x057c9 || message.message == 0x057cc ||
                message.message == 0x057ed || message.message == 0x057f5) {
                spdlog::info("{} retrieved input hwnd=0x{:08x} msg=0x{:08x} wparam=0x{:08x} lparam=0x{:08x} peek={} remove={} queued={}",
                             name, message.hwnd, message.message, message.wParam, message.lParam,
                             peek ? 1 : 0, (!peek || (removeFlags & kRemoveMessageFlag)) ? 1 : 0, ceGwe_.messageCount());
            }
            lastMessagePos_ = uint32_t(uint16_t(message.x) | (uint32_t(uint16_t(message.y)) << 16));
            lastMessageTime_ = message.time;
            writeGuestMessage(a0, message);
            if ((!peek || (removeFlags & kRemoveMessageFlag)) && a0) {
                if (message.synchronousSender) {
                    retrievedSyncSendersByMsgPtr_[a0] = message.synchronousSender;
                } else {
                    retrievedSyncSendersByMsgPtr_.erase(a0);
                }
            }
            ret = 1;
        }
        break;
    }
    case ord(CoredllOrdinal::GetSystemInfo):
    {
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
        break;
    }
    case ord(CoredllOrdinal::GetStoreInformation):
    {
        if (a0) {
            writeU32(a0, 64u * 1024u * 1024u);
            writeU32(a0 + 4, 32u * 1024u * 1024u);
        }
        ret = a0 ? 1 : 0;
        break;
    }
    case ord(CoredllOrdinal::GlobalAddAtomW):
    {
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
        break;
    }
    case ord(CoredllOrdinal::GlobalFindAtomW):
    {
        const std::string atomName = lowerAscii(readUtf16(a0));
        auto it = atomsByName_.find(atomName);
        ret = it == atomsByName_.end() ? 0 : it->second;
        lastError_ = ret ? 0 : 2;
        break;
    }
    case ord(CoredllOrdinal::GlobalDeleteAtom):
    {
        auto it = atomNames_.find(uint16_t(a0));
        if (it != atomNames_.end()) {
            atomsByName_.erase(it->second);
            atomNames_.erase(it);
        }
        ret = 0;
        lastError_ = 0;
        break;
    }
    case ord(CoredllOrdinal::WNetGetUserW):
    {
        ret = handleWNetGetUserW(a0, a1, a2);
        break;
    }
    case ord(CoredllOrdinal::WNetGetUniversalNameW):
    case ord(CoredllOrdinal::WNetConnectionDialog1W):
    {
        lastError_ = 1200;
        ret = 1200;
        break;
    }
    default:
        return false;
    }

    return true;
}
