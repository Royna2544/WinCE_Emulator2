#include "synthetic_dll.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

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
        const uint16_t ch = readLe16(bytes, offset + i * 2);
        result.push_back(ch < 0x80 ? char(ch) : '?');
    }
    return result;
}

std::string lowerAscii(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string pathToUtf8(const std::filesystem::path& path) {
    auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

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


void SyntheticDllRuntime::registerCoredllResExports(SyntheticModule& module) {
    struct CoreDllRes {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.res",
                {
                    {0x005E, {"LoadAcceleratorsW", Code::CoreDllLoadAcceleratorsW, &SyntheticDllRuntime::handleLoadAcceleratorsW}},
                    {0x0213, {"FindResource", Code::CoreDllFindResource, &SyntheticDllRuntime::handleFindResource}},
                    {0x0214, {"FindResourceW", Code::CoreDllFindResourceW, &SyntheticDllRuntime::handleFindResource}},
                    {0x0215, {"LoadResource", Code::CoreDllLoadResource, &SyntheticDllRuntime::handleLoadResource}},
                    {0x0216, {"SizeofResource", Code::CoreDllSizeofResource, &SyntheticDllRuntime::handleSizeofResource}},
                    {0x02AB, {"LoadCursorW", Code::CoreDllLoadCursorW, &SyntheticDllRuntime::handleLoadCursorOrdinalW}},
                    {0x02D8, {"LoadIconW", Code::CoreDllLoadIconW, &SyntheticDllRuntime::handleLoadIconW}},
                    {0x02DA, {"LoadImageW", Code::CoreDllLoadImageW, &SyntheticDllRuntime::handleLoadImageW}},
                    {0x034B, {"RemoveMenu", Code::CoreDllRemoveMenu, &SyntheticDllRuntime::handleRemoveMenu}},
                    {0x034E, {"LoadMenuW", Code::CoreDllLoadMenuW, &SyntheticDllRuntime::handleLoadMenuW}},
                    {0x0350, {"CheckMenuItem", Code::CoreDllCheckMenuItem, &SyntheticDllRuntime::handleCheckMenuItem}},
                    {0x0351, {"CheckMenuRadioItem", Code::CoreDllCheckMenuRadioItem, &SyntheticDllRuntime::handleCheckMenuRadioItem}},
                    {0x036A, {"LoadStringW", Code::CoreDllLoadStringW, &SyntheticDllRuntime::handleLoadStringW}},
                },
            };
        }
    };

    const CoreDllRes res;
    registerHandlers(module, res.group());
}

bool SyntheticDllRuntime::handleFindResource(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    const char* apiName = code == SyntheticExportCode::CoreDllFindResourceW ? "FindResourceW" : "FindResource";
    const ResourceEntry* resource = findResource(args.a2, args.a1);
    if (!resource) {
        spdlog::info("{} miss type=0x{:08x} name=0x{:08x}", apiName, args.a2, args.a1);
        lastError_ = 1814;
        ret = 0;
    } else {
        const size_t index = size_t(resource - mainResources_.data());
        ret = makeGuestHandle({GuestHandle::Kind::GuestResource, index + 1, 0});
        spdlog::info("{} hit type=0x{:08x} name=0x{:08x} size={}",
                     apiName, args.a2, args.a1, resource->data.size());
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleSizeofResource(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const ResourceEntry* resource = resourceFromHandle(args.a1);
    if (!resource) {
        lastError_ = 1814;
        ret = 0;
    } else {
        lastError_ = 0;
        ret = uint32_t(resource->data.size());
    }
    return true;
}

bool SyntheticDllRuntime::handleLoadResource(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const ResourceEntry* resource = resourceFromHandle(args.a1);
    if (!resource) {
        lastError_ = 1814;
        ret = 0;
    } else {
        auto loaded = loadedResourceMemory_.find(args.a1);
        if (loaded != loadedResourceMemory_.end()) {
            ret = loaded->second;
        } else {
            ret = allocate(uint32_t(resource->data.empty() ? 1 : resource->data.size()), false);
            if (ret && !resource->data.empty()) {
                uc_mem_write(uc_, ret, resource->data.data(), resource->data.size());
            }
            loadedResourceMemory_[args.a1] = ret;
        }
        lastError_ = ret ? 0 : 8;
    }
    return true;
}

bool SyntheticDllRuntime::handleLoadStringW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const uint32_t blockId = (args.a1 >> 4) + 1;
    const uint32_t stringIndex = args.a1 & 0x0f;
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
        spdlog::info("LoadStringW miss id={}", args.a1);
        lastError_ = 1814;
        ret = 0;
    } else {
        ret = args.a2 && args.a3 ? writeUtf16(args.a2, value, args.a3) : uint32_t(value.size());
        spdlog::info("LoadStringW hit id={} ret={} value=\"{}\"", args.a1, ret, value);
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleLoadAcceleratorsW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    const ResourceEntry* resource = findResource(9, args.a1);
    if (!resource || resource->data.empty()) {
        lastError_ = 1814;
        ret = 0;
        return true;
    }

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
    return true;
}

bool SyntheticDllRuntime::handleLoadIconW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = handleLoadImageApi("LoadIconW", args.a0, args.a1, args.a2, args.a3, stackArg(4), stackArg(5));
    return true;
}

bool SyntheticDllRuntime::handleLoadImageW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = handleLoadImageApi("LoadImageW", args.a0, args.a1, args.a2, args.a3, stackArg(4), stackArg(5));
    return true;
}

bool SyntheticDllRuntime::handleLoadMenuW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = loadMenuResourceHandle(args.a1);
    return true;
}

bool SyntheticDllRuntime::handleLoadCursorOrdinalW(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    ret = handleLoadCursorW(args.a0, args.a1);
    return true;
}

bool SyntheticDllRuntime::handleRemoveMenu(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostMenu || !handle->hostValue) {
        lastError_ = 1401;
        ret = 0;
    } else {
        ret = RemoveMenu(reinterpret_cast<HMENU>(handle->hostValue), args.a1, args.a2) ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
    }
    return true;
}

bool SyntheticDllRuntime::handleCheckMenuItem(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostMenu || !handle->hostValue) {
        lastError_ = 1401;
        ret = 0xffffffffu;
    } else {
        ret = CheckMenuItem(reinterpret_cast<HMENU>(handle->hostValue), args.a1, args.a2);
        if (ret == 0xffffffffu) lastError_ = GetLastError();
    }
    return true;
}

bool SyntheticDllRuntime::handleCheckMenuRadioItem(SyntheticExportCode, const GuestCallArgs& args, uint32_t& ret) {
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostMenu || !handle->hostValue) {
        lastError_ = 1401;
        ret = 0;
    } else {
        ret = CheckMenuRadioItem(reinterpret_cast<HMENU>(handle->hostValue),
                                 args.a1, args.a2, args.a3, stackArg(4)) ? 1 : 0;
        if (!ret) lastError_ = GetLastError();
    }
    return true;
}

uint32_t SyntheticDllRuntime::handleLoadCursorW(uint32_t, uint32_t cursorName) {
    HCURSOR cursor = nullptr;
    if (cursorName && cursorName < 0x10000) cursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(cursorName));
    else cursor = ::LoadCursorW(nullptr, IDC_ARROW);
    const uint32_t result = cursor
        ? makeGuestHandle({GuestHandle::Kind::HostCursor, reinterpret_cast<uintptr_t>(cursor), 0})
        : 0;
    lastError_ = result ? 0 : GetLastError();
    return result;
}

uint32_t SyntheticDllRuntime::handleLoadImageApi(const std::string& name, uint32_t, uint32_t imageName,
                                                 uint32_t imageType, uint32_t desiredCx, uint32_t desiredCy,
                                                 uint32_t loadFlags) {
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
}

uint32_t SyntheticDllRuntime::loadMenuResourceHandle(uint32_t nameArg) {
    const ResourceEntry* resource = findResource(4, nameArg);
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
}
