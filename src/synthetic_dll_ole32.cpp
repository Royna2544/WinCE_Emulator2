#define WIN32_LEAN_AND_MEAN
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <windows.h>
#include <objbase.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <string>
#include <utility>

std::optional<SyntheticModule> SyntheticDllRuntime::createOle32() {
    struct Ole32Dll {
        SyntheticDllSpec spec() const {
            return SyntheticDllSpec{
                "ole32.dll",
                512,
                {
                    {0x0000, {"CLSIDFromProgID"}},
                    {0x0001, {"CLSIDFromString"}},
                    {0x0002, {"CoCreateInstance"}},
                    {0x0006, {"CoInitializeEx"}},
                    {0x0009, {"CoTaskMemAlloc"}},
                    {0x000A, {"CoTaskMemFree"}},
                    {0x000B, {"CoTaskMemRealloc"}},
                    {0x000D, {"CoUninitialize"}},
                    {0x000E, {"CoTaskMemSize"}},
                    {0x0011, {"ProgIDFromCLSID"}},
                    {0x0013, {"StringFromGUID2"}},
                    {0x0014, {"StringFromIID"}},
                    {0x001B, {"CoCreateGuid"}},
                    {0x001C, {"ReadClassStm"}},
                    {0x001D, {"OleSave"}},
                    {0x001E, {"OleRun"}},
                    {0x001F, {"OleIsRunning"}},
                    {0x0022, {"StringFromCLSID"}},
                    {0x0025, {"CreateOleAdviseHolder"}},
                    {0x0026, {"WriteClassStm"}},
                    {0x0027, {"OleDraw"}},
                    {0x0028, {"OleSetContainedObject"}},
                    {0x01F0, {"__ComQueryInterface"}},
                    {0x01F1, {"__ComAddRef"}},
                    {0x01F2, {"__ComRelease"}},
                },
            };
        }
    };

    const Ole32Dll ole32;
    auto module = createGenericOrdinalDll(ole32.spec());
    if (!module) return module;

    comQueryInterfaceStub_ = module->imageBase + module->exportsByOrdinal[0x01F0];
    comAddRefStub_ = module->imageBase + module->exportsByOrdinal[0x01F1];
    comReleaseStub_ = module->imageBase + module->exportsByOrdinal[0x01F2];
    return module;
}

bool SyntheticDllRuntime::dispatchOle32(const std::string& name,
                                        const GuestCallArgs& args,
                                        uint32_t& ret) {
    const uint32_t a0 = args.a0;
    const uint32_t a1 = args.a1;
    const uint32_t a2 = args.a2;
    const uint32_t a3 = args.a3;
    auto readGuid = [&](uint32_t ptr, GUID& guid) -> bool {
        return ptr && uc_mem_read(uc_, ptr, &guid, sizeof(guid)) == UC_ERR_OK;
    };
    auto wideFromGuest = [&](uint32_t ptr) {
        const std::string text = readUtf16(ptr, 256);
        return std::wstring(text.begin(), text.end());
    };
    auto asciiFromWide = [](const wchar_t* text) {
        std::string out;
        for (const wchar_t* p = text; p && *p; ++p) out.push_back(*p < 0x80 ? char(*p) : '?');
        return out;
    };
    auto comHandleFromThis = [&](uint32_t thisPtr) -> std::pair<uint32_t, GuestHandle*> {
        if (!thisPtr) return {0, nullptr};
        const uint32_t guestHandle = readU32(thisPtr + 4);
        GuestHandle* handle = lookupGuestHandle(guestHandle);
        if (!handle || handle->kind != GuestHandle::Kind::HostComInterface || !handle->hostValue) {
            return {guestHandle, nullptr};
        }
        return {guestHandle, handle};
    };
    if (name == "__ComQueryInterface") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        (void)guestHandle;
        GUID iid{};
        IUnknown* out = nullptr;
        HRESULT hr = E_POINTER;
        if (a2) writeU32(a2, 0);
        if (handle && readGuid(a1, iid) && a2) {
            hr = reinterpret_cast<IUnknown*>(handle->hostValue)->QueryInterface(iid, reinterpret_cast<void**>(&out));
            if (SUCCEEDED(hr) && out) writeU32(a2, makeGuestComProxy(reinterpret_cast<uintptr_t>(out)));
        }
        ret = uint32_t(hr);
    } else if (name == "__ComAddRef") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        (void)guestHandle;
        ret = handle ? reinterpret_cast<IUnknown*>(handle->hostValue)->AddRef() : 0;
    } else if (name == "__ComRelease") {
        auto [guestHandle, handle] = comHandleFromThis(a0);
        if (!handle) {
            ret = 0;
        } else {
            ret = reinterpret_cast<IUnknown*>(handle->hostValue)->Release();
            if (!ret) ceKernel_.handles().erase(guestHandle);
        }
    } else if (name == "CoInitializeEx") {
        ret = uint32_t(::CoInitializeEx(nullptr, a1));
    } else if (name == "CoUninitialize") {
        ::CoUninitialize();
        ret = 0;
    } else if (name == "CoTaskMemAlloc") {
        ret = allocate(a0, false);
    } else if (name == "CoTaskMemRealloc") {
        if (!a1) {
            releaseAllocation(a0);
            ret = 0;
        } else {
            const uint32_t oldSize = allocationSize(a0);
            ret = allocate(a1, false);
            if (ret && a0 && oldSize) {
                copyGuest(ret, a0, std::min(oldSize, a1));
                releaseAllocation(a0);
            }
        }
    } else if (name == "CoTaskMemFree") {
        releaseAllocation(a0);
        ret = 0;
    } else if (name == "CoTaskMemSize") {
        ret = allocationSize(a0);
    } else if (name == "CoCreateGuid") {
        GUID guid{};
        const HRESULT hr = ::CoCreateGuid(&guid);
        if (SUCCEEDED(hr) && a0) uc_mem_write(uc_, a0, &guid, sizeof(guid));
        ret = uint32_t(hr);
    } else if (name == "CLSIDFromString" || name == "CLSIDFromProgID") {
        GUID guid{};
        const std::wstring text = wideFromGuest(a0);
        const HRESULT hr = name == "CLSIDFromString"
            ? ::CLSIDFromString(text.c_str(), &guid)
            : ::CLSIDFromProgID(text.c_str(), &guid);
        if (SUCCEEDED(hr) && a1) uc_mem_write(uc_, a1, &guid, sizeof(guid));
        ret = uint32_t(hr);
    } else if (name == "StringFromGUID2") {
        GUID guid{};
        wchar_t text[64]{};
        if (!readGuid(a0, guid) || !a1 || !a2) {
            ret = 0;
        } else {
            const int written = ::StringFromGUID2(guid, text, int(a2));
            writeUtf16(a1, asciiFromWide(text), a2);
            ret = uint32_t(written);
        }
    } else if (name == "StringFromCLSID" || name == "StringFromIID" || name == "ProgIDFromCLSID") {
        GUID guid{};
        LPOLESTR hostText = nullptr;
        HRESULT hr = E_INVALIDARG;
        if (readGuid(a0, guid) && a1) {
            if (name == "ProgIDFromCLSID") hr = ::ProgIDFromCLSID(guid, &hostText);
            else if (name == "StringFromIID") hr = ::StringFromIID(guid, &hostText);
            else hr = ::StringFromCLSID(guid, &hostText);
        }
        if (SUCCEEDED(hr) && hostText) {
            const std::string text = asciiFromWide(hostText);
            const uint32_t guestText = allocate(uint32_t((text.size() + 1) * 2), true);
            writeUtf16(guestText, text, uint32_t(text.size() + 1));
            writeU32(a1, guestText);
            ::CoTaskMemFree(hostText);
        }
        ret = uint32_t(hr);
    } else if (name == "CoCreateInstance") {
        const uint32_t outPtr = stackArg(4);
        if (outPtr) writeU32(outPtr, 0);
        GUID clsid{};
        GUID iid{};
        IUnknown* out = nullptr;
        HRESULT hr = E_INVALIDARG;
        if (!outPtr) {
            hr = E_POINTER;
        } else if (a1) {
            hr = CLASS_E_NOAGGREGATION;
        } else if (readGuid(a0, clsid) && readGuid(a3, iid)) {
            const DWORD context = a2 ? a2 : CLSCTX_INPROC_SERVER;
            hr = ::CoCreateInstance(clsid, nullptr, context, iid, reinterpret_cast<void**>(&out));
            if (SUCCEEDED(hr) && out) writeU32(outPtr, makeGuestComProxy(reinterpret_cast<uintptr_t>(out)));
        }
        ret = uint32_t(hr);
    } else if (name == "OleCreate") {
        if (stackArg(6)) writeU32(stackArg(6), 0);
        ret = 0x80004001u;
    } else if (name == "OleRun" || name == "OleSave" || name == "OleSetMenuDescriptor" ||
               name == "OleDraw" || name == "OleSetContainedObject" ||
               name == "CreateOleAdviseHolder" || name == "ReadClassStm" || name == "WriteClassStm") {
        ret = 0x80004001u;
    } else if (name == "OleIsRunning") {
        ret = 0;
    } else {
        return false;
    }
    return true;
}
