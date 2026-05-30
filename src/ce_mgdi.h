#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class CeMgdi {
public:
    struct Rect {
        int32_t left{};
        int32_t top{};
        int32_t right{};
        int32_t bottom{};
    };

    struct DcState {
        uint32_t hdc{};
        uint32_t hwnd{};
        uint32_t selectedBrush{};
        uint32_t selectedPen{};
        uint32_t selectedFont{};
        uint32_t selectedBitmap{};
        uint32_t textColor{0x00000000};
        uint32_t bkColor{0x00ffffff};
        uint32_t bkMode{1};
        uint32_t textAlign{};
        int32_t x{};
        int32_t y{};
        bool hasSystemClip{};
        Rect systemClip;
        bool hasAppClip{};
        Rect appClip;
    };

    struct BitmapState {
        uint32_t hbitmap{};
        int32_t width{};
        int32_t heightRaw{};
        uint16_t bpp{};
        uint32_t stride{};
        uint32_t bits{};
        uint32_t redMask{};
        uint32_t greenMask{};
        uint32_t blueMask{};
        size_t paletteEntries{};
        std::vector<uint32_t> palette;
        bool stock{};
    };

    struct BrushState {
        uint32_t hbrush{};
        uint32_t colorRef{};
        uint32_t patternBitmap{};
        bool stock{};
    };

    struct PenState {
        uint32_t hpen{};
        uint32_t style{};
        uint32_t width{};
        uint32_t colorRef{};
        bool stock{};
    };

    struct FontState {
        uint32_t hfont{};
        std::array<uint8_t, 92> logFont{};
        bool stock{};
    };

    struct RegionState {
        uint32_t hregion{};
        bool hasBounds{};
        Rect bounds;
    };

    struct WindowBitmapState {
        uint32_t hwnd{};
        Rect viewport;
        Rect systemClip;
        bool hasSystemClip{};
        size_t dcCount{};
    };

    static constexpr std::string_view name() noexcept { return "CE MGDI"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for DC state, GDI objects, clipping, and window bitmap semantics.";
    }

    static bool rectContainsPoint(const Rect& rect, int32_t x, int32_t y) noexcept {
        return x >= rect.left && y >= rect.top && x < rect.right && y < rect.bottom;
    }

    static uint64_t bitmapStorageByteCount(const BitmapState& bitmap) noexcept {
        const uint32_t height = uint32_t(bitmap.heightRaw < 0 ? -bitmap.heightRaw : bitmap.heightRaw);
        return uint64_t(bitmap.stride) * uint64_t(height);
    }

    void createDc(uint32_t hdc, uint32_t hwnd) {
        if (!hdc) return;
        if (const auto existing = dcStates_.find(hdc); existing != dcStates_.end()) {
            removeDcFromWindowBitmap(existing->second.hwnd);
        }
        auto& state = dcStates_[hdc];
        state = DcState{};
        state.hdc = hdc;
        state.hwnd = hwnd;
        addDcToWindowBitmap(hwnd);
    }

    void destroyDc(uint32_t hdc) {
        auto it = dcStates_.find(hdc);
        if (it != dcStates_.end()) {
            removeDcFromWindowBitmap(it->second.hwnd);
            dcStates_.erase(it);
        }
    }

    DcState* dcState(uint32_t hdc) {
        auto it = dcStates_.find(hdc);
        return it == dcStates_.end() ? nullptr : &it->second;
    }

    const DcState* dcState(uint32_t hdc) const {
        auto it = dcStates_.find(hdc);
        return it == dcStates_.end() ? nullptr : &it->second;
    }

    uint32_t selectedBitmapForDc(uint32_t hdc, uint32_t fallback = 0) const {
        const DcState* state = dcState(hdc);
        return state ? state->selectedBitmap : fallback;
    }

    uint32_t selectedBrushForDc(uint32_t hdc, uint32_t fallback = 0) const {
        const DcState* state = dcState(hdc);
        return state ? state->selectedBrush : fallback;
    }

    uint32_t selectedPenForDc(uint32_t hdc, uint32_t fallback = 0) const {
        const DcState* state = dcState(hdc);
        return state ? state->selectedPen : fallback;
    }

    uint32_t selectedFontForDc(uint32_t hdc, uint32_t fallback = 0) const {
        const DcState* state = dcState(hdc);
        return state ? state->selectedFont : fallback;
    }

    uint32_t textColorForDc(uint32_t hdc, uint32_t fallback = 0x00000000) const {
        const DcState* state = dcState(hdc);
        return state ? state->textColor : fallback;
    }

    uint32_t bkColorForDc(uint32_t hdc, uint32_t fallback = 0x00ffffff) const {
        const DcState* state = dcState(hdc);
        return state ? state->bkColor : fallback;
    }

    uint32_t bkModeForDc(uint32_t hdc, uint32_t fallback = 1) const {
        const DcState* state = dcState(hdc);
        return state ? state->bkMode : fallback;
    }

    uint32_t textAlignForDc(uint32_t hdc, uint32_t fallback = 0) const {
        const DcState* state = dcState(hdc);
        return state ? state->textAlign : fallback;
    }

    std::pair<int32_t, int32_t> currentPositionForDc(uint32_t hdc,
                                                     int32_t fallbackX = 0,
                                                     int32_t fallbackY = 0) const {
        const DcState* state = dcState(hdc);
        return state ? std::pair<int32_t, int32_t>{state->x, state->y}
                     : std::pair<int32_t, int32_t>{fallbackX, fallbackY};
    }

    void updateSelectedObjects(uint32_t hdc,
                               uint32_t brush,
                               uint32_t pen,
                               uint32_t font,
                               uint32_t bitmap) {
        if (auto* state = dcState(hdc)) {
            state->selectedBrush = brush;
            state->selectedPen = pen;
            state->selectedFont = font;
            state->selectedBitmap = bitmap;
        }
    }

    void setSelectedBrush(uint32_t hdc, uint32_t brush) {
        if (auto* state = dcState(hdc)) state->selectedBrush = brush;
    }

    void setSelectedPen(uint32_t hdc, uint32_t pen) {
        if (auto* state = dcState(hdc)) state->selectedPen = pen;
    }

    void setSelectedFont(uint32_t hdc, uint32_t font) {
        if (auto* state = dcState(hdc)) state->selectedFont = font;
    }

    void setSelectedBitmap(uint32_t hdc, uint32_t bitmap) {
        if (auto* state = dcState(hdc)) state->selectedBitmap = bitmap;
    }

    void setTextColor(uint32_t hdc, uint32_t color) {
        if (auto* state = dcState(hdc)) state->textColor = color;
    }

    void setBkColor(uint32_t hdc, uint32_t color) {
        if (auto* state = dcState(hdc)) state->bkColor = color;
    }

    void setBkMode(uint32_t hdc, uint32_t mode) {
        if (auto* state = dcState(hdc)) state->bkMode = mode;
    }

    void setTextAlign(uint32_t hdc, uint32_t align) {
        if (auto* state = dcState(hdc)) state->textAlign = align;
    }

    void setCurrentPosition(uint32_t hdc, int32_t x, int32_t y) {
        if (auto* state = dcState(hdc)) {
            state->x = x;
            state->y = y;
        }
    }

    void setSystemClip(uint32_t hdc, Rect rect) {
        if (auto* state = dcState(hdc)) {
            state->hasSystemClip = true;
            state->systemClip = rect;
        }
    }

    void clearSystemClip(uint32_t hdc) {
        if (auto* state = dcState(hdc)) {
            state->hasSystemClip = false;
            state->systemClip = Rect{};
        }
    }

    void setAppClip(uint32_t hdc, Rect rect) {
        if (auto* state = dcState(hdc)) {
            state->hasAppClip = true;
            state->appClip = rect;
        }
    }

    void clearAppClip(uint32_t hdc) {
        if (auto* state = dcState(hdc)) {
            state->hasAppClip = false;
            state->appClip = Rect{};
        }
    }

    std::optional<Rect> systemClip(uint32_t hdc) const {
        const DcState* state = dcState(hdc);
        if (!state || !state->hasSystemClip) return std::nullopt;
        return state->systemClip;
    }

    const std::map<uint32_t, DcState>& dcStates() const noexcept { return dcStates_; }

    void trackBitmap(const BitmapState& bitmap) {
        if (!bitmap.hbitmap) return;
        bitmapStates_[bitmap.hbitmap] = bitmap;
    }

    void destroyBitmap(uint32_t hbitmap) {
        bitmapStates_.erase(hbitmap);
    }

    BitmapState* bitmapState(uint32_t hbitmap) {
        auto it = bitmapStates_.find(hbitmap);
        return it == bitmapStates_.end() ? nullptr : &it->second;
    }

    const BitmapState* bitmapState(uint32_t hbitmap) const {
        auto it = bitmapStates_.find(hbitmap);
        return it == bitmapStates_.end() ? nullptr : &it->second;
    }

    void setBitmapPaletteEntries(uint32_t hbitmap, size_t entries) {
        if (auto* state = bitmapState(hbitmap)) state->paletteEntries = entries;
    }

    void setBitmapPalette(uint32_t hbitmap, std::vector<uint32_t> palette) {
        if (auto* state = bitmapState(hbitmap)) {
            state->palette = std::move(palette);
            state->paletteEntries = state->palette.size();
        }
    }

    const std::map<uint32_t, BitmapState>& bitmapStates() const noexcept { return bitmapStates_; }

    void trackBrush(const BrushState& brush) {
        if (!brush.hbrush) return;
        brushStates_[brush.hbrush] = brush;
    }

    void setBrushPatternBitmap(uint32_t hbrush, uint32_t hbitmap) {
        if (auto* state = brushState(hbrush)) state->patternBitmap = hbitmap;
    }

    void destroyBrush(uint32_t hbrush) {
        brushStates_.erase(hbrush);
    }

    BrushState* brushState(uint32_t hbrush) {
        auto it = brushStates_.find(hbrush);
        return it == brushStates_.end() ? nullptr : &it->second;
    }

    const BrushState* brushState(uint32_t hbrush) const {
        auto it = brushStates_.find(hbrush);
        return it == brushStates_.end() ? nullptr : &it->second;
    }

    void trackPen(const PenState& pen) {
        if (!pen.hpen) return;
        penStates_[pen.hpen] = pen;
    }

    void destroyPen(uint32_t hpen) {
        penStates_.erase(hpen);
    }

    PenState* penState(uint32_t hpen) {
        auto it = penStates_.find(hpen);
        return it == penStates_.end() ? nullptr : &it->second;
    }

    const PenState* penState(uint32_t hpen) const {
        auto it = penStates_.find(hpen);
        return it == penStates_.end() ? nullptr : &it->second;
    }

    void trackFont(const FontState& font) {
        if (!font.hfont) return;
        fontStates_[font.hfont] = font;
    }

    void destroyFont(uint32_t hfont) {
        fontStates_.erase(hfont);
    }

    FontState* fontState(uint32_t hfont) {
        auto it = fontStates_.find(hfont);
        return it == fontStates_.end() ? nullptr : &it->second;
    }

    const FontState* fontState(uint32_t hfont) const {
        auto it = fontStates_.find(hfont);
        return it == fontStates_.end() ? nullptr : &it->second;
    }

    void trackRegion(const RegionState& region) {
        if (!region.hregion) return;
        regionStates_[region.hregion] = region;
    }

    void destroyRegion(uint32_t hregion) {
        regionStates_.erase(hregion);
    }

    RegionState* regionState(uint32_t hregion) {
        auto it = regionStates_.find(hregion);
        return it == regionStates_.end() ? nullptr : &it->second;
    }

    const RegionState* regionState(uint32_t hregion) const {
        auto it = regionStates_.find(hregion);
        return it == regionStates_.end() ? nullptr : &it->second;
    }

    void updateWindowBitmap(uint32_t hwnd, Rect viewport, std::optional<Rect> systemClip) {
        if (!hwnd) return;
        auto& state = windowBitmapStates_[hwnd];
        state.hwnd = hwnd;
        state.viewport = viewport;
        if (systemClip) {
            state.hasSystemClip = true;
            state.systemClip = *systemClip;
        } else {
            state.hasSystemClip = false;
            state.systemClip = Rect{};
        }
    }

    void destroyWindowBitmap(uint32_t hwnd) {
        windowBitmapStates_.erase(hwnd);
    }

    const WindowBitmapState* windowBitmapState(uint32_t hwnd) const {
        auto it = windowBitmapStates_.find(hwnd);
        return it == windowBitmapStates_.end() ? nullptr : &it->second;
    }

    const std::map<uint32_t, WindowBitmapState>& windowBitmapStates() const noexcept {
        return windowBitmapStates_;
    }

private:
    void addDcToWindowBitmap(uint32_t hwnd) {
        if (!hwnd) return;
        auto& state = windowBitmapStates_[hwnd];
        state.hwnd = hwnd;
        ++state.dcCount;
    }

    void removeDcFromWindowBitmap(uint32_t hwnd) {
        if (!hwnd) return;
        auto it = windowBitmapStates_.find(hwnd);
        if (it != windowBitmapStates_.end() && it->second.dcCount) --it->second.dcCount;
    }

    std::map<uint32_t, DcState> dcStates_;
    std::map<uint32_t, BitmapState> bitmapStates_;
    std::map<uint32_t, BrushState> brushStates_;
    std::map<uint32_t, PenState> penStates_;
    std::map<uint32_t, FontState> fontStates_;
    std::map<uint32_t, RegionState> regionStates_;
    std::map<uint32_t, WindowBitmapState> windowBitmapStates_;
};
