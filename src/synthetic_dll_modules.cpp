#include "synthetic_dll.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>

namespace {

std::string lowerAscii(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool sameModule(std::string name, std::string_view wanted) {
    name = lowerAscii(std::move(name));
    if (name == wanted) return true;
    return name.size() > wanted.size() &&
           name.compare(name.size() - wanted.size(), wanted.size(), wanted) == 0;
}

}

std::optional<SyntheticModule> SyntheticDllRuntime::createModule(const std::string& dllName) {
    if (sameModule(dllName, "coredll.dll")) return createCoredll();
    if (sameModule(dllName, "commctrl.dll")) return createCommctrl();
    if (sameModule(dllName, "winsock.dll") || sameModule(dllName, "ws2.dll")) return createWinsock(dllName);
    if (sameModule(dllName, "ole32.dll")) return createOle32();
    if (sameModule(dllName, "oleaut32.dll")) return createOleAut32();
    return std::nullopt;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createCoredll() {
    SyntheticModule module;
    module.moduleName = "coredll.dll";
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00020000;
    nextModuleBase_ += 0x00020000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic COREDLL.dll");
    }

    for (uint16_t ordinal = 1; ordinal <= 2500; ++ordinal) {
        registerExport(module, ordinal, {});
    }

    // Ordinals and names are from the Windows CE 4.2 Standard SDK MIPSII
    // coredll.lib COFF import-object headers. Keep these SDK ordinals
    // authoritative; runtime surprises belong in TODO.md until verified.
    registerCoredllSyncExports(module);
    registerExport(module, 0x0006, "ExitThread");
    registerCoredllTimeExports(module);
    registerExport(module, 0x002C, "GetAPIAddress");
    registerCoredllMemoryExports(module);
    registerExport(module, 0x0059, "SystemParametersInfoW");
    registerExport(module, 0x0058, "GlobalMemoryStatus");
    registerExport(module, 0x005A, "CreateDIBSection");
    registerExport(module, 0x005F, "RegisterClassW");
    registerCoredllRectExports(module);
    registerCoredllCommExports(module);
    registerCoredllFsExports(module);
    registerExport(module, 0x00B3, "DeviceIoControl");
    registerExport(module, 0x00C0, "IsDBCSLeadByteEx");
    registerExport(module, 0x00C1, "iswctype");
    registerExport(module, 0x00C4, "MultiByteToWideChar");
    registerExport(module, 0x00C5, "WideCharToMultiByte");
    registerExport(module, 0x00DD, "CharLowerW");
    registerExport(module, 0x00E0, "CharUpperW");
    registerExport(module, 0x00EA, "FormatMessageW");
    registerExport(module, 0x00F6, "CreateWindowExW");
    registerExport(module, 0x00F7, "SetWindowPos");
    registerExport(module, 0x00F8, "GetWindowRect");
    registerExport(module, 0x00F9, "GetClientRect");
    registerExport(module, 0x00FA, "InvalidateRect");
    registerExport(module, 0x00FB, "GetWindow");
    registerExport(module, 0x00FE, "ClientToScreen");
    registerExport(module, 0x00FF, "ScreenToClient");
    registerExport(module, 0x0102, "SetWindowLongW");
    registerExport(module, 0x0103, "GetWindowLongW");
    registerCoredllPaintExports(module);
    registerCoredllGuiExports(module);
    registerExport(module, 0x0108, "DefWindowProcW");
    registerExport(module, 0x0109, "DestroyWindow");
    registerExport(module, 0x010A, "ShowWindow");
    registerExport(module, 0x010B, "UpdateWindow");
    registerExport(module, 0x010D, "GetParent");
    registerExport(module, 0x0110, "MoveWindow");
    registerCoredllWindowExports(module);
    registerExport(module, 0x011D, "CallWindowProcW");
    registerExport(module, 0x011E, "FindWindowW");
    registerExport(module, 0x0143, "GetStoreInformation");
    registerCoredllAudioExports(module);
    registerCoredllRegistryExports(module);
    registerExport(module, 0x01BE, "WNetConnectionDialog1W");
    registerExport(module, 0x01C2, "WNetGetUniversalNameW");
    registerExport(module, 0x01C3, "WNetGetUserW");
    registerCoredllThreadExports(module);
    registerExport(module, 0x01ED, "CreateProcessW");
    registerExport(module, 0x01F1, "WaitForSingleObject");
    registerExport(module, 0x0210, "LoadLibraryW");
    registerExport(module, 0x0212, "GetProcAddressW");
    registerCoredllResExports(module);
    registerExport(module, 0x021E, "GetSystemInfo");
    registerExport(module, 0x0219, "GetModuleFileNameW");
    registerExport(module, 0x021D, "OutputDebugStringW");
    registerExport(module, 0x0224, "CreateFileMappingW");
    registerExport(module, 0x0225, "MapViewOfFile");
    registerExport(module, 0x0226, "UnmapViewOfFile");
    registerExport(module, 0x0227, "FlushViewOfFile");
    registerExport(module, 0x0228, "CreateFileForMapping");
    registerExport(module, 0x0229, "CloseHandle");
    registerExport(module, 0x022D, "KernelIoControl");
    registerCoredllSystemExports(module);
    registerExport(module, 0x02BA, "IsDialogMessageW");
    registerExport(module, 0x02CD, "GetVersionExW");
    registerExport(module, 0x036B, "SetTimer");
    registerExport(module, 0x036C, "KillTimer");
    registerExport(module, 0x036E, "GetClassInfoW");
    registerExport(module, 0x035B, "DispatchMessageW");
    registerExport(module, 0x035D, "GetMessageW");
    registerExport(module, 0x035E, "GetMessagePos");
    registerExport(module, 0x035F, "GetMessageWNoWait");
    registerExport(module, 0x0360, "PeekMessageW");
    registerExport(module, 0x0361, "PostMessageW");
    registerExport(module, 0x0362, "PostQuitMessage");
    registerExport(module, 0x0364, "SendMessageW");
    registerExport(module, 0x0366, "TranslateMessage");
    registerExport(module, 0x0394, "GetDeviceCaps");
    registerExport(module, 0x037F, "CreateFontIndirectW");
    registerExport(module, 0x0380, "ExtTextOutW");
    registerExport(module, 0x0385, "CreateBitmap");
    registerExport(module, 0x0387, "BitBlt");
    registerExport(module, 0x038A, "TransparentImage");
    registerExport(module, 0x038E, "CreateCompatibleDC");
    registerExport(module, 0x0386, "CreateCompatibleBitmap");
    registerExport(module, 0x0389, "StretchBlt");
    registerExport(module, 0x0390, "DeleteObject");
    registerExport(module, 0x038F, "DeleteDC");
    registerExport(module, 0x0397, "GetStockObject");
    registerExport(module, 0x0396, "GetObjectW");
    registerExport(module, 0x0399, "SelectObject");
    registerExport(module, 0x039A, "SetBkColor");
    registerExport(module, 0x039B, "SetBkMode");
    registerExport(module, 0x039C, "SetTextColor");
    registerExport(module, 0x039D, "CreatePatternBrush");
    registerExport(module, 0x039E, "CreatePen");
    registerExport(module, 0x03A2, "CreatePenIndirect");
    registerExport(module, 0x03A3, "CreateSolidBrush");
    registerExport(module, 0x03A6, "Ellipse");
    registerExport(module, 0x03A7, "FillRect");
    registerExport(module, 0x03AA, "PatBlt");
    registerExport(module, 0x03AB, "Polygon");
    registerExport(module, 0x03AC, "Polyline");
    registerExport(module, 0x03AD, "Rectangle");
    registerExport(module, 0x03AF, "SetBrushOrgEx");
    registerExport(module, 0x03B1, "DrawTextW");
    registerExport(module, 0x03D4, "CreateRectRgn");
    registerExport(module, 0x03C8, "CombineRgn");
    registerExport(module, 0x03CB, "GetClipBox");
    registerExport(module, 0x037C, "RegisterTaskBar");
    registerCoredllMathExports(module);
    registerCoredllCrtExports(module);
    registerExport(module, 0x0499, "GetModuleHandleW");
    registerExport(module, 0x04CE, "GetProcAddressA");
    registerExport(module, 0x05D1, "KernelLibIoControl");
    registerExport(module, 0x05E3, "RegisterDesktop");
    registerExport(module, 0x05EF, "GlobalAddAtomW");
    registerExport(module, 0x05F0, "GlobalDeleteAtom");
    registerExport(module, 0x05F1, "GlobalFindAtomW");
    registerExport(module, 0x0576, "SetWindowRgn");
    registerExport(module, 0x0577, "GetWindowRgn");
    registerExport(module, 0x0673, "MoveToEx");
    registerExport(module, 0x0674, "LineTo");
    registerExport(module, 0x0676, "SetTextAlign");
    registerExport(module, 0x0682, "SetDIBColorTable");
    registerExport(module, 0x0683, "StretchDIBits");
    registerExport(module, 0x06BD, "SetBitmapBits");
    registerExport(module, 0x06BE, "SetDIBitsToDevice");

    destroyWindowContinuationStub_ = module.imageBase + 0x0001f000;
    auto& destroyContinuation = exportsByAddress_[destroyWindowContinuationStub_];
    destroyContinuation.moduleName = module.moduleName;
    destroyContinuation.moduleKind = SyntheticModuleKind::Coredll;
    destroyContinuation.name = "__DestroyWindowContinue";
    writeStub(destroyWindowContinuationStub_);
    createWindowContinuationStub_ = module.imageBase + 0x0001f008;
    auto& createContinuation = exportsByAddress_[createWindowContinuationStub_];
    createContinuation.moduleName = module.moduleName;
    createContinuation.moduleKind = SyntheticModuleKind::Coredll;
    createContinuation.name = "__CreateWindowContinue";
    writeStub(createWindowContinuationStub_);
    blockingApiContinuationStub_ = module.imageBase + 0x0001f010;
    auto& blockingContinuation = exportsByAddress_[blockingApiContinuationStub_];
    blockingContinuation.moduleName = module.moduleName;
    blockingContinuation.moduleKind = SyntheticModuleKind::Coredll;
    blockingContinuation.name = "__BlockingApiContinue";
    writeStub(blockingApiContinuationStub_);
    updateWindowContinuationStub_ = module.imageBase + 0x0001f018;
    auto& updateWindowContinuation = exportsByAddress_[updateWindowContinuationStub_];
    updateWindowContinuation.moduleName = module.moduleName;
    updateWindowContinuation.moduleKind = SyntheticModuleKind::Coredll;
    updateWindowContinuation.name = "__UpdateWindowContinue";
    writeStub(updateWindowContinuationStub_);
    threadExitStub_ = module.imageBase + 0x0001f020;
    auto& threadExit = exportsByAddress_[threadExitStub_];
    threadExit.moduleName = module.moduleName;
    threadExit.moduleKind = SyntheticModuleKind::Coredll;
    threadExit.name = "__ThreadExit";
    writeStub(threadExitStub_);
    messageTransferContinuationStub_ = module.imageBase + 0x0001f028;
    auto& messageTransfer = exportsByAddress_[messageTransferContinuationStub_];
    messageTransfer.moduleName = module.moduleName;
    messageTransfer.moduleKind = SyntheticModuleKind::Coredll;
    messageTransfer.name = "__MessageTransferContinue";
    writeStub(messageTransferContinuationStub_);
    spdlog::info("mapped synthetic COREDLL.dll base=0x{:08x} ordinals={}",
                 module.imageBase, module.exportsByOrdinal.size());
    return module;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createCommctrl() {
    SyntheticModule module;
    module.moduleName = "commctrl.dll";
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00010000;
    nextModuleBase_ += 0x00010000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic commctrl.dll");
    }
    for (uint16_t ordinal = 1; ordinal <= 128; ++ordinal) {
        registerExport(module, ordinal, {});
    }

    registerCommctrlExports(module);

    spdlog::info("mapped synthetic commctrl.dll base=0x{:08x} ordinals={}",
                 module.imageBase, module.exportsByOrdinal.size());
    return module;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createGenericOrdinalDll(
    const std::string& moduleName, uint16_t maxOrdinal) {
    SyntheticModule module;
    module.moduleName = moduleName;
    module.imageBase = nextModuleBase_;
    module.imageSize = 0x00010000;
    nextModuleBase_ += 0x00010000;

    if (uc_mem_map(uc_, module.imageBase, module.imageSize, UC_PROT_ALL) != UC_ERR_OK) {
        throw std::runtime_error("cannot map synthetic " + moduleName);
    }
    for (uint16_t ordinal = 1; ordinal <= maxOrdinal; ++ordinal) {
        registerExport(module, ordinal, {});
    }
    spdlog::info("mapped synthetic {} base=0x{:08x} ordinals={}",
                 moduleName, module.imageBase, module.exportsByOrdinal.size());
    return module;
}

std::optional<SyntheticModule> SyntheticDllRuntime::createGenericOrdinalDll(const SyntheticDllSpec& spec) {
    auto module = createGenericOrdinalDll(spec.name ? spec.name : "", spec.maxOrdinal);
    if (!module) return module;
    registerHandlers(*module, spec.handlers);
    return module;
}

void SyntheticDllRuntime::registerHandlers(SyntheticModule& module,
                                           const OrdinalHandlerGroup& group) {
    registerHandlers(module, group.handlers);
}

void SyntheticDllRuntime::registerHandlers(SyntheticModule& module,
                                           const OrdinalHandlerMap& handlers) {
    auto& dll = registeredDllsByName_[lowerAscii(module.moduleName)];
    if (dll.name.empty()) dll.name = module.moduleName;
    for (const auto& [ordinal, handler] : handlers) {
        dll.handlers[ordinal] = handler;
        registerExport(module, ordinal, handler.name ? handler.name : "", handler.code, handler.handler);
    }
}

void SyntheticDllRuntime::registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name,
                                         SyntheticExportCode code,
                                         OrdinalHandlerFunction ordinalHandler) {
    const uint32_t rva = 0x1000 + uint32_t(ordinal) * 8;
    module.exportsByOrdinal[ordinal] = rva;
    if (!name.empty()) {
        module.exportNamesByOrdinal[ordinal] = name;
        module.exportsByName[lowerAscii(name)] = rva;
    }
    const uint32_t address = module.imageBase + rva;
    auto& entry = exportsByAddress_[address];
    entry.moduleName = module.moduleName;
    entry.moduleKind = moduleKindForName(module.moduleName);
    entry.code = code;
    entry.ordinalHandler = ordinalHandler;
    entry.ordinal = ordinal;
    if (!name.empty()) entry.name = name;
    writeStub(address);
}

const SyntheticDllRuntime::OrdinalHandlerSpec*
SyntheticDllRuntime::findOrdinalHandler(const ExportEntry& entry) const {
    const auto dllIt = registeredDllsByName_.find(lowerAscii(entry.moduleName));
    if (dllIt == registeredDllsByName_.end()) return nullptr;
    const auto handlerIt = dllIt->second.handlers.find(entry.ordinal);
    if (handlerIt == dllIt->second.handlers.end()) return nullptr;
    return &handlerIt->second;
}

SyntheticDllRuntime::SyntheticModuleKind
SyntheticDllRuntime::moduleKindForName(const std::string& moduleName) const {
    const std::string name = lowerAscii(moduleName);
    if (name == "coredll.dll") return SyntheticModuleKind::Coredll;
    if (name == "commctrl.dll") return SyntheticModuleKind::Commctrl;
    if (name == "winsock.dll" || name == "ws2.dll") return SyntheticModuleKind::Winsock;
    if (name == "ole32.dll") return SyntheticModuleKind::Ole32;
    if (name == "oleaut32.dll") return SyntheticModuleKind::OleAut32;
    return SyntheticModuleKind::Unknown;
}

void SyntheticDllRuntime::writeStub(uint32_t address) {
    const std::array<uint8_t, 8> stub = {
        0x08, 0x00, 0xe0, 0x03, // jr ra
        0x00, 0x00, 0x00, 0x00, // nop
    };
    uc_mem_write(uc_, address, stub.data(), stub.size());
}

