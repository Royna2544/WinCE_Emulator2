#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string_view>

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

    static constexpr std::string_view name() noexcept { return "CE MGDI"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for DC state, GDI objects, clipping, and window bitmap semantics.";
    }

    static bool rectContainsPoint(const Rect& rect, int32_t x, int32_t y) noexcept {
        return x >= rect.left && y >= rect.top && x < rect.right && y < rect.bottom;
    }

    void createDc(uint32_t hdc, uint32_t hwnd) {
        if (!hdc) return;
        auto& state = dcStates_[hdc];
        state = DcState{};
        state.hdc = hdc;
        state.hwnd = hwnd;
    }

    void destroyDc(uint32_t hdc) {
        dcStates_.erase(hdc);
    }

    DcState* dcState(uint32_t hdc) {
        auto it = dcStates_.find(hdc);
        return it == dcStates_.end() ? nullptr : &it->second;
    }

    const DcState* dcState(uint32_t hdc) const {
        auto it = dcStates_.find(hdc);
        return it == dcStates_.end() ? nullptr : &it->second;
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

private:
    std::map<uint32_t, DcState> dcStates_;
};
