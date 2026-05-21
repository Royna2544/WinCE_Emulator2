#pragma once

#include <unicorn/unicorn.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct SyntheticModule {
    std::string moduleName;
    uint32_t imageBase{};
    uint32_t imageSize{};
    std::map<std::string, uint32_t> exportsByName;
    std::map<uint16_t, uint32_t> exportsByOrdinal;
    std::map<uint16_t, std::string> exportNamesByOrdinal;
};

class SyntheticDllRuntime {
public:
    explicit SyntheticDllRuntime(uc_engine* uc);

    void setMainModulePath(std::string path);
    void setFramebuffer(uint32_t* bgra, int width, int height);
    void setRegistryPath(const std::filesystem::path& path);
    void flushRegistry();
    bool hasHostWindows() const;
    void runHostMessageLoopUntilClosed();
    std::optional<SyntheticModule> createModule(const std::string& dllName);
    static void hookCode(uc_engine* uc, uint64_t address, uint32_t size, void* user);

private:
    struct ExportEntry {
        std::string moduleName;
        std::string name;
        uint16_t ordinal{};
        uint64_t calls{};
    };
    struct GuestCallArgs {
        uint32_t a0{};
        uint32_t a1{};
        uint32_t a2{};
        uint32_t a3{};
        uint32_t ra{};
    };
    struct GuestHandle {
        enum class Kind {
            HostFile,
            HostFind,
            HostWaveIn,
            HostEvent,
            HostMutex,
            HostMenu,
            HostAccelerator,
            HostIcon,
            HostCursor,
            HostBitmap,
            HostSocket,
            HostComInterface,
            GuestFileMapping,
            GuestPropertySheetPage,
            GuestHeap,
            GuestResource,
            GuestRegistryKey,
            GuestWindow,
            GuestDc,
            GuestBrush,
            GuestPen,
            GuestFont,
        };

        Kind kind{Kind::GuestHeap};
        uintptr_t hostValue{};
        uint32_t filePointer{};
    };
    struct GuestWindowClass {
        std::array<uint8_t, 40> bytes{};
        std::string name;
        uint16_t atom{};
    };
    struct GuestWindow {
        uint32_t hwnd{};
        std::string className;
        std::string title;
        uint32_t style{};
        uint32_t exStyle{};
        uint32_t parent{};
        uint32_t menu{};
        uint32_t instance{};
        uint32_t param{};
        uint32_t wndProc{};
        uint32_t userData{};
        int32_t x{};
        int32_t y{};
        int32_t width{800};
        int32_t height{480};
        uintptr_t hostHwnd{};
        bool visible{};
        std::map<int32_t, uint32_t> extraLongs;
    };
    struct GuestDc {
        uint32_t hwnd{};
        uint32_t selectedBrush{};
        uint32_t selectedPen{};
        uint32_t selectedFont{};
        uint32_t textColor{0x00000000};
        uint32_t bkColor{0x00ffffff};
        uint32_t bkMode{1};
        uint32_t textAlign{};
        int32_t x{};
        int32_t y{};
    };
    struct GuestBrush {
        uint32_t colorRef{};
        bool stock{};
    };
    struct GuestPen {
        uint32_t style{};
        uint32_t width{};
        uint32_t colorRef{};
        bool stock{};
    };
    struct GuestFont {
        std::array<uint8_t, 92> logFont{};
        bool stock{};
    };
    struct HostWaveBuffer {
        std::vector<uint8_t> data;
        std::array<uint8_t, 64> header{};
    };
    struct GuestMessage {
        uint32_t hwnd{};
        uint32_t message{};
        uint32_t wParam{};
        uint32_t lParam{};
        uint32_t time{};
        uint32_t x{};
        uint32_t y{};
    };
    struct ResourceName {
        bool ordinal{};
        uint32_t id{};
        std::string name;
    };
    struct ResourceEntry {
        ResourceName type;
        ResourceName name;
        uint16_t language{};
        std::vector<uint8_t> data;
    };
    struct GuestFileMapping {
        uint32_t fileHandle{};
        uint64_t size{};
        uint32_t protect{};
        std::string name;
    };
    struct GuestMappedView {
        uint32_t mappingHandle{};
        uint64_t offset{};
        uint32_t size{};
    };

    uc_engine* uc_{};
    uint32_t nextModuleBase_ = 0x70000000;
    uint32_t heapBase_ = 0x30000000;
    uint32_t heapLimit_ = 0x34000000;
    uint32_t nextHeap_ = 0x30010000;
    uint32_t lastError_ = 0;
    uint32_t nextHandle_ = 0x10000;
    uint32_t processHeapHandle_ = 0;
    uint64_t tick_ = 0;
    bool quitPosted_ = false;
    uint32_t currentCursor_ = 0;
    uint32_t comProxyVtable_ = 0;
    uint32_t comQueryInterfaceStub_ = 0;
    uint32_t comAddRefStub_ = 0;
    uint32_t comReleaseStub_ = 0;
    std::string mainModulePath_ = "\\INavi\\INavi.exe";
    std::filesystem::path hostBaseDir_;
    uint16_t nextAtom_ = 0xc000;
    std::map<uint32_t, ExportEntry> exportsByAddress_;
    std::map<uint32_t, uint32_t> allocationSizes_;
    std::map<uint32_t, uint32_t> tlsValues_;
    std::map<uint32_t, uint32_t> criticalSectionDepth_;
    std::map<uint32_t, uint32_t> syntheticHandleValues_;
    std::map<uint32_t, GuestHandle> guestHandles_;
    std::map<std::string, GuestWindowClass> windowClassesByName_;
    std::map<uint16_t, std::string> windowClassNamesByAtom_;
    std::map<uint32_t, GuestWindow> windows_;
    std::map<uint32_t, GuestDc> dcs_;
    std::map<uint32_t, GuestBrush> brushes_;
    std::map<uint32_t, GuestPen> pens_;
    std::map<uint32_t, GuestFont> fonts_;
    std::map<int32_t, uint32_t> stockObjects_;
    std::map<uint32_t, HostWaveBuffer> hostWaveBuffers_;
    std::map<uint32_t, std::string> registryHandles_;
    std::map<uint32_t, std::string> fileHandleDebugNames_;
    std::map<uint32_t, uint32_t> fileReadCounts_;
    std::map<uint32_t, uint32_t> fileSeekCounts_;
    std::map<uint32_t, GuestFileMapping> fileMappings_;
    std::map<uint32_t, GuestMappedView> mappedViews_;
    std::deque<GuestMessage> guestMessages_;
    std::vector<uintptr_t> retainedHostWindows_;
    std::vector<ResourceEntry> mainResources_;
    std::map<uint32_t, uint32_t> loadedResourceMemory_;
    std::map<std::string, uint16_t> atomsByName_;
    std::map<uint16_t, std::string> atomNames_;
    uint32_t* framebuffer_{};
    int32_t framebufferWidth_{};
    int32_t framebufferHeight_{};
    std::filesystem::path registryPath_;
    nlohmann::json registry_;
    bool registryDirty_{};

    std::optional<SyntheticModule> createCoredll();
    std::optional<SyntheticModule> createCommctrl();
    std::optional<SyntheticModule> createGenericOrdinalDll(const std::string& moduleName, uint16_t maxOrdinal);
    void registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name);
    void writeStub(uint32_t address);
    void dispatch(const ExportEntry& entry);
    bool dispatchHostWin32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchGuestMemoryApi(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchCommctrl(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchWinsock(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchOle32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchOleAut32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    uint32_t handleWNetGetUserW(uint32_t providerName, uint32_t userName, uint32_t lengthPtr);
    uint32_t handleWaveInGetID(uint32_t waveInHandle, uint32_t deviceIdPtr);
    uint32_t handleWaveInBuffer(const std::string& name, uint32_t waveInHandle, uint32_t headerPtr);
    uint32_t handleSystemParametersInfoW(uint32_t action, uint32_t uiParam, uint32_t pvParam, uint32_t flags);
    uint32_t handleLoadCursorW(uint32_t instance, uint32_t cursorName);
    uint32_t handleGetSysColorBrush(uint32_t colorIndex);
    uint32_t handleGetDeviceCaps(uint32_t dc, uint32_t index);
    uint32_t handleWideCharToMultiByte(uint32_t codePage, uint32_t flags, uint32_t widePtr, uint32_t wideChars);
    uint32_t handleCreateFileMappingW(uint32_t fileHandle, uint32_t security, uint32_t protect, uint32_t sizeHigh);
    uint32_t handleMapViewOfFile(uint32_t mappingHandle, uint32_t desiredAccess, uint32_t offsetHigh, uint32_t offsetLow);
    uint32_t handleUnmapViewOfFile(uint32_t baseAddress);
    uint32_t handleFlushViewOfFile(uint32_t baseAddress, uint32_t bytesToFlush);

    uint32_t makeGuestHandle(GuestHandle handle);
    GuestHandle* lookupGuestHandle(uint32_t guestHandle);
    uint32_t closeGuestHandle(uint32_t guestHandle);
    uint32_t makeGuestDc(uint32_t hwnd);
    GuestDc* lookupGuestDc(uint32_t hdc);
    uint32_t makeGuestBrush(uint32_t colorRef, bool stock = false);
    uint32_t makeGuestPen(uint32_t style, uint32_t width, uint32_t colorRef, bool stock = false);
    uint32_t makeGuestFont(const std::array<uint8_t, 92>& logFont, bool stock = false);
    uint32_t makeGuestComProxy(uintptr_t hostInterface);
    uint32_t makeStockObject(int32_t index);
    uint32_t makeGuestWindow(const std::string& className, const std::string& title, uint32_t style,
                             uint32_t exStyle, uint32_t parent, uint32_t menu, uint32_t instance,
                             uint32_t param, int32_t x, int32_t y, int32_t width, int32_t height,
                             bool visible, uint32_t wndProc = 0);
    uint32_t loadMenuResourceHandle(uint32_t nameArg);
    void ensureHostWindow(uint32_t guestHwnd, GuestWindow& window);
    void destroyHostWindow(GuestWindow& window);
    void invalidateHostWindows();
    void pumpHostMessages();
    uint32_t colorRefToPixel(uint32_t colorRef) const;
    bool readGuestRect(uint32_t address, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom) const;
    void writeGuestRect(uint32_t address, int32_t left, int32_t top, int32_t right, int32_t bottom) const;
    void fillFramebufferRect(const GuestDc& dc, int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t pixel);
    void drawFramebufferLine(const GuestDc& dc, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel);
    bool stretchDibToFramebuffer(const GuestDc& dc, int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                                 int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                 uint32_t bitsPtr, uint32_t infoPtr);
    std::optional<std::string> registryPathFromHandle(uint32_t hkey, const std::string& subKey) const;
    bool registryKeyExists(const std::string& path) const;
    void registryEnsureKey(const std::string& path);
    uint32_t makeRegistryHandle(const std::string& path);
    std::vector<std::string> registryChildNames(const std::string& path) const;
    nlohmann::json* registryValue(const std::string& path, const std::string& valueName);
    const nlohmann::json* registryValue(const std::string& path, const std::string& valueName) const;
    std::vector<uint8_t> registryValueBytes(const nlohmann::json& value) const;
    nlohmann::json registryJsonFromBytes(uint32_t type, uint32_t dataPtr, uint32_t dataSize) const;
    void loadMainResources(const std::filesystem::path& path);
    const ResourceEntry* findResource(uint32_t typeArg, uint32_t nameArg) const;
    const ResourceEntry* resourceFromHandle(uint32_t guestHandle) const;
    bool resourceNameMatches(const ResourceName& resourceName, uint32_t guestArg) const;
    bool writeGuestMessage(uint32_t address, const GuestMessage& message) const;
    uint32_t reg(int regId) const;
    void setReg(int regId, uint32_t value) const;
    uint32_t stackArg(uint32_t index) const;
    uint32_t allocate(uint32_t size, bool zeroFill);
    uint32_t readU32(uint32_t address) const;
    void writeU32(uint32_t address, uint32_t value) const;
    bool isGuestRangeReadable(uint32_t address, uint32_t size) const;
    bool copyGuest(uint32_t dst, uint32_t src, uint32_t size) const;
    bool fillGuest(uint32_t dst, uint8_t value, uint32_t size) const;
    std::string readAscii(uint32_t address, size_t maxChars = 512) const;
    void writeAscii(uint32_t address, const std::string& value) const;
    std::string readUtf16(uint32_t address, size_t maxChars = 512) const;
    uint32_t writeUtf16(uint32_t address, const std::string& value, uint32_t maxChars) const;
    std::filesystem::path resolveGuestPath(const std::string& guestPath) const;
};
