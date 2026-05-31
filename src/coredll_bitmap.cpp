#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#endif

#include "synthetic_dll.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#if defined(__AVX2__) || defined(__AVX512F__)
#include <immintrin.h>
#endif

#include <spdlog/spdlog.h>

namespace {
constexpr uint32_t kCosmeticPenWidth = 1;
constexpr uint32_t kMaxRasterPenWidth = 256;

uint32_t effectivePenWidth(uint32_t width) {
    return std::clamp(width ? width : kCosmeticPenWidth, kCosmeticPenWidth, kMaxRasterPenWidth);
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

uint8_t expand6To8(uint16_t value) {
    value &= 0x3fu;
    return uint8_t((value << 2) | (value >> 4));
}

uint32_t decodeRgb555(uint16_t value) {
    const uint8_t r = expand5To8(value >> 10);
    const uint8_t g = expand5To8(value >> 5);
    const uint8_t b = expand5To8(value);
    return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint16_t encodeRgb555(uint32_t pixel) {
    const uint16_t r = uint16_t((pixel >> 19) & 0x1fu);
    const uint16_t g = uint16_t((pixel >> 11) & 0x1fu);
    const uint16_t b = uint16_t((pixel >> 3) & 0x1fu);
    return uint16_t((r << 10) | (g << 5) | b);
}

uint32_t decodeRgb565(uint16_t value) {
    const uint8_t r = expand5To8(value >> 11);
    const uint8_t g = expand6To8(value >> 5);
    const uint8_t b = expand5To8(value);
    return 0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
}

uint16_t encodeRgb565(uint32_t pixel) {
    const uint16_t r = uint16_t((pixel >> 19) & 0x1fu);
    const uint16_t g = uint16_t((pixel >> 10) & 0x3fu);
    const uint16_t b = uint16_t((pixel >> 3) & 0x1fu);
    return uint16_t((r << 11) | (g << 5) | b);
}

constexpr uint32_t kRgb565RedMask = 0x0000f800u;
constexpr uint32_t kRgb565GreenMask = 0x000007e0u;
constexpr uint32_t kRgb565BlueMask = 0x0000001fu;
constexpr uint32_t kRgb555RedMask = 0x00007c00u;
constexpr uint32_t kRgb555GreenMask = 0x000003e0u;
constexpr uint32_t kRgb555BlueMask = 0x0000001fu;
constexpr int32_t kStockWhiteBrush = 0;
constexpr int32_t kStockLtGrayBrush = 1;
constexpr int32_t kStockGrayBrush = 2;
constexpr int32_t kStockDkGrayBrush = 3;
constexpr int32_t kStockBlackBrush = 4;
constexpr int32_t kStockNullBrush = 5;
constexpr int32_t kStockWhitePen = 6;
constexpr int32_t kStockBlackPen = 7;
constexpr int32_t kStockNullPen = 8;
constexpr int32_t kStockSystemFont = 13;
constexpr int32_t kStockDefaultGuiFont = 17;
constexpr int32_t kStockDefaultBitmap = 21;
constexpr uint32_t kBitmapObjectBytes = 24;

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
    redMask = kRgb565RedMask;
    greenMask = kRgb565GreenMask;
    blueMask = kRgb565BlueMask;
}

uint32_t decodeBitmap16(uint16_t value, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    if ((!redMask && !greenMask && !blueMask) ||
        (redMask == kRgb565RedMask && greenMask == kRgb565GreenMask && blueMask == kRgb565BlueMask)) {
        return decodeRgb565(value);
    }
    if (redMask == kRgb555RedMask && greenMask == kRgb555GreenMask && blueMask == kRgb555BlueMask) {
        return decodeRgb555(value);
    }
    return decodeMasked16(value, redMask, greenMask, blueMask);
}

uint16_t encodeBitmap16(uint32_t pixel, uint32_t redMask, uint32_t greenMask, uint32_t blueMask) {
    if ((!redMask && !greenMask && !blueMask) ||
        (redMask == kRgb565RedMask && greenMask == kRgb565GreenMask && blueMask == kRgb565BlueMask)) {
        return encodeRgb565(pixel);
    }
    if (redMask == kRgb555RedMask && greenMask == kRgb555GreenMask && blueMask == kRgb555BlueMask) {
        return encodeRgb555(pixel);
    }
    return encodeMasked16(pixel, redMask, greenMask, blueMask);
}

uint32_t readBgra32Pixel(const uint8_t* row, int32_t x) {
    uint32_t value = 0;
    std::memcpy(&value, row + size_t(x) * 4, sizeof(value));
    return 0xff000000u | (value & 0x00ffffffu);
}

void writeBgra32Pixel(uint8_t* row, int32_t x, uint32_t pixel) {
    const uint32_t value = 0xff000000u | (pixel & 0x00ffffffu);
    std::memcpy(row + size_t(x) * 4, &value, sizeof(value));
}

uint32_t palettePixel(const std::vector<uint32_t>& palette, uint32_t index) {
    if (palette.empty()) return 0xff000000u;
    if (index >= palette.size()) index = uint32_t(palette.size() - 1);
    return palette[index];
}

#if defined(__AVX2__) || defined(__AVX512F__)
void convertRgb565ToBgra32Avx2(const uint8_t* src, uint8_t* dst, int32_t pixels) {
    const __m256i mask5 = _mm256_set1_epi32(0x1f);
    const __m256i mask6 = _mm256_set1_epi32(0x3f);
    const __m256i alpha = _mm256_set1_epi32(-16777216);
    int32_t x = 0;
    for (; x + 8 <= pixels; x += 8) {
        const __m128i packed = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + size_t(x) * 2));
        const __m256i value = _mm256_cvtepu16_epi32(packed);
        const __m256i r5 = _mm256_srli_epi32(value, 11);
        const __m256i g6 = _mm256_and_si256(_mm256_srli_epi32(value, 5), mask6);
        const __m256i b5 = _mm256_and_si256(value, mask5);
        const __m256i r8 = _mm256_or_si256(_mm256_slli_epi32(r5, 3), _mm256_srli_epi32(r5, 2));
        const __m256i g8 = _mm256_or_si256(_mm256_slli_epi32(g6, 2), _mm256_srli_epi32(g6, 4));
        const __m256i b8 = _mm256_or_si256(_mm256_slli_epi32(b5, 3), _mm256_srli_epi32(b5, 2));
        const __m256i out = _mm256_or_si256(
            alpha,
            _mm256_or_si256(_mm256_slli_epi32(r8, 16), _mm256_or_si256(_mm256_slli_epi32(g8, 8), b8)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + size_t(x) * 4), out);
    }
    for (; x < pixels; ++x) {
        const uint16_t value = uint16_t(src[size_t(x) * 2] | (uint16_t(src[size_t(x) * 2 + 1]) << 8));
        const uint32_t pixel = decodeRgb565(value);
        dst[size_t(x) * 4 + 0] = uint8_t(pixel & 0xffu);
        dst[size_t(x) * 4 + 1] = uint8_t((pixel >> 8) & 0xffu);
        dst[size_t(x) * 4 + 2] = uint8_t((pixel >> 16) & 0xffu);
        dst[size_t(x) * 4 + 3] = 0xff;
    }
}
#endif

}

uint32_t SyntheticDllRuntime::makeGuestDc(uint32_t hwnd) {
    GuestDc dc{};
    dc.hwnd = hwnd;
    dc.selectedBrush = makeStockObject(kStockBlackBrush);
    dc.selectedPen = makeStockObject(kStockBlackPen);
    dc.selectedFont = makeStockObject(kStockDefaultGuiFont);
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestDc, 0, 0});
    dc.hdc = handle;
    dcs_[handle] = dc;
    ceMgdi_.createDc(handle, hwnd);
    ceMgdi_.updateSelectedObjects(handle, dc.selectedBrush, dc.selectedPen, dc.selectedFont, dc.selectedBitmap);
    if (const auto visibleRect = ceGwe_.visibleRectForWindow(hwnd)) {
        ceMgdi_.setSystemClip(handle,
                              CeMgdi::Rect{visibleRect->left,
                                           visibleRect->top,
                                           visibleRect->right,
                                           visibleRect->bottom});
    }
    return handle;
}

SyntheticDllRuntime::GuestDc* SyntheticDllRuntime::lookupGuestDc(uint32_t hdc) {
    auto handle = ceKernel_.handles().find(hdc);
    if (handle == ceKernel_.handles().end() || handle->second.kind != GuestHandle::Kind::GuestDc) return nullptr;
    auto dc = dcs_.find(hdc);
    return dc == dcs_.end() ? nullptr : &dc->second;
}

void SyntheticDllRuntime::mirrorMgdiBitmap(uint32_t handle, const GuestBitmap& bitmap) {
    CeMgdi::BitmapState state{};
    state.hbitmap = handle;
    state.width = bitmap.width;
    state.heightRaw = bitmap.heightRaw;
    state.bpp = bitmap.bpp;
    state.stride = bitmap.stride;
    state.bits = bitmap.bits;
    state.redMask = bitmap.redMask;
    state.greenMask = bitmap.greenMask;
    state.blueMask = bitmap.blueMask;
    state.paletteEntries = bitmap.palette.size();
    state.palette = bitmap.palette;
    state.stock = bitmap.stock;
    ceMgdi_.trackBitmap(state);
}

uint32_t SyntheticDllRuntime::makeGuestBrush(uint32_t colorRef, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestBrush, 0, stock ? 1u : 0u});
    brushes_[handle] = GuestBrush{colorRef, 0, stock};
    ceMgdi_.trackBrush(CeMgdi::BrushState{handle, colorRef, 0, stock});
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
    ceMgdi_.setBrushPatternBitmap(brush, bitmapHandle);
    return brush;
}

uint32_t SyntheticDllRuntime::makeGuestPen(uint32_t style, uint32_t width, uint32_t colorRef, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestPen, 0, stock ? 1u : 0u});
    pens_[handle] = GuestPen{style, width, colorRef, stock};
    ceMgdi_.trackPen(CeMgdi::PenState{handle, style, width, colorRef, stock});
    return handle;
}

uint32_t SyntheticDllRuntime::makeGuestFont(const std::array<uint8_t, 92>& logFont, bool stock) {
    const uint32_t handle = makeGuestHandle({GuestHandle::Kind::GuestFont, 0, stock ? 1u : 0u});
    fonts_[handle] = GuestFont{logFont, stock};
    ceMgdi_.trackFont(CeMgdi::FontState{handle, logFont, stock});
    return handle;
}

uint32_t SyntheticDllRuntime::makeStockObject(int32_t index) {
    auto existing = stockObjects_.find(index);
    if (existing != stockObjects_.end()) return existing->second;

    uint32_t handle = 0;
    switch (index) {
    case kStockWhiteBrush: handle = makeGuestBrush(0x00ffffff, true); break;
    case kStockLtGrayBrush: handle = makeGuestBrush(0x00c0c0c0, true); break;
    case kStockGrayBrush: handle = makeGuestBrush(0x00808080, true); break;
    case kStockDkGrayBrush: handle = makeGuestBrush(0x00404040, true); break;
    case kStockBlackBrush: handle = makeGuestBrush(0x00000000, true); break;
    case kStockNullBrush: handle = makeGuestBrush(0xffffffffu, true); break;
    case kStockWhitePen: handle = makeGuestPen(0, 1, 0x00ffffff, true); break;
    case kStockBlackPen: handle = makeGuestPen(0, 1, 0x00000000, true); break;
    case kStockNullPen: handle = makeGuestPen(5, 1, 0xffffffffu, true); break;
    case kStockSystemFont:
    case kStockDefaultGuiFont: {
        std::array<uint8_t, 92> font{};
        handle = makeGuestFont(font, true);
        break;
    }
    case kStockDefaultBitmap: {
        handle = makeGuestHandle({GuestHandle::Kind::HostBitmap, 0, 0});
        GuestBitmap bitmap{};
        bitmap.width = 1;
        bitmap.heightRaw = -1;
        bitmap.bpp = 1;
        bitmap.stride = 4;
        bitmap.palette = defaultIndexedPalette(1);
        bitmap.stock = true;
        bitmaps_[handle] = std::move(bitmap);
        mirrorMgdiBitmap(handle, bitmaps_[handle]);
        break;
    }
    default:
        return 0;
    }
    stockObjects_[index] = handle;
    return handle;
}

uint32_t SyntheticDllRuntime::readFramebufferTargetPixel(uint32_t targetHwnd,
                                                         int32_t x,
                                                         int32_t y) const {
    if (!framebuffer_ || x < 0 || y < 0 || x >= framebufferWidth_ || y >= framebufferHeight_) {
        return 0xff000000u;
    }
    const uint32_t coveringPopup = targetHwnd ? coveringFullScreenOwnedPopup(targetHwnd) : 0;
    const uint64_t targetZ = targetHwnd ? windowZOrder(targetHwnd) : 0;
    for (const auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
            window.backingWidth <= 0 || window.backingHeight <= 0 ||
            isWindowOrDescendant(targetHwnd, hwnd)) {
            continue;
        }
        if (targetHwnd && window.zOrder < targetZ && hwnd != coveringPopup) continue;
        if (x < window.backingX || y < window.backingY ||
            x >= window.backingX + window.backingWidth ||
            y >= window.backingY + window.backingHeight) {
            continue;
        }
        const size_t offset = size_t(y - window.backingY) * size_t(window.backingWidth) +
                              size_t(x - window.backingX);
        if (offset < window.backingPixels.size()) return window.backingPixels[offset];
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
    const uint32_t coveringPopup = targetHwnd ? coveringFullScreenOwnedPopup(targetHwnd) : 0;
    bool covered = false;
    const uint64_t targetZ = targetHwnd ? windowZOrder(targetHwnd) : 0;
    for (auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
            window.backingWidth <= 0 || window.backingHeight <= 0 ||
            isWindowOrDescendant(targetHwnd, hwnd)) {
            continue;
        }
        if (targetHwnd && window.zOrder < targetZ && hwnd != coveringPopup) continue;
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
    if (!covered && !coveringPopup) framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)] = pixel;
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

std::optional<CeMgdi::Rect> SyntheticDllRuntime::framebufferClipForDc(const GuestDc& dc) const {
    auto clip = ceMgdi_.effectiveClipForDc(dc.hdc);
    if (!clip) return std::nullopt;
    clip->left = std::clamp<int32_t>(clip->left, 0, framebufferWidth_);
    clip->right = std::clamp<int32_t>(clip->right, 0, framebufferWidth_);
    clip->top = std::clamp<int32_t>(clip->top, 0, framebufferHeight_);
    clip->bottom = std::clamp<int32_t>(clip->bottom, 0, framebufferHeight_);
    return clip;
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
    if (const auto clip = framebufferClipForDc(dc)) {
        left = std::max(left, clip->left);
        right = std::min(right, clip->right);
        top = std::max(top, clip->top);
        bottom = std::min(bottom, clip->bottom);
    }
    if (left >= right || top >= bottom) return;
    noteGuestWindowPaint(dc.hwnd, left, top, right, bottom);

    const uint32_t coveringPopup = dc.hwnd ? coveringFullScreenOwnedPopup(dc.hwnd) : 0;

    bool needsLayeredWrite = coveringPopup != 0;
    const uint64_t targetZ = dc.hwnd ? windowZOrder(dc.hwnd) : 0;
    for (const auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
            window.backingWidth <= 0 || window.backingHeight <= 0 ||
            isWindowOrDescendant(dc.hwnd, hwnd)) {
            continue;
        }
        if (dc.hwnd && window.zOrder < targetZ && hwnd != coveringPopup) continue;
        const int32_t backingRight = window.backingX + window.backingWidth;
        const int32_t backingBottom = window.backingY + window.backingHeight;
        if (left < backingRight && right > window.backingX &&
            top < backingBottom && bottom > window.backingY) {
            needsLayeredWrite = true;
            break;
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
                                              uint32_t pixel,
                                              uint32_t width) {
    if (!framebuffer_ || pixel == 0 || framebufferWidth_ <= 0 || framebufferHeight_ <= 0) return;
    captureGuestWindowBacking(dc.hwnd);
    width = effectivePenWidth(width);
    const int32_t strokeBefore = int32_t((width - 1) / 2);
    const int32_t strokeAfter = int32_t(width / 2);
    int32_t originX = 0;
    int32_t originY = 0;
    if (dc.hwnd) std::tie(originX, originY) = guestWindowOrigin(dc.hwnd);
    x0 += originX;
    x1 += originX;
    y0 += originY;
    y1 += originY;
    const auto clip = framebufferClipForDc(dc);
    noteGuestWindowPaint(dc.hwnd,
                         std::clamp<int32_t>(std::min(x0, x1) - strokeBefore, 0, framebufferWidth_),
                         std::clamp<int32_t>(std::min(y0, y1) - strokeBefore, 0, framebufferHeight_),
                         std::clamp<int32_t>(std::max(x0, x1) + strokeAfter + 1, 0, framebufferWidth_),
                         std::clamp<int32_t>(std::max(y0, y1) + strokeAfter + 1, 0, framebufferHeight_));
    const int32_t dx = std::abs(x1 - x0);
    const int32_t sx = x0 < x1 ? 1 : -1;
    const int32_t dy = -std::abs(y1 - y0);
    const int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    for (;;) {
        for (int32_t py = y0 - strokeBefore; py <= y0 + strokeAfter; ++py) {
            if (py < 0 || py >= framebufferHeight_) continue;
            for (int32_t px = x0 - strokeBefore; px <= x0 + strokeAfter; ++px) {
                if (px >= 0 && px < framebufferWidth_ &&
                    (!clip || CeMgdi::rectContainsPoint(*clip, px, py))) {
                    writeFramebufferTargetPixel(dc.hwnd, px, py, pixel);
                }
            }
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
        writeBitmapSpan(bitmap, raw, height, y, left, right, pixel);
    }
    return uc_mem_write(uc_, bitmap.bits, raw.data(), raw.size()) == UC_ERR_OK;
}

bool SyntheticDllRuntime::drawBitmapLine(const GuestBitmap& bitmap,
                                         int32_t x0,
                                         int32_t y0,
                                         int32_t x1,
                                         int32_t y1,
                                         uint32_t pixel,
                                         uint32_t width) {
    const int32_t height = std::abs(bitmap.heightRaw);
    if (!bitmap.bits || bitmap.width <= 0 || height <= 0 || bitmap.stride == 0 || pixel == 0) return false;
    width = effectivePenWidth(width);
    const int32_t strokeBefore = int32_t((width - 1) / 2);
    const int32_t strokeAfter = int32_t(width / 2);
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
        for (int32_t py = y0 - strokeBefore; py <= y0 + strokeAfter; ++py) {
            if (py < 0 || py >= height) continue;
            for (int32_t px = x0 - strokeBefore; px <= x0 + strokeAfter; ++px) {
                if (px >= 0 && px < bitmap.width) {
                    writeBitmapPixel(bitmap, raw, height, px, py, pixel);
                }
            }
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
    const uint32_t selectedBitmap = ceMgdi_.selectedBitmapForDc(dc.hdc, dc.selectedBitmap);
    auto bitmap = bitmaps_.find(selectedBitmap);
    if (bitmap != bitmaps_.end()) {
        syncBitmapPaletteFromMgdi(selectedBitmap, bitmap->second);
        return fillBitmapRect(bitmap->second, left, top, right, bottom, pixel);
    }
    fillFramebufferRect(dc, left, top, right, bottom, pixel);
    return true;
}

bool SyntheticDllRuntime::drawDcLine(const GuestDc& dc,
                                     int32_t x0,
                                     int32_t y0,
                                     int32_t x1,
                                     int32_t y1,
                                     uint32_t pixel,
                                     uint32_t width) {
    const uint32_t selectedBitmap = ceMgdi_.selectedBitmapForDc(dc.hdc, dc.selectedBitmap);
    auto bitmap = bitmaps_.find(selectedBitmap);
    if (bitmap != bitmaps_.end()) {
        syncBitmapPaletteFromMgdi(selectedBitmap, bitmap->second);
        return drawBitmapLine(bitmap->second, x0, y0, x1, y1, pixel, width);
    }
    drawFramebufferLine(dc, x0, y0, x1, y1, pixel, width);
    return true;
}

bool SyntheticDllRuntime::fillDcPolygon(const GuestDc& dc,
                                        const std::vector<std::pair<int32_t, int32_t>>& points,
                                        uint32_t pixel) {
    if (points.size() < 3 || pixel == 0) return false;

    std::vector<std::pair<int32_t, int32_t>> translatedPoints = points;
    const uint32_t selectedBitmap = ceMgdi_.selectedBitmapForDc(dc.hdc, dc.selectedBitmap);
    auto bitmapIt = bitmaps_.find(selectedBitmap);
    if (bitmapIt != bitmaps_.end()) {
#if defined(_WIN32)
        syncBitmapPaletteFromMgdi(selectedBitmap, bitmapIt->second);
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
            uint8_t* dibRow = pixels + size_t(y) * size_t(dibWidth) * 4;
            for (int32_t x = 0; x < dibWidth; ++x) {
                uint32_t current = 0;
                readBitmapPixel(bitmap, raw, bitmapHeight, clipLeft + x, clipTop + y, current);
                writeBgra32Pixel(dibRow, x, current);
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
            const uint32_t fillPixel = pixel & 0x00ffffffu;
            for (int32_t y = 0; y < dibHeight; ++y) {
                const uint8_t* row = pixels + size_t(y) * size_t(dibWidth) * 4;
                int32_t runStart = -1;
                for (int32_t x = 0; x < dibWidth; ++x) {
                    const bool filled = (readBgra32Pixel(row, x) & 0x00ffffffu) == fillPixel;
                    if (filled && runStart < 0) {
                        runStart = x;
                    } else if (!filled && runStart >= 0) {
                        writeBitmapSpan(bitmap, raw, bitmapHeight, clipTop + y,
                                        clipLeft + runStart, clipLeft + x, pixel);
                        runStart = -1;
                    }
                }
                if (runStart >= 0) {
                    writeBitmapSpan(bitmap, raw, bitmapHeight, clipTop + y,
                                    clipLeft + runStart, clipLeft + dibWidth, pixel);
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
    std::optional<CeMgdi::Rect> framebufferClip;
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
        framebufferClip = framebufferClipForDc(dc);
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
                writeBitmapSpan(bitmap, raw, bitmapHeight, y, left, right + 1, pixel);
            } else {
                left = std::clamp<int32_t>(left, 0, framebufferWidth_ - 1);
                right = std::clamp<int32_t>(right, 0, framebufferWidth_ - 1);
                for (int32_t x = left; x <= right; ++x) {
                    if (!framebufferClip || CeMgdi::rectContainsPoint(*framebufferClip, x, y)) {
                        writeFramebufferTargetPixel(dc.hwnd, x, y, pixel);
                    }
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
        const uint32_t selectedFontHandle = ceMgdi_.selectedFontForDc(dc.hdc, dc.selectedFont);
        const CeMgdi::FontState* font = ceMgdi_.fontState(selectedFontHandle);
        if (!font || font->stock) {
            HFONT stockFont = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            if (!stockFont || GetObjectW(stockFont, sizeof(logFont), &logFont) != sizeof(logFont)) {
                return stockFont;
            }
        } else {
            const size_t bytes = std::min(sizeof(logFont), font->logFont.size());
            std::memcpy(&logFont, font->logFont.data(), bytes);
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
        const uint32_t textColor = ceMgdi_.textColorForDc(dc.hdc, dc.textColor);
        const uint32_t bkColor = ceMgdi_.bkColorForDc(dc.hdc, dc.bkColor);
        const uint32_t bkMode = ceMgdi_.bkModeForDc(dc.hdc, dc.bkMode);
        SetTextColor(memDc, COLORREF(textColor));
        SetBkColor(memDc, COLORREF(bkColor));
        SetBkMode(memDc, bkMode == 1 ? TRANSPARENT : OPAQUE);
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

    const uint32_t selectedBitmap = ceMgdi_.selectedBitmapForDc(dc.hdc, dc.selectedBitmap);
    auto dstBitmap = bitmaps_.find(selectedBitmap);
    if (dstBitmap != bitmaps_.end()) {
        GuestBitmap& bitmap = dstBitmap->second;
        syncBitmapPaletteFromMgdi(selectedBitmap, bitmap);
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

void SyntheticDllRuntime::syncBitmapPaletteFromMgdi(uint32_t hbitmap, GuestBitmap& bitmap) {
    const CeMgdi::BitmapState* bitmapState = ceMgdi_.bitmapState(hbitmap);
    if (bitmapState && !bitmapState->palette.empty()) bitmap.palette = bitmapState->palette;
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
        pixel = readBgra32Pixel(row, x);
    } else if (bitmap.bpp == 24) {
        const size_t o = size_t(x) * 3;
        pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
    } else if (bitmap.bpp == 16) {
        const size_t o = size_t(x) * 2;
        uint16_t v = 0;
        std::memcpy(&v, row + o, sizeof(v));
        pixel = decodeBitmap16(v, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
    } else if (bitmap.bpp == 8 && !bitmap.palette.empty()) {
        pixel = palettePixel(bitmap.palette, row[x]);
    } else if (bitmap.bpp == 4 && !bitmap.palette.empty()) {
        const uint8_t packed = row[size_t(x) / 2];
        const uint8_t index = uint8_t((x & 1) ? (packed & 0x0f) : (packed >> 4));
        pixel = palettePixel(bitmap.palette, index);
    } else if (bitmap.bpp == 1 && !bitmap.palette.empty()) {
        const uint8_t packed = row[size_t(x) / 8];
        const uint8_t index = uint8_t((packed >> (7 - (x & 7))) & 1);
        pixel = palettePixel(bitmap.palette, index);
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
    if (bitmap.bpp == 32) {
        writeBgra32Pixel(row, x, pixel);
    } else if (bitmap.bpp == 24) {
        const size_t o = size_t(x) * 3;
        row[o + 0] = uint8_t(pixel & 0xff);
        row[o + 1] = uint8_t((pixel >> 8) & 0xff);
        row[o + 2] = uint8_t((pixel >> 16) & 0xff);
    } else if (bitmap.bpp == 16) {
        const uint16_t v = encodeBitmap16(pixel, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
        const size_t o = size_t(x) * 2;
        std::memcpy(row + o, &v, sizeof(v));
    } else if ((bitmap.bpp == 8 || bitmap.bpp == 4 || bitmap.bpp == 1) && !bitmap.palette.empty()) {
        const uint8_t r = uint8_t((pixel >> 16) & 0xff);
        const uint8_t g = uint8_t((pixel >> 8) & 0xff);
        const uint8_t b = uint8_t(pixel & 0xff);
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

bool SyntheticDllRuntime::writeBitmapSpan(const GuestBitmap& bitmap,
                                          std::vector<uint8_t>& bits,
                                          int32_t height,
                                          int32_t y,
                                          int32_t left,
                                          int32_t right,
                                          uint32_t pixel) const {
    if (y < 0 || y >= height || bitmap.width <= 0 || bitmap.stride == 0) return false;
    left = std::clamp<int32_t>(left, 0, bitmap.width);
    right = std::clamp<int32_t>(right, 0, bitmap.width);
    if (left >= right) return true;

    const int32_t rowIndex = bitmap.heightRaw < 0 ? y : (height - 1 - y);
    uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
    const int32_t width = right - left;

    if (bitmap.bpp == 32) {
        const uint32_t value = 0xff000000u | (pixel & 0x00ffffffu);
        auto* dst = reinterpret_cast<uint32_t*>(row + size_t(left) * 4);
        std::fill(dst, dst + width, value);
        return true;
    }

    if (bitmap.bpp == 16) {
        const uint16_t value = encodeBitmap16(pixel, bitmap.redMask, bitmap.greenMask, bitmap.blueMask);
        auto* dst = reinterpret_cast<uint16_t*>(row + size_t(left) * 2);
        std::fill(dst, dst + width, value);
        return true;
    }

    for (int32_t x = left; x < right; ++x) {
        if (!writeBitmapPixel(bitmap, bits, height, x, y, pixel)) return false;
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
    if (!diagnostics_.dumpsEnabled()) return;
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
    if (!diagnostics_.dumpsEnabled()) return;
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
        mirrorMgdiBitmap(ret, bitmaps_[ret]);
    }
    lastError_ = ret ? 0 : 8;
    spdlog::info("CreateBitmap {}x{} planes={} bpp={} bits=0x{:08x} bitmap=0x{:08x}",
                 width, height, planes, bpp, bits, ret);
    return true;
}

bool SyntheticDllRuntime::handleGetObjectW(const GuestCallArgs& args, uint32_t& ret) {
    const CeMgdi::BitmapState* bitmap = ceMgdi_.bitmapState(args.a0);
    if (!bitmap) {
        lastError_ = 6;
        ret = 0;
        return true;
    }
    if (!args.a2) {
        lastError_ = 0;
        ret = kBitmapObjectBytes;
        return true;
    }

    const int32_t height = bitmap->heightRaw < 0 ? -bitmap->heightRaw : bitmap->heightRaw;
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
    putU32(4, uint32_t(bitmap->width));
    putU32(8, uint32_t(height));
    putU32(12, bitmap->stride);
    putU16(16, 1);
    putU16(18, bitmap->bpp);
    putU32(20, bitmap->bits);
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
    const uint32_t selectedBitmap = dc ? ceMgdi_.selectedBitmapForDc(args.a0, dc->selectedBitmap) : 0;
    CeMgdi::BitmapState* bitmapState = ceMgdi_.bitmapState(selectedBitmap);
    auto bitmap = dc ? bitmaps_.find(selectedBitmap) : bitmaps_.end();
    if (!dc || !bitmapState || bitmap == bitmaps_.end() || !args.a3 || bitmapState->bpp > 8) {
        lastError_ = dc ? 87 : 6;
        ret = 0;
        return true;
    }

    GuestBitmap& bm = bitmap->second;
    const uint32_t maxColors = 1u << bitmapState->bpp;
    const uint32_t start = std::min<uint32_t>(args.a1, maxColors);
    const uint32_t count = std::min<uint32_t>(args.a2, maxColors - start);
    if (bitmapState->palette.empty()) bitmapState->palette = defaultIndexedPalette(bitmapState->bpp);
    if (bitmapState->palette.size() < maxColors) bitmapState->palette.resize(maxColors, 0xff000000u);

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
        bitmapState->palette[size_t(start + i)] =
            0xff000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
    }
    bitmapState->paletteEntries = bitmapState->palette.size();
    bm.palette = bitmapState->palette;
    lastError_ = 0;
    ret = count;
    spdlog::info("SetDIBColorTable hdc=0x{:08x} bitmap=0x{:08x} start={} count={} ret={}",
                 args.a0, selectedBitmap, args.a1, args.a2, ret);
    return true;
}

bool SyntheticDllRuntime::handleSetBitmapBits(const GuestCallArgs& args, uint32_t& ret) {
    const CeMgdi::BitmapState* bitmap = ceMgdi_.bitmapState(args.a0);
    if (!bitmap || !args.a2) {
        lastError_ = bitmap ? 87 : 6;
        ret = 0;
        return true;
    }

    const uint64_t storageBytes = CeMgdi::bitmapStorageByteCount(*bitmap);
    const uint32_t cappedStorageBytes =
        storageBytes > UINT32_MAX ? UINT32_MAX : uint32_t(storageBytes);
    const uint32_t byteCount = std::min<uint32_t>(args.a1, cappedStorageBytes);
    std::vector<uint8_t> raw(byteCount);
    if (byteCount && uc_mem_read(uc_, args.a2, raw.data(), raw.size()) != UC_ERR_OK) {
        lastError_ = 998;
        ret = 0;
    } else if (byteCount && uc_mem_write(uc_, bitmap->bits, raw.data(), raw.size()) != UC_ERR_OK) {
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
    const CeMgdi::DcState* dcState = ceMgdi_.dcState(args.a0);
    const uint32_t selectedBitmap = dcState ? dcState->selectedBitmap : dc->selectedBitmap;
    auto dstBitmap = bitmaps_.find(selectedBitmap);
    if (dstBitmap != bitmaps_.end()) syncBitmapPaletteFromMgdi(selectedBitmap, dstBitmap->second);
    const bool ok = supported && (dstBitmap != bitmaps_.end()
        ? stretchDibToBitmap(dstBitmap->second, int32_t(args.a1), int32_t(args.a2), dstW, scanLines,
                             srcX, srcY, dstW, scanLines, stackArg(9), stackArg(10))
        : stretchDibToFramebuffer(*dc, int32_t(args.a1), int32_t(args.a2), dstW, scanLines,
                                  srcX, srcY, dstW, scanLines, stackArg(9), stackArg(10)));
    if (ok) {
        if (std::abs(dstW) >= 200 || std::abs(scanLines) >= 120 || dc->hwnd) {
            spdlog::info("SetDIBitsToDevice ok dst=0x{:08x} hwnd=0x{:08x} dstBitmap=0x{:08x} dst={},{} {}x{} srcOrigin={},{} startScan={} scanLines={} bits=0x{:08x} info=0x{:08x}",
                         args.a0, dc->hwnd, selectedBitmap, int32_t(args.a1), int32_t(args.a2),
                         dstW, dstH, srcX, int32_t(stackArg(6)), stackArg(7), stackArg(8),
                         stackArg(9), stackArg(10));
        }
        ret = uint32_t(std::abs(scanLines));
        lastError_ = 0;
    } else {
        spdlog::info("SetDIBitsToDevice failed dst=0x{:08x} dstBitmap=0x{:08x} "
                     "dst={}x{} srcOrigin={},{} startScan={} scanLines={} bits=0x{:08x} "
                     "info=0x{:08x} usage={}",
                     args.a0, selectedBitmap, dstW, dstH, srcX, int32_t(stackArg(6)),
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
    const auto clip = framebufferClipForDc(dc);
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < 0 || dstPy >= framebufferHeight_) continue;
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        if (sy < 0 || sy >= dibHeight) continue;
        const int32_t row = topDown ? sy : (dibHeight - 1 - sy);
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < 0 || dstPx >= framebufferWidth_) continue;
            if (clip && !CeMgdi::rectContainsPoint(*clip, dstPx, dstPy)) continue;
            const int32_t sx = srcX + (int64_t(x) * srcW) / outW;
            if (sx < 0 || sx >= dibWidth) continue;
            const uint8_t* p = bits.data() + size_t(row) * rowStride;
            uint32_t pixel = 0;
        if (bpp == 32) {
            const size_t o = size_t(sx) * 4;
            pixel = readBgra32Pixel(p, sx);
        } else if (bpp == 24) {
            const size_t o = size_t(sx) * 3;
            pixel = 0xff000000u | (uint32_t(p[o + 2]) << 16) | (uint32_t(p[o + 1]) << 8) | p[o];
        } else if (bpp == 16) {
            uint16_t v = 0;
            std::memcpy(&v, p + size_t(sx) * 2, sizeof(v));
            pixel = decodeBitmap16(v, redMask, greenMask, blueMask);
        } else if (bpp == 8 && !palette.empty()) {
            pixel = palettePixel(palette, p[sx]);
        } else if (bpp == 4 && !palette.empty()) {
            const uint8_t packed = p[size_t(sx) / 2];
            const uint8_t index = uint8_t((sx & 1) ? (packed & 0x0f) : (packed >> 4));
            pixel = palettePixel(palette, index);
        } else if (bpp == 1 && !palette.empty()) {
            const uint8_t packed = p[size_t(sx) / 8];
            const uint8_t index = uint8_t((packed >> (7 - (sx & 7))) & 1);
            pixel = palettePixel(palette, index);
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
            pixel = readBgra32Pixel(row, x);
        } else if (bpp == 24) {
            const size_t o = size_t(x) * 3;
            pixel = 0xff000000u | (uint32_t(row[o + 2]) << 16) | (uint32_t(row[o + 1]) << 8) | row[o];
        } else if (bpp == 16) {
            const size_t o = size_t(x) * 2;
            uint16_t v = 0;
            std::memcpy(&v, row + o, sizeof(v));
            pixel = decodeBitmap16(v, redMask, greenMask, blueMask);
        } else if (bpp == 8 && !palette.empty()) {
            pixel = palettePixel(palette, row[x]);
        } else if (bpp == 4 && !palette.empty()) {
            const uint8_t packed = row[size_t(x) / 2];
            const uint8_t index = uint8_t((x & 1) ? (packed & 0x0f) : (packed >> 4));
            pixel = palettePixel(palette, index);
        } else if (bpp == 1 && !palette.empty()) {
            const uint8_t packed = row[size_t(x) / 8];
            const uint8_t index = uint8_t((packed >> (7 - (x & 7))) & 1);
            pixel = palettePixel(palette, index);
        } else {
            return false;
        }
        return true;
    };

    auto writeDestPixel = [&](int32_t x, int32_t y, uint32_t pixel) -> bool {
        if (x < 0 || x >= dstBitmap.width || y < 0 || y >= dstHeight) return false;
        const int32_t rowIndex = dstBitmap.heightRaw < 0 ? y : (dstHeight - 1 - y);
        uint8_t* row = dstBits.data() + size_t(rowIndex) * size_t(dstBitmap.stride);
        if (dstBitmap.bpp == 32) {
            writeBgra32Pixel(row, x, pixel);
        } else if (dstBitmap.bpp == 24) {
            const size_t o = size_t(x) * 3;
            row[o + 0] = uint8_t(pixel & 0xff);
            row[o + 1] = uint8_t((pixel >> 8) & 0xff);
            row[o + 2] = uint8_t((pixel >> 16) & 0xff);
        } else if (dstBitmap.bpp == 16) {
            const uint16_t v = encodeBitmap16(pixel, dstBitmap.redMask, dstBitmap.greenMask, dstBitmap.blueMask);
            const size_t o = size_t(x) * 2;
            std::memcpy(row + o, &v, sizeof(v));
        } else if (dstBitmap.bpp == 8 && !dstBitmap.palette.empty()) {
            const uint8_t r = uint8_t((pixel >> 16) & 0xff);
            const uint8_t g = uint8_t((pixel >> 8) & 0xff);
            const uint8_t b = uint8_t(pixel & 0xff);
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
            const uint8_t r = uint8_t((pixel >> 16) & 0xff);
            const uint8_t g = uint8_t((pixel >> 8) & 0xff);
            const uint8_t b = uint8_t(pixel & 0xff);
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

    int32_t clipLeft = std::clamp<int32_t>(outLeft, 0, framebufferWidth_);
    int32_t clipTop = std::clamp<int32_t>(outTop, 0, framebufferHeight_);
    int32_t clipRight = std::clamp<int32_t>(outLeft + outW, 0, framebufferWidth_);
    int32_t clipBottom = std::clamp<int32_t>(outTop + outH, 0, framebufferHeight_);
    if (const auto dcClip = framebufferClipForDc(dstDc)) {
        clipLeft = std::max(clipLeft, dcClip->left);
        clipTop = std::max(clipTop, dcClip->top);
        clipRight = std::min(clipRight, dcClip->right);
        clipBottom = std::min(clipBottom, dcClip->bottom);
    }
    if (clipLeft >= clipRight || clipTop >= clipBottom) {
        invalidateHostWindows();
        return true;
    }

    const uint32_t coveringPopup = dstDc.hwnd ? coveringFullScreenOwnedPopup(dstDc.hwnd) : 0;

    struct BackingLayer {
        GuestWindow* window{};
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };
    std::vector<BackingLayer> backingLayers;
    const uint64_t targetZ = dstDc.hwnd ? windowZOrder(dstDc.hwnd) : 0;
    for (auto& [hwnd, window] : ceGwe_.windows()) {
        if (!window.visible || !window.backingValid || window.backingPixels.empty() ||
            window.backingWidth <= 0 || window.backingHeight <= 0 ||
            isWindowOrDescendant(dstDc.hwnd, hwnd)) {
            continue;
        }
        if (dstDc.hwnd && window.zOrder < targetZ && hwnd != coveringPopup) continue;
        const int32_t layerLeft = std::max<int32_t>(clipLeft, window.backingX);
        const int32_t layerTop = std::max<int32_t>(clipTop, window.backingY);
        const int32_t layerRight = std::min<int32_t>(clipRight, window.backingX + window.backingWidth);
        const int32_t layerBottom = std::min<int32_t>(clipBottom, window.backingY + window.backingHeight);
        if (layerLeft < layerRight && layerTop < layerBottom) {
            backingLayers.push_back(BackingLayer{&window, layerLeft, layerTop, layerRight, layerBottom});
        }
    }

    auto readDestinationPixel = [&](int32_t x, int32_t y) -> uint32_t {
        for (const BackingLayer& layer : backingLayers) {
            if (x < layer.left || y < layer.top || x >= layer.right || y >= layer.bottom) continue;
            const size_t offset = size_t(y - layer.window->backingY) * size_t(layer.window->backingWidth) +
                                  size_t(x - layer.window->backingX);
            if (offset < layer.window->backingPixels.size()) return layer.window->backingPixels[offset];
        }
        return framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)];
    };

    auto writeDestinationPixel = [&](int32_t x, int32_t y, uint32_t pixel) {
        bool covered = false;
        for (const BackingLayer& layer : backingLayers) {
            if (x < layer.left || y < layer.top || x >= layer.right || y >= layer.bottom) continue;
            const size_t offset = size_t(y - layer.window->backingY) * size_t(layer.window->backingWidth) +
                                  size_t(x - layer.window->backingX);
            if (offset < layer.window->backingPixels.size()) {
                layer.window->backingPixels[offset] = pixel;
                covered = true;
            }
        }
        if (!covered && !coveringPopup) framebuffer_[size_t(y) * size_t(framebufferWidth_) + size_t(x)] = pixel;
    };

    const bool sameScaleX = srcW == outW;
    const bool sameScaleY = srcH == outH;
    const bool ropSourceCopy = rop == 0x00cc0020u;
    auto tryCopyRgb565ToFramebuffer = [&]() {
#if defined(__AVX2__) || defined(__AVX512F__)
        const bool rgb565 =
            bitmap.bpp == 16 &&
            ((!bitmap.redMask && !bitmap.greenMask && !bitmap.blueMask) ||
             (bitmap.redMask == kRgb565RedMask &&
              bitmap.greenMask == kRgb565GreenMask &&
              bitmap.blueMask == kRgb565BlueMask));
        if (!rgb565 || coveringPopup || !backingLayers.empty() || !ropSourceCopy ||
            !sameScaleX || !sameScaleY ||
            srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
            return false;
        }

        const int32_t copyLeft = std::max({0, clipLeft - outLeft, -srcX});
        const int32_t copyTop = std::max({0, clipTop - outTop, -srcY});
        const int32_t copyRight = std::min({outW, clipRight - outLeft, bitmap.width - srcX});
        const int32_t copyBottom = std::min({outH, clipBottom - outTop, bitmapHeight - srcY});
        if (copyRight <= copyLeft || copyBottom <= copyTop) return true;

        const int32_t copyWidth = copyRight - copyLeft;
        for (int32_t y = copyTop; y < copyBottom; ++y) {
            const int32_t sy = srcY + y;
            const int32_t dstPy = outTop + y;
            const int32_t sx = srcX + copyLeft;
            const int32_t dstPx = outLeft + copyLeft;
            const int32_t rowIndex = topDown ? sy : (bitmapHeight - 1 - sy);
            const uint8_t* srcRow = bits.data() + size_t(rowIndex) * size_t(bitmap.stride) + size_t(sx) * 2;
            auto* dstRow = reinterpret_cast<uint8_t*>(
                framebuffer_ + size_t(dstPy) * size_t(framebufferWidth_) + size_t(dstPx));
            convertRgb565ToBgra32Avx2(srcRow, dstRow, copyWidth);
        }
        return true;
#else
        return false;
#endif
    };
    if (tryCopyRgb565ToFramebuffer()) {
        invalidateHostWindows();
        return true;
    }
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < clipTop || dstPy >= clipBottom) continue;
        const int32_t sy = sameScaleY ? srcY + y : srcY + (int64_t(y) * srcH) / outH;
        if (sy < 0 || sy >= bitmapHeight) continue;
        const int32_t rowIndex = topDown ? sy : (bitmapHeight - 1 - sy);
        const uint8_t* row = bits.data() + size_t(rowIndex) * size_t(bitmap.stride);
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < clipLeft || dstPx >= clipRight) continue;
            const int32_t sx = sameScaleX ? srcX + x : srcX + (int64_t(x) * srcW) / outW;
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
            const uint32_t outPixel = ropSourceCopy
                ? pixel
                : applySourceRasterOp(rop, pixel, readDestinationPixel(dstPx, dstPy));
            writeDestinationPixel(dstPx, dstPy, outPixel);
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
    const bool sameScaleX = srcW == outW;
    const bool sameScaleY = srcH == outH;
    const bool ropSourceCopy = rop == 0x00cc0020u;
    auto isRgb565Bitmap = [](const GuestBitmap& bitmap) {
        return bitmap.bpp == 16 &&
               ((!bitmap.redMask && !bitmap.greenMask && !bitmap.blueMask) ||
                (bitmap.redMask == kRgb565RedMask &&
                 bitmap.greenMask == kRgb565GreenMask &&
                 bitmap.blueMask == kRgb565BlueMask));
    };
    auto tryCopyRgb565ToBgra32 = [&]() {
#if defined(__AVX2__) || defined(__AVX512F__)
        if (!isRgb565Bitmap(srcBitmap) || dstBitmap.bpp != 32 || outW <= 0 || outH <= 0) return false;
        const int32_t copyLeft = std::max({0, -outLeft, -srcX});
        const int32_t copyTop = std::max({0, -outTop, -srcY});
        const int32_t copyRight = std::min({outW, dstBitmap.width - outLeft, srcBitmap.width - srcX});
        const int32_t copyBottom = std::min({outH, dstHeight - outTop, srcHeight - srcY});
        if (copyRight <= copyLeft || copyBottom <= copyTop) return true;
        const int32_t copyWidth = copyRight - copyLeft;
        for (int32_t y = copyTop; y < copyBottom; ++y) {
            const int32_t sy = srcY + y;
            const int32_t dy = outTop + y;
            const int32_t sx = srcX + copyLeft;
            const int32_t dx = outLeft + copyLeft;
            const int32_t srcRowIndex = srcBitmap.heightRaw < 0 ? sy : (srcHeight - 1 - sy);
            const int32_t dstRowIndex = dstBitmap.heightRaw < 0 ? dy : (dstHeight - 1 - dy);
            const uint8_t* srcRow = srcBits.data() + size_t(srcRowIndex) * size_t(srcBitmap.stride) + size_t(sx) * 2;
            uint8_t* dstRow = dstBits.data() + size_t(dstRowIndex) * size_t(dstBitmap.stride) + size_t(dx) * 4;
            convertRgb565ToBgra32Avx2(srcRow, dstRow, copyWidth);
        }
        return true;
#else
        return false;
#endif
    };
    if (ropSourceCopy && sameScaleX && sameScaleY && srcW > 0 && srcH > 0 && dstW > 0 && dstH > 0 &&
        tryCopyRgb565ToBgra32()) {
        return uc_mem_write(uc_, dstBitmap.bits, dstBits.data(), dstBits.size()) == UC_ERR_OK;
    }
    for (int32_t y = 0; y < outH; ++y) {
        const int32_t sy = sameScaleY ? (srcY + y) : (srcY + (int64_t(y) * srcH) / outH);
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t sx = sameScaleX ? (srcX + x) : (srcX + (int64_t(x) * srcW) / outW);
            uint32_t pixel = 0;
            const int32_t dx = outLeft + x;
            const int32_t dy = outTop + y;
            if (!readBitmapPixel(srcBitmap, srcBits, srcHeight, sx, sy, pixel)) continue;
            if (ropSourceCopy) {
                writeDestPixel(dx, dy, pixel);
            } else {
                uint32_t dstPixel = 0;
                if (!readBitmapPixel(dstBitmap, dstBits, dstHeight, dx, dy, dstPixel)) continue;
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
    const auto clip = framebufferClipForDc(dstDc);
    noteGuestWindowPaint(dstDc.hwnd, outLeft, outTop, outLeft + outW, outTop + outH);

    for (int32_t y = 0; y < outH; ++y) {
        const int32_t dstPy = outTop + y;
        if (dstPy < 0 || dstPy >= framebufferHeight_) continue;
        const int32_t sy = srcY + (int64_t(y) * srcH) / outH;
        for (int32_t x = 0; x < outW; ++x) {
            const int32_t dstPx = outLeft + x;
            if (dstPx < 0 || dstPx >= framebufferWidth_) continue;
            if (clip && !CeMgdi::rectContainsPoint(*clip, dstPx, dstPy)) continue;
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

