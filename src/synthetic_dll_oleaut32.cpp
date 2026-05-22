#include "synthetic_dll.h"

#include <cstdlib>
#include <string>

std::optional<SyntheticModule> SyntheticDllRuntime::createOleAut32() {
    struct OleAut32Dll {
        SyntheticDllSpec spec() const {
            return SyntheticDllSpec{
                "OLEAUT32.dll",
                512,
                {
                    {0x0001, {"CreateErrorInfo"}},
                    {0x000C, {"LoadRegTypeLib"}},
                    {0x000D, {"LoadTypeLib"}},
                    {0x000F, {"RegisterTypeLib"}},
                    {0x0025, {"SetErrorInfo"}},
                    {0x0026, {"SysAllocString"}},
                    {0x0027, {"SysAllocStringByteLen"}},
                    {0x0028, {"SysAllocStringLen"}},
                    {0x0029, {"SysFreeString"}},
                    {0x002C, {"SysStringByteLen"}},
                    {0x002D, {"SysStringLen"}},
                    {0x00D8, {"VarUI4FromStr"}},
                    {0x00DC, {"VariantChangeType"}},
                    {0x00DE, {"VariantClear"}},
                },
            };
        }
    };

    const OleAut32Dll oleaut32;
    return createGenericOrdinalDll(oleaut32.spec());
}

bool SyntheticDllRuntime::dispatchOleAut32(const std::string& name,
                                           const GuestCallArgs& args,
                                           uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    auto allocBstr = [&](uint32_t source, uint32_t chars) -> uint32_t {
        if (!chars && source) chars = uint32_t(readUtf16(source, 65536).size());
        const uint32_t base = allocate(4 + chars * 2 + 2, true);
        writeU32(base, chars * 2);
        if (source && chars) copyGuest(base + 4, source, chars * 2);
        return base ? base + 4 : 0;
    };
    if (name == "SysAllocString") {
        ret = allocBstr(a0, 0);
    } else if (name == "SysAllocStringLen") {
        ret = allocBstr(a0, a1);
    } else if (name == "SysAllocStringByteLen") {
        const uint32_t base = allocate(4 + a1 + 2, true);
        writeU32(base, a1);
        if (a0 && a1) copyGuest(base + 4, a0, a1);
        ret = base ? base + 4 : 0;
    } else if (name == "SysFreeString") {
        if (a0 >= 4) releaseAllocation(a0 - 4);
        ret = 0;
    } else if (name == "SysStringLen") {
        ret = a0 >= 4 ? readU32(a0 - 4) / 2 : 0;
    } else if (name == "SysStringByteLen") {
        ret = a0 >= 4 ? readU32(a0 - 4) : 0;
    } else if (name == "VariantClear") {
        if (a0) {
            const uint16_t empty = 0;
            uc_mem_write(uc_, a0, &empty, sizeof(empty));
        }
        ret = 0;
    } else if (name == "VariantChangeType") {
        if (a0 && a1) copyGuest(a0, a1, 16);
        ret = 0;
    } else if (name == "VarUI4FromStr") {
        const std::string value = readUtf16(a0, 128);
        char* end = nullptr;
        const uint32_t parsed = uint32_t(std::strtoul(value.c_str(), &end, 0));
        if (a3) writeU32(a3, parsed);
        ret = 0;
    } else if (name == "CreateErrorInfo" || name == "SetErrorInfo" ||
               name == "LoadRegTypeLib" || name == "LoadTypeLib" || name == "RegisterTypeLib") {
        ret = 0x80004001u;
    } else {
        return false;
    }
    return true;
}
