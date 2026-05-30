#pragma once

#include <string_view>

class CeKernel {
public:
    static constexpr std::string_view name() noexcept { return "CE virtual kernel"; }
    static constexpr std::string_view role() noexcept {
        return "Future owner for CE handles, processes, threads, waits, and kernel-call dispatch.";
    }
};
