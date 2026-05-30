#pragma once

#include <string_view>

class CeMgdi {
public:
    static constexpr std::string_view name() noexcept { return "CE MGDI"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for DC state, GDI objects, clipping, and window bitmap semantics.";
    }
};
