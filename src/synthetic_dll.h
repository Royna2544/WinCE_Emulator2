#pragma once

#include <unicorn/unicorn.h>

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    struct GuestProcessLaunch {
        std::filesystem::path hostApplication;
        std::string guestApplication;
        std::string commandLine;
        uint32_t processHandle{};
        uint32_t threadHandle{};
        uint32_t processId{};
        uint32_t threadId{};
    };
    struct CachedFileAttributes {
        bool ok{};
        uint32_t error{};
        uint32_t attributes{};
        uint32_t creationLow{};
        uint32_t creationHigh{};
        uint32_t accessLow{};
        uint32_t accessHigh{};
        uint32_t writeLow{};
        uint32_t writeHigh{};
        uint32_t sizeHigh{};
        uint32_t sizeLow{};
    };
    using GuestProcessLauncher = std::function<bool(GuestProcessLaunch&)>;

    explicit SyntheticDllRuntime(uc_engine* uc);

    void setMainModulePath(std::string path);
    void setMainModuleBase(uint32_t base);
    void setFramebuffer(uint32_t* bgra, int width, int height);
    void setRegistryPath(const std::filesystem::path& path);
    void setSdmmcHostPath(const std::filesystem::path& path);
    void setSerialDeviceMapPath(const std::filesystem::path& path);
    void registerLoadedModule(const std::string& moduleName, const std::filesystem::path& path, uint32_t base,
                              uint32_t imageSize,
                              const std::map<std::string, uint32_t>& exportsByName = {},
                              const std::map<uint16_t, uint32_t>& exportsByOrdinal = {});
    void setGuestProcessLauncher(GuestProcessLauncher launcher);
    bool startGuestProcessImage(const std::string& guestApplication,
                                const std::filesystem::path& hostApplication,
                                uint32_t moduleBase,
                                uint32_t entryPoint,
                                const std::string& commandLine,
                                uint32_t& processHandle,
                                uint32_t& threadHandle,
                                uint32_t& processId,
                                uint32_t& threadId);
    void flushRegistry();
    bool hasHostWindows() const;
    void runHostMessageLoopUntilClosed(bool showHostWindows = true);
    uint32_t threadExitStubAddress() const;
    void queueHostMouseMessage(uint32_t rootGuestHwnd, uint32_t message, int32_t hostX, int32_t hostY);
    std::optional<SyntheticModule> createModule(const std::string& dllName);
    static void hookCode(uc_engine* uc, uint64_t address, uint32_t size, void* user);
    static void hookBasicBlock(uc_engine* uc, uint64_t address, uint32_t size, void* user);

private:
    enum class SyntheticModuleKind : uint8_t {
        Unknown,
        Coredll,
        Commctrl,
        Winsock,
        Ole32,
        OleAut32,
    };
    enum class SyntheticExportCode : uint16_t {
        Unknown,
        CommctrlInitCommonControls,
        CommctrlInitCommonControlsEx,
        CommctrlCommandBarCreate,
        CommctrlCommandBarShow,
        CommctrlCommandBarAddBitmap,
        CommctrlCommandBarInsertComboBox,
        CommctrlCommandBarInsertControl,
        CommctrlCommandBarInsertMenubar,
        CommctrlCommandBarGetMenu,
        CommctrlCommandBarAddAdornments,
        CommctrlCommandBarGetItemWindow,
        CommctrlCommandBarHeight,
        CommctrlIsCommandBarMessage,
        CommctrlCreateUpDownControl,
        CommctrlCreateToolbar,
        CommctrlCreateToolbarEx,
        CommctrlCreateStatusWindowW,
        CommctrlPropertySheetW,
        CommctrlCreatePropertySheetPageW,
        CommctrlDestroyPropertySheetPage,
        CommctrlDrawStatusTextW,
        CommctrlInvertRect,
        CommctrlCommandBarInsertMenubarEx,
        CommctrlCommandBarDrawMenuBar,
        CommctrlCommandBarAlignAdornments,
        CommctrlInitClass,
        CommctrlListViewSetItemSpacing,
        WinsockCleanup,
        WinsockStartup,
        WinsockFdIsSet,
        WinsockAccept,
        WinsockBind,
        WinsockCloseSocket,
        WinsockConnect,
        WinsockGetHostByName,
        WinsockGetHostName,
        WinsockHtonl,
        WinsockHtons,
        WinsockInetAddr,
        WinsockInetNtoa,
        WinsockIoctlSocket,
        WinsockListen,
        WinsockNtohl,
        WinsockNtohs,
        WinsockRecv,
        WinsockSelect,
        WinsockSend,
        WinsockSetSockOpt,
        WinsockSocket,
        CoreDllWaveOutGetNumDevs,
        CoreDllWaveOutSetVolume,
        CoreDllWaveOutClose,
        CoreDllWaveOutPrepareHeader,
        CoreDllWaveOutUnprepareHeader,
        CoreDllWaveOutWrite,
        CoreDllWaveOutReset,
        CoreDllWaveOutOpen,
        CoreDllWaveInClose,
        CoreDllWaveInUnprepareHeader,
        CoreDllWaveInAddBuffer,
        CoreDllWaveInReset,
        CoreDllWaveInGetID,
        CoreDllWaveInMessage,
        CoreDllWaveInOpen,
        CoreDllMixerGetControlDetails,
        CoreDllRegCreateKeyExW,
        CoreDllRegCloseKey,
        CoreDllRegDeleteKeyW,
        CoreDllRegDeleteValueW,
        CoreDllRegEnumValueW,
        CoreDllRegEnumKeyExW,
        CoreDllRegOpenKeyExW,
        CoreDllRegQueryInfoKeyW,
        CoreDllRegQueryValueExW,
        CoreDllRegSetValueExW,
        CoreDllRegFlushKey,
        CoreDllSystemTimeToFileTime,
        CoreDllFileTimeToSystemTime,
        CoreDllGetLocalTime,
        CoreDllGetSystemTime,
        CoreDllSetSystemTime,
        CoreDllGetTimeZoneInformation,
        CoreDllSleep,
        CoreDllGetTickCount,
        CoreDllQueryPerformanceCounter,
        CoreDllQueryPerformanceFrequency,
        CoreDllGetProcessIndexFromID,
        CoreDllGetACP,
        CoreDllIsValidLocale,
        CoreDllGetLastError,
        CoreDllSetLastError,
        CoreDllIsBadReadPtr,
        CoreDllIsBadWritePtr,
        CoreDllIsProcessDying,
        CoreDllLocalAlloc,
        CoreDllLocalReAlloc,
        CoreDllLocalSize,
        CoreDllLocalFree,
        CoreDllRemoteLocalReAlloc,
        CoreDllHeapCreate,
        CoreDllHeapDestroy,
        CoreDllHeapAlloc,
        CoreDllHeapReAlloc,
        CoreDllHeapSize,
        CoreDllHeapFree,
        CoreDllGetProcessHeap,
        CoreDllVirtualAlloc,
        CoreDllVirtualFree,
        CoreDllLocalAllocTrace,
        CoreDllFree,
        CoreDllMalloc,
        CoreDllRealloc,
        CoreDllMsize,
        CoreDllOperatorDelete,
        CoreDllOperatorNew,
        CoreDllOperatorVectorNew,
        CoreDllOperatorVectorDelete,
        CoreDllOperatorNewNoThrow,
        CoreDllOperatorVectorNewNoThrow,
        CoreDllOperatorDeleteNoThrow,
        CoreDllOperatorVectorDeleteNoThrow,
        CoreDllRemoteHeapFree,
        CoreDllInitializeCriticalSection,
        CoreDllDeleteCriticalSection,
        CoreDllEnterCriticalSection,
        CoreDllLeaveCriticalSection,
        CoreDllTryEnterCriticalSection,
        CoreDllTlsGetValue,
        CoreDllTlsSetValue,
        CoreDllTlsCall,
        CoreDllInterlockedTestExchange,
        CoreDllInterlockedIncrement,
        CoreDllInterlockedDecrement,
        CoreDllInterlockedExchange,
        CoreDllInterlockedExchangeAdd,
        CoreDllInterlockedCompareExchange,
        CoreDllClearCommError,
        CoreDllGetCommState,
        CoreDllPurgeComm,
        CoreDllSetCommMask,
        CoreDllSetCommState,
        CoreDllSetCommTimeouts,
        CoreDllSetupComm,
        CoreDllCopyRect,
        CoreDllEqualRect,
        CoreDllInflateRect,
        CoreDllIsRectEmpty,
        CoreDllPtInRect,
        CoreDllSetRect,
        CoreDllSetRectEmpty,
        CoreDllCreateThread,
        CoreDllCreateEventW,
        CoreDllEventModify,
        CoreDllWaitForMultipleObjects,
        CoreDllResumeThread,
        CoreDllSetThreadPriority,
        CoreDllGetThreadPriority,
        CoreDllCreateMutexW,
        CoreDllReleaseMutex,
        CoreDllAtoi,
        CoreDllAtof,
        CoreDllAtan,
        CoreDllCeil,
        CoreDllCos,
        CoreDllDifftime,
        CoreDllFabs,
        CoreDllFloor,
        CoreDllHypot,
        CoreDllPow,
        CoreDllRand,
        CoreDllSqrt,
        CoreDllSrand,
        CoreDllSin,
        CoreDllStrtol,
        CoreDllStrtoul,
        CoreDllWcstoul,
        CoreDllFmodf,
        CoreDllLlLshift,
        CoreDllLlDiv,
        CoreDllFloatToLongLong,
        CoreDllLongLongToDouble,
        CoreDllUnsignedLongLongToDouble,
        CoreDllFloatAdd,
        CoreDllDoubleAdd,
        CoreDllFloatSub,
        CoreDllDoubleSub,
        CoreDllFloatMul,
        CoreDllDoubleMul,
        CoreDllFloatDiv,
        CoreDllDoubleDiv,
        CoreDllFloatToLong,
        CoreDllFloatToUnsignedLong,
        CoreDllDoubleToLong,
        CoreDllDoubleToUnsignedLong,
        CoreDllLongToDouble,
        CoreDllUnsignedLongToDouble,
        CoreDllFloatToDouble,
        CoreDllDoubleToFloat,
        CoreDllLongToFloat,
        CoreDllUnsignedLongToFloat,
        CoreDllFloatLessThan,
        CoreDllFloatLessEqual,
        CoreDllFloatEqual,
        CoreDllFloatGreaterEqual,
        CoreDllFloatGreaterThan,
        CoreDllFloatNotEqual,
        CoreDllDoubleLessThan,
        CoreDllDoubleLessEqual,
        CoreDllDoubleEqual,
        CoreDllDoubleGreaterEqual,
        CoreDllDoubleGreaterThan,
        CoreDllDoubleNotEqual,
        CoreDllFgetc,
        CoreDllFgets,
        CoreDllFopen,
        CoreDllFclose,
        CoreDllFread,
        CoreDllFwrite,
        CoreDllFflush,
        CoreDllFeof,
        CoreDllFerror,
        CoreDllFseek,
        CoreDllFtell,
        CoreDllWfopen,
        CoreDllLongjmp,
        CoreDllMemcmp,
        CoreDllMemcpy,
        CoreDllMemmove,
        CoreDllMemset,
        CoreDllStrcat,
        CoreDllStrcmp,
        CoreDllStrcpy,
        CoreDllStrcspn,
        CoreDllStrlen,
        CoreDllStrstr,
        CoreDllStrtok,
        CoreDllStricmp,
        CoreDllStrnicmp,
        CoreDllUltow,
        CoreDllToupper,
        CoreDllSnwprintf,
        CoreDllSwprintf,
        CoreDllVswprintf,
        CoreDllPrintf,
        CoreDllVsprintf,
        CoreDllVsnwprintf,
        CoreDllGetCrtStorageEx,
        CoreDllGetCrtFlags,
        CoreDllSecurityGenCookie,
        CoreDllSecurityErrorHandler,
        CoreDllSetjmp,
        CoreDllEhvecCtor,
        CoreDllSprintf,
        CoreDllSnprintf,
        CoreDllWsprintfW,
        CoreDllWcschr,
        CoreDllWcscpy,
        CoreDllWcscspn,
        CoreDllWcslen,
        CoreDllWcsncmp,
        CoreDllWcsrchr,
        CoreDllWcsstr,
        CoreDllWcsdup,
        CoreDllWtol,
        CoreDllWcsnicmp,
        CoreDllWcsicmp,
        CoreDllCreateDirectoryW,
        CoreDllDeleteFileW,
        CoreDllGetFileAttributesW,
        CoreDllFindFirstFileW,
        CoreDllCreateFileW,
        CoreDllCreateFileForMappingW,
        CoreDllReadFile,
        CoreDllWriteFile,
        CoreDllGetFileSize,
        CoreDllSetFilePointer,
        CoreDllFlushFileBuffers,
        CoreDllSetFileTime,
        CoreDllFindClose,
        CoreDllFindNextFileW,
        CoreDllGetFileAttributesExW,
        CoreDllSetWindowTextW,
        CoreDllGetWindowTextW,
        CoreDllGetWindowTextLengthW,
        CoreDllEnableWindow,
        CoreDllIsWindowEnabled,
        CoreDllSetFocus,
        CoreDllGetFocus,
        CoreDllGetCapture,
        CoreDllSetCapture,
        CoreDllReleaseCapture,
        CoreDllBeginPaint,
        CoreDllEndPaint,
        CoreDllGetDC,
        CoreDllGetPixel,
        CoreDllReleaseDC,
        CoreDllValidateRect,
        CoreDllGetDCEx,
        CoreDllFindResource,
        CoreDllFindResourceW,
        CoreDllLoadResource,
        CoreDllSizeofResource,
        CoreDllLoadAcceleratorsW,
        CoreDllLoadIconW,
        CoreDllLoadImageW,
        CoreDllLoadMenuW,
        CoreDllLoadStringW,
        CoreDllLoadCursorW,
        CoreDllRemoveMenu,
        CoreDllCheckMenuItem,
        CoreDllCheckMenuRadioItem,
        CoreDllSetCursor,
        CoreDllGetCursorPos,
        CoreDllGetSystemMetrics,
        CoreDllGetSysColor,
        CoreDllGetSysColorBrush,
        CoreDllRegisterWindowMessageW,
        CoreDllAdjustWindowRectEx,
        CoreDllGetDlgItem,
        CoreDllGetDlgCtrlID,
        CoreDllGetForegroundWindow,
        CoreDllSetForegroundWindow,
        CoreDllSetActiveWindow,
        CoreDllGetActiveWindow,
        CoreDllMessageBoxW,
        CoreDllTranslateAcceleratorW,
        CoreDllIsWindowVisible,
    };
    struct GuestCallArgs {
        uint32_t a0{};
        uint32_t a1{};
        uint32_t a2{};
        uint32_t a3{};
        uint32_t ra{};
    };
    using OrdinalHandlerFunction = bool (SyntheticDllRuntime::*)(SyntheticExportCode code,
                                                                 const GuestCallArgs& args,
                                                                 uint32_t& ret);
    struct ExportEntry {
        std::string moduleName;
        std::string name;
        SyntheticModuleKind moduleKind{SyntheticModuleKind::Unknown};
        SyntheticExportCode code{SyntheticExportCode::Unknown};
        uint16_t ordinal{};
        uint64_t calls{};
    };
    struct OrdinalHandlerSpec {
        const char* name{};
        SyntheticExportCode code{SyntheticExportCode::Unknown};
        OrdinalHandlerFunction handler{};
    };
    using OrdinalHandlerMap = std::unordered_map<uint16_t, OrdinalHandlerSpec>;
    struct OrdinalHandlerGroup {
        const char* name{};
        OrdinalHandlerMap handlers;
    };
    struct RegisteredSyntheticDll {
        std::string name;
        OrdinalHandlerMap handlers;
    };
    struct SyntheticDllSpec {
        const char* name{};
        uint16_t maxOrdinal{};
        OrdinalHandlerMap handlers;
    };
    struct GuestHandle {
        enum class Kind {
            HostFile,
            HostFind,
            HostCrtFile,
            HostWaveIn,
            HostWaveOut,
            HostEvent,
            HostMutex,
            HostMenu,
            HostAccelerator,
            HostIcon,
            HostCursor,
            HostBitmap,
            HostRegion,
            HostSocket,
            HostComInterface,
            HostSerialDevice,
            GuestFileMapping,
            GuestPropertySheetPage,
            GuestHeap,
            GuestResource,
            GuestRegistryKey,
            GuestProcess,
            GuestThread,
            GuestSerialDevice,
            GuestFind,
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
    struct SerialDeviceConfig {
        std::string guest;
        std::string type;
        std::string backend;
        std::string host;
        bool enabled{};
        uint32_t baud{9600};
        std::string mode{"8N1"};
        std::string note;
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
        uint32_t createStruct{};
        uint32_t ownerThread{};
        uint64_t zOrder{};
        int32_t x{};
        int32_t y{};
        int32_t width{800};
        int32_t height{480};
        uintptr_t hostHwnd{};
        bool visible{};
        bool enabled{true};
        bool destroyed{};
        bool externalProcess{};
        uint32_t externalProcessId{};
        uint32_t externalHwnd{};
        bool paintBoundsValid{};
        int32_t paintLeft{};
        int32_t paintTop{};
        int32_t paintRight{};
        int32_t paintBottom{};
        bool backingValid{};
        int32_t backingX{};
        int32_t backingY{};
        int32_t backingWidth{};
        int32_t backingHeight{};
        std::vector<uint32_t> backingPixels;
        std::map<int32_t, uint32_t> extraLongs;
    };
    struct GuestDc {
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
    };
    struct GuestBrush {
        uint32_t colorRef{};
        uint32_t patternBitmap{};
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
    struct GuestBitmap {
        int32_t width{};
        int32_t heightRaw{};
        uint16_t bpp{};
        uint32_t stride{};
        uint32_t bits{};
        uint32_t redMask{};
        uint32_t greenMask{};
        uint32_t blueMask{};
        std::vector<uint32_t> palette;
        bool stock{};
    };
    struct BitmapProbeStats {
        uint32_t sampled{};
        uint32_t nonBlack{};
        uint32_t uniqueApprox{};
        uint32_t firstPixel{};
        uint32_t lastPixel{};
    };
    struct HostWaveBuffer {
        std::vector<uint8_t> data;
        std::array<uint8_t, 64> header{};
        std::shared_ptr<void> completionContext;
    };
    struct GuestWaveOutState {
        uint32_t callback{};
        uint32_t instance{};
        uint32_t flags{};
        uint32_t avgBytesPerSec{};
        uint32_t deviceId{};
        uint32_t hostFlags{};
        uintptr_t hostCallback{};
        uint16_t formatTag{};
        uint16_t channels{};
        uint32_t samplesPerSec{};
        uint16_t blockAlign{};
        uint16_t bitsPerSample{};
    };
    struct CachedWaveOutDevice {
        uintptr_t hostValue{};
        uint32_t deviceId{};
        uint32_t hostFlags{};
        uintptr_t hostCallback{};
        uint16_t formatTag{};
        uint16_t channels{};
        uint32_t samplesPerSec{};
        uint32_t avgBytesPerSec{};
        uint16_t blockAlign{};
        uint16_t bitsPerSample{};
    };
    struct GuestMessage {
        uint32_t hwnd{};
        uint32_t message{};
        uint32_t wParam{};
        uint32_t lParam{};
        uint32_t time{};
        uint32_t x{};
        uint32_t y{};
        uint32_t synchronousSender{};
        bool crossProcess{};
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
    struct LoadedModuleInfo {
        std::string name;
        std::filesystem::path path;
        uint32_t base{};
        uint32_t imageSize{};
        std::map<std::string, uint32_t> exportsByName;
        std::map<uint16_t, uint32_t> exportsByOrdinal;
    };
    struct GuestFileMapping {
        uint32_t fileHandle{};
        uint64_t size{};
        uint32_t protect{};
        std::string name;
        std::filesystem::path backingPath;
        bool namedShared{};
    };
    struct GuestMappedView {
        uint32_t mappingHandle{};
        uint64_t offset{};
        uint32_t size{};
        std::vector<uint8_t> shadow;
        uint64_t backingVersion{};
    };
    struct GuestTimer {
        uint32_t hwnd{};
        uint32_t id{};
        uint32_t intervalMs{};
        uint32_t callback{};
        uint64_t nextDueMs{};
    };
    struct PendingDestroyWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t stage{};
        uint32_t parent{};
        bool wasVisible{};
    };
    struct PendingCreateWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t createStruct{};
        uint32_t stage{};
    };
    struct PendingBlockingApi {
        std::string name;
        uint16_t ordinal{};
        GuestCallArgs args;
        uint32_t paintDispatches{};
    };
    struct PendingUpdateWindow {
        uint32_t hwnd{};
        uint32_t wndProc{};
        uint32_t originalRa{};
        uint32_t eraseDc{};
        uint32_t stage{};
        std::string sourceName;
    };
    struct PendingMessageTransfer {
        uint32_t hwnd{};
        uint32_t message{};
        uint32_t originalRa{};
        uint32_t synchronousSender{};
        std::string sourceName;
    };
    struct GuestCpuContext {
        std::map<int, uint32_t> registers;
        bool valid{};
    };
    enum class GuestThreadRunState {
        Suspended,
        Runnable,
        Running,
        Waiting,
        WaitingForMessage,
        WaitingForSendMessage,
        Terminated,
    };
    struct GuestThreadState {
        uint32_t handle{};
        uint32_t threadId{};
        uint32_t startAddress{};
        uint32_t parameter{};
        uint32_t stackBase{};
        uint32_t stackSize{};
        uint32_t tlsBase{};
        uint32_t suspendCount{};
        uint32_t exitCode{};
        uint32_t processHandle{};
        uint32_t processId{};
        uint32_t moduleBase{};
        std::string modulePath;
        uint32_t waitHandle{};
        uint64_t sleepUntilMs{};
        std::vector<uint32_t> waitHandles;
        bool waitAll{};
        GuestThreadRunState state{GuestThreadRunState::Suspended};
        GuestCpuContext context;
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
    uint64_t windowZOrder_ = 0;
    bool quitPosted_ = false;
    uint32_t currentCursor_ = 0;
    uint32_t focusedWindow_ = 0;
    uint32_t capturedWindow_ = 0;
    uint32_t hostPointerCaptureWindow_ = 0;
    uint32_t pendingSyntheticChildButtonUpWindow_ = 0;
    uint32_t strtokNext_ = 0;
    uint32_t comProxyVtable_ = 0;
    uint32_t comQueryInterfaceStub_ = 0;
    uint32_t comAddRefStub_ = 0;
    uint32_t comReleaseStub_ = 0;
    uint32_t destroyWindowContinuationStub_ = 0;
    uint32_t createWindowContinuationStub_ = 0;
    uint32_t blockingApiContinuationStub_ = 0;
    uint32_t updateWindowContinuationStub_ = 0;
    uint32_t threadExitStub_ = 0;
    uint32_t messageTransferContinuationStub_ = 0;
    std::string mainModulePath_ = "\\INavi\\INavi.exe";
    std::string sdmmcGuestRoot_ = "\\SDMMC Disk";
    std::filesystem::path sdmmcHostRoot_;
    std::filesystem::path hostMainModulePath_;
    std::filesystem::path crossProcessWindowRegistryPath_;
    std::filesystem::path sharedMappingDirectory_;
    uint32_t mainModuleBase_ = 0;
    std::filesystem::path hostBaseDir_;
    uint16_t nextAtom_ = 0xc000;
    std::map<uint32_t, ExportEntry> exportsByAddress_;
    std::map<std::string, RegisteredSyntheticDll> registeredDllsByName_;
    std::map<uint32_t, uint32_t> allocationSizes_;
    std::map<uint32_t, uint32_t> allocationCapacities_;
    std::multimap<uint32_t, uint32_t> freeBlocksBySize_;
    std::map<uint32_t, uint32_t> tlsValues_;
    std::map<uint32_t, uint32_t> criticalSectionDepth_;
    std::map<uint32_t, uint32_t> syntheticHandleValues_;
    std::map<uint32_t, GuestHandle> guestHandles_;
    std::map<uint32_t, GuestThreadState> guestThreads_;
    GuestCpuContext mainThreadContext_;
    uint32_t activeGuestThread_{};
    uint32_t lastScheduledGuestThread_{};
    uint32_t nextGuestThreadId_{1};
    uint32_t mainThreadPseudoHandle_{0xfffffffeu};
    uint32_t mainProcessPseudoHandle_{0xffffffffu};
    uint32_t mainProcessId_{1};
    uint32_t nextGuestProcessId_{2};
    uint32_t mainThreadTls_{};
    std::map<std::string, GuestWindowClass> windowClassesByName_;
    std::map<uint16_t, std::string> windowClassNamesByAtom_;
    std::map<uint32_t, GuestWindow> windows_;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> externalWindowHandles_;
    std::map<uint32_t, GuestDc> dcs_;
    std::map<uint32_t, GuestBrush> brushes_;
    std::map<uint32_t, GuestPen> pens_;
    std::map<uint32_t, GuestFont> fonts_;
    std::map<uint32_t, GuestBitmap> bitmaps_;
    std::map<int32_t, uint32_t> stockObjects_;
    GuestProcessLauncher guestProcessLauncher_;
    std::map<uint32_t, HostWaveBuffer> hostWaveBuffers_;
    std::map<uint32_t, GuestWaveOutState> waveOutStates_;
    std::vector<CachedWaveOutDevice> cachedWaveOutDevices_;
    std::map<uint32_t, std::string> registryHandles_;
    std::map<uint32_t, std::string> fileHandleDebugNames_;
    std::map<std::wstring, CachedFileAttributes> fileAttributeCache_;
    std::map<uint32_t, SerialDeviceConfig> guestDeviceConfigsByHandle_;
    std::map<uint32_t, uint32_t> fileReadCounts_;
    std::map<uint32_t, uint32_t> fileSeekCounts_;
    std::map<uint32_t, GuestFileMapping> fileMappings_;
    std::map<uint32_t, GuestMappedView> mappedViews_;
    std::map<uint64_t, GuestTimer> timers_;
    std::vector<PendingDestroyWindow> pendingDestroyWindows_;
    std::vector<PendingCreateWindow> pendingCreateWindows_;
    std::vector<PendingBlockingApi> pendingBlockingApis_;
    std::vector<PendingUpdateWindow> pendingUpdateWindows_;
    std::vector<PendingMessageTransfer> pendingMessageTransfers_;
    std::deque<GuestMessage> guestMessages_;
    std::map<uint32_t, uint32_t> retrievedSyncSendersByMsgPtr_;
    bool interactiveSliceActive_{};
    bool interactiveSliceStopRequested_{};
    uint32_t interactiveSliceBlockCounter_{};
    uint64_t interactiveSliceInstructionBudget_{};
    std::string interactiveSliceReason_;
    std::chrono::steady_clock::time_point interactiveSliceDeadline_{};
    std::chrono::steady_clock::time_point lastHostInputQueuedAt_{};
    uint32_t lastMessagePos_{};
    uint32_t lastMessageTime_{};
    std::vector<uintptr_t> retainedHostWindows_;
    uint32_t hostPresenterGuestHwnd_{};
    uint64_t lastHostPresentMs_{};
    bool hostPresentDirty_{};
    std::vector<ResourceEntry> mainResources_;
    std::map<std::string, LoadedModuleInfo> loadedModulesByName_;
    std::map<std::string, LoadedModuleInfo> loadedModulesByPath_;
    std::map<uint32_t, LoadedModuleInfo> loadedModulesByBase_;
    std::map<uint32_t, uint32_t> loadedResourceMemory_;
    std::map<std::string, uint16_t> atomsByName_;
    std::map<uint16_t, std::string> atomNames_;
    uint32_t* framebuffer_{};
    int32_t framebufferWidth_{};
    int32_t framebufferHeight_{};
    uint32_t splashBlitDumpCounter_{};
    uint32_t splashCompositeBitmap_{};
    uint32_t blitProbeLogCounter_{};
    uint32_t blitProbeDumpCounter_{};
    bool splashTopBlitDumped_{};
    bool splashBottomBlitDumped_{};
    bool splashFramebufferDumped_{};
    std::filesystem::path registryPath_;
    nlohmann::json registry_;
    bool registryDirty_{};
    std::filesystem::path serialDeviceMapPath_;
    std::map<std::string, SerialDeviceConfig> serialDevicesByGuest_;
    uint32_t defaultSerialBaud_{9600};
    std::string defaultSerialMode_{"8N1"};
    bool diagnosticDumpsEnabled_{};

    std::optional<SyntheticModule> createCoredll();
    std::optional<SyntheticModule> createCommctrl();
    std::optional<SyntheticModule> createWinsock(const std::string& dllName);
    std::optional<SyntheticModule> createOle32();
    std::optional<SyntheticModule> createOleAut32();
    std::optional<SyntheticModule> createGenericOrdinalDll(const std::string& moduleName, uint16_t maxOrdinal);
    std::optional<SyntheticModule> createGenericOrdinalDll(const SyntheticDllSpec& spec);
    void registerCommctrlExports(SyntheticModule& module);
    void registerCoredllAudioExports(SyntheticModule& module);
    void registerCoredllRegistryExports(SyntheticModule& module);
    void registerCoredllTimeExports(SyntheticModule& module);
    void registerCoredllSystemExports(SyntheticModule& module);
    void registerCoredllMemoryExports(SyntheticModule& module);
    void registerCoredllSyncExports(SyntheticModule& module);
    void registerCoredllCommExports(SyntheticModule& module);
    void registerCoredllRectExports(SyntheticModule& module);
    void registerCoredllThreadExports(SyntheticModule& module);
    void registerCoredllMathExports(SyntheticModule& module);
    void registerCoredllCrtExports(SyntheticModule& module);
    void registerCoredllFsExports(SyntheticModule& module);
    void registerCoredllWindowExports(SyntheticModule& module);
    void registerCoredllPaintExports(SyntheticModule& module);
    void registerCoredllResExports(SyntheticModule& module);
    void registerCoredllGuiExports(SyntheticModule& module);
    void registerHandlers(SyntheticModule& module, const OrdinalHandlerGroup& group);
    void registerHandlers(SyntheticModule& module, const OrdinalHandlerMap& handlers);
    void registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name,
                        SyntheticExportCode code = SyntheticExportCode::Unknown);
    void writeStub(uint32_t address);
    SyntheticModuleKind moduleKindForName(const std::string& moduleName) const;
    const OrdinalHandlerSpec* findOrdinalHandler(const ExportEntry& entry) const;
    void dispatch(const ExportEntry& entry);
    bool dispatchHostWin32(uint16_t ordinal, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchGuestMemoryApi(uint16_t ordinal, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchSimpleHostWin32(uint16_t ordinal, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchOle32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchOleAut32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlInitCommonControls(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlInitClass(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarCreate(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarShow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarHeight(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarInsertComboBox(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarInsertControl(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarAddBitmap(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarInsertMenubar(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarGetMenu(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCommandBarAdornments(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlIsCommandBarMessage(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCreateStatusWindowW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCreateToolbar(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlDrawStatusTextW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlInvertRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlCreatePropertySheetPageW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlDestroyPropertySheetPage(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlPropertySheetW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCommctrlListViewSetItemSpacing(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockCleanup(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockStartup(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockFdIsSet(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockAccept(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockBind(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockCloseSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockConnect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockGetHostByName(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockGetHostName(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockHtonl(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockHtons(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockInetAddr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockInetNtoa(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockIoctlSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockListen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockNtohl(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockNtohs(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockRecv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockSelect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockSend(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockSetSockOpt(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWinsockSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutGetNumDevs(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutSetVolume(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutPrepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutUnprepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutWrite(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutReset(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveOutOpen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInUnprepareHeader(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInAddBuffer(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInReset(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInGetID(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInMessage(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaveInOpen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMixerGetControlDetails(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegCreateKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegCloseKey(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegDeleteKeyW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegDeleteValueW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegEnumValueW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegEnumKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegOpenKeyExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegQueryInfoKeyW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegQueryValueExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegSetValueExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegFlushKey(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSystemTimeToFileTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFileTimeToSystemTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetLocalTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetSystemTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetSystemTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetTimeZoneInformation(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSleep(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetTickCount(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleQueryPerformanceCounter(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleQueryPerformanceFrequency(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetProcessIndexFromID(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetACP(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsValidLocale(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetLastError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetLastError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsBadReadPtr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsBadWritePtr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsProcessDying(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLocalAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLocalReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLocalSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLocalFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRemoteLocalReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapCreate(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapDestroy(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHeapFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetProcessHeap(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleVirtualAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleVirtualFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLocalAllocTrace(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMalloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRealloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMsize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorDelete(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorNew(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorVectorNew(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorVectorDelete(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorNewNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorVectorNewNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorDeleteNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleOperatorVectorDeleteNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRemoteHeapFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInitializeCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDeleteCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEnterCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLeaveCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleTryEnterCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleTlsGetValue(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleTlsSetValue(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleTlsCall(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedTestExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedIncrement(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedDecrement(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedExchangeAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInterlockedCompareExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleClearCommError(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetCommState(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handlePurgeComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetCommMask(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetCommState(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetCommTimeouts(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetupComm(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCopyRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEqualRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleInflateRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsRectEmpty(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handlePtInRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetRectEmpty(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCreateThread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCreateEventW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEventModify(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWaitForMultipleObjects(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleResumeThread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetThreadPriority(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetThreadPriority(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCreateMutexW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleReleaseMutex(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleAtoi(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleAtof(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleAtan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCeil(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCos(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDifftime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFabs(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloor(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleHypot(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handlePow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRand(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSqrt(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSrand(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSin(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrtol(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrtoul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcstoul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFmodf(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLlLshift(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLlDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatToLongLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLongLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleUnsignedLongLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatSub(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleSub(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatMul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleMul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatToLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatToUnsignedLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleToLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleToUnsignedLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleUnsignedLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleToFloat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLongToFloat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleUnsignedLongToFloat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatLessThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatLessEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatGreaterEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatGreaterThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFloatNotEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleLessThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleLessEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleGreaterEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleGreaterThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDoubleNotEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFgetc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFgets(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFopen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFclose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFread(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFwrite(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFflush(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFeof(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFerror(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFseek(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFtell(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWfopen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLongjmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMemcmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMemcpy(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMemset(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrcat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrcmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrcpy(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrcspn(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrlen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrstr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrtok(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStricmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleStrnicmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleUltow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleToupper(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWideFormat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleNarrowFormat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetCrtStorageEx(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetCrtFlags(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSecurityCookie(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetjmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEhvecCtor(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcschr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcscpy(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcscspn(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcslen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcsncmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcsicmp(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcsstr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWcsdup(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWtol(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCreateDirectoryW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleDeleteFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetFileAttributesW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFindFirstFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCreateFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleReadFile(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleWriteFile(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetFileSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetFilePointer(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFlushFileBuffers(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetFileTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFindClose(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFindNextFileW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetFileAttributesExW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetWindowTextW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetWindowTextLengthW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetWindowTextW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEnableWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsWindowEnabled(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetFocus(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetFocus(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleReleaseCapture(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleBeginPaint(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleEndPaint(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetDC(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetPixel(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleReleaseDC(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleValidateRect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetDCEx(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleFindResource(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSizeofResource(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadResource(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadStringW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadAcceleratorsW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadIconW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadImageW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadMenuW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleLoadCursorOrdinalW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRemoveMenu(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCheckMenuItem(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleCheckMenuRadioItem(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetCursor(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetCursorPos(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetSystemMetrics(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetSysColor(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetSysColorBrushOrdinal(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleRegisterWindowMessageW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleAdjustWindowRectEx(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetDlgItem(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetDlgCtrlID(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetForegroundWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetForegroundWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleSetActiveWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleGetActiveWindow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleMessageBoxW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleTranslateAcceleratorW(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    bool handleIsWindowVisible(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret);
    uint32_t handleWNetGetUserW(uint32_t providerName, uint32_t userName, uint32_t lengthPtr);
    uint32_t handleSystemParametersInfoW(uint32_t action, uint32_t uiParam, uint32_t pvParam, uint32_t flags);
    uint32_t handleLoadCursorW(uint32_t instance, uint32_t cursorName);
    uint32_t handleLoadImageApi(const std::string& name, uint32_t instance, uint32_t imageName,
                                uint32_t imageType, uint32_t desiredCx, uint32_t desiredCy,
                                uint32_t loadFlags);
    uint32_t handleGetSysColorBrush(uint32_t colorIndex);
    uint32_t handleGetDeviceCaps(uint32_t dc, uint32_t index);
    uint32_t handleWideCharToMultiByte(uint32_t codePage, uint32_t flags, uint32_t widePtr, uint32_t wideChars);
    uint32_t handleCreateFileMappingW(uint32_t fileHandle, uint32_t security, uint32_t protect, uint32_t sizeHigh);
    uint32_t handleMapViewOfFile(uint32_t mappingHandle, uint32_t desiredAccess, uint32_t offsetHigh, uint32_t offsetLow);
    uint32_t handleUnmapViewOfFile(uint32_t baseAddress);
    uint32_t handleFlushViewOfFile(uint32_t baseAddress, uint32_t bytesToFlush);
    std::filesystem::path ensureSharedMappingDirectory();
    std::filesystem::path sharedMappingBackingPath(const std::string& name);
    uint64_t sharedMappingVersion(const std::filesystem::path& backingPath) const;
    void writeSharedMappingVersion(const std::filesystem::path& backingPath, uint64_t version) const;
    bool ensureSharedMappingBacking(const std::filesystem::path& backingPath, uint64_t requestedSize, uint64_t& actualSize, bool& existed);
    bool readSharedMappingBytes(const std::filesystem::path& backingPath, uint64_t offset, std::vector<uint8_t>& bytes) const;
    bool writeSharedMappingBytes(const std::filesystem::path& backingPath, uint64_t offset, const std::vector<uint8_t>& bytes);
    bool syncNamedMappedView(uint32_t baseAddress, GuestMappedView& view, bool forceWrite);
    void syncNamedMappedViews(bool forceWrite = false);
    uint32_t handleRegEnumValueW(uint32_t hkey, uint32_t index, uint32_t valueNamePtr, uint32_t valueNameSizePtr);
    uint32_t openGuestSerialDevice(const std::string& guestPath, uint32_t access, uint32_t share);
    uint32_t dispatchDeviceIoControl(uint32_t handle, uint32_t controlCode, uint32_t inPtr, uint32_t inSize);

    uint32_t makeGuestHandle(GuestHandle handle);
    GuestHandle* lookupGuestHandle(uint32_t guestHandle);
    uint32_t closeGuestHandle(uint32_t guestHandle);
    uint32_t createPatternBrushFromBitmap(uint32_t bitmapHandle);
    bool readGuestWaitHandles(uint32_t count, uint32_t handlesPtr, std::vector<uint32_t>& handles);
    uint32_t waitForMultipleGuestObjects(uint32_t count, uint32_t handlesPtr, bool waitAll);
    void initializeUserKData();
    void updateCurrentThreadKData(uint32_t currentThreadValue, uint32_t tlsBase);
    GuestCpuContext captureGuestCpuContext() const;
    GuestCpuContext initialGuestThreadContext(uint32_t startAddress, uint32_t parameter, uint32_t stackTop) const;
    void restoreGuestCpuContext(const GuestCpuContext& context) const;
    uint32_t createGuestThread(uint32_t startAddress, uint32_t parameter, uint32_t flags);
    uint32_t resumeGuestThread(uint32_t guestHandle);
    void wakeGuestThreadsWaitingForMessage();
    const GuestThreadState* activeGuestThreadState() const;
    std::string currentProcessModulePath() const;
    uint32_t currentProcessModuleBase() const;
    void refreshCompletedHostWaveBuffers();
    void refreshSignaledGuestWaits();
    bool hasRunnableGuestThread();
    bool switchToRunnableGuestThread(const char* reason, uint32_t returnAddress = 0, uint32_t preferredHandle = 0);
    bool yieldActiveGuestThread(const char* reason, uint32_t returnAddress = 0);
    bool finishActiveGuestThread(uint32_t exitCode);
    bool cooperateGuestThreadsAfterCall(const std::string& name, uint32_t returnAddress = 0);
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
    void syncHostWindowPlacement(GuestWindow& window, bool present);
    std::filesystem::path ensureCrossProcessWindowRegistryPath();
    void publishGuestWindowState(uint32_t hwnd);
    uint64_t nextWindowZOrder();
    uint64_t windowZOrder(uint32_t hwnd) const;
    std::vector<uint32_t> orderedWindowsTopToBottom() const;
    std::vector<uint32_t> orderedSiblingWindows(uint32_t parent, bool childWindow) const;
    std::optional<uint32_t> findExternalGuestWindow(const std::string& className,
                                                    const std::string& title,
                                                    bool matchClass,
                                                    bool matchTitle);
    std::filesystem::path crossProcessMessageQueuePath();
    bool postCrossProcessGuestMessage(uint32_t processId,
                                      uint32_t hwnd,
                                      uint32_t message,
                                      uint32_t wParam,
                                      uint32_t lParam);
    bool postCrossProcessBroadcastMessage(uint32_t message, uint32_t wParam, uint32_t lParam);
    void pollCrossProcessGuestMessages();
    void presentHostWindows(bool force);
    void invalidateHostWindows();
    void queueGuestPaint(uint32_t hwnd, bool erase);
    void prioritizeQueuedWindowMessages(uint32_t hwnd);
    void queueVisibleFullScreenPopupPaint(uint32_t hwnd);
    void queueVisiblePopupPaint(uint32_t hwnd);
    void queueVisiblePopupPaintsAbove(uint32_t hwnd);
    std::pair<int32_t, int32_t> guestWindowOrigin(uint32_t hwnd) const;
    void noteGuestWindowPaint(uint32_t hwnd,
                              int32_t left,
                              int32_t top,
                              int32_t right,
                              int32_t bottom);
    void captureGuestWindowBacking(uint32_t hwnd);
    bool guestWindowCoversFramebuffer(uint32_t hwnd) const;
    bool isWindowInOwnedPopupStack(uint32_t hwnd, uint32_t ancestor) const;
    uint32_t coveringFullScreenOwnedPopup(uint32_t hwnd) const;
    void retireOlderFullScreenOwnedPopupsForPopup(uint32_t popupHwnd);
    bool restoreGuestWindowBacking(uint32_t hwnd,
                                   GuestWindow& window,
                                   bool allowCoveredByNewer = false,
                                   bool presentRestoredFrame = true);
    void eraseGuestWindowArea(uint32_t hwnd, const GuestWindow& window);
    bool isWindowOrDescendant(uint32_t hwnd, uint32_t ancestor) const;
    bool isOwnedPopupWindow(uint32_t hwnd) const;
    bool isTopLevelPopupWindow(uint32_t hwnd) const;
    bool hasCoveringRootPopup(uint32_t hwnd) const;
    uint32_t readFramebufferTargetPixel(uint32_t targetHwnd, int32_t x, int32_t y) const;
    void writeFramebufferTargetPixel(uint32_t targetHwnd, int32_t x, int32_t y, uint32_t pixel);
    void pumpHostMessages();
    void enqueueDueTimers();
    uint32_t timerWaitMilliseconds() const;
    uint32_t windowAtPoint(uint32_t rootGuestHwnd, int32_t x, int32_t y, int32_t& clientX, int32_t& clientY) const;
    void compactQueuedPointerMotion(size_t maxMotionPerWindow = 2);
    uint32_t colorRefToPixel(uint32_t colorRef) const;
    bool readGuestRect(uint32_t address, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom) const;
    void writeGuestRect(uint32_t address, int32_t left, int32_t top, int32_t right, int32_t bottom) const;
    void fillFramebufferRect(const GuestDc& dc, int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t pixel);
    void drawFramebufferLine(const GuestDc& dc, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel);
    bool fillBitmapRect(const GuestBitmap& bitmap, int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t pixel);
    bool drawBitmapLine(const GuestBitmap& bitmap, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel);
    bool fillDcRect(const GuestDc& dc, int32_t left, int32_t top, int32_t right, int32_t bottom, uint32_t pixel);
    bool drawDcLine(const GuestDc& dc, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel);
    bool fillDcPolygon(const GuestDc& dc, const std::vector<std::pair<int32_t, int32_t>>& points, uint32_t pixel);
    bool handleCreateBitmap(const GuestCallArgs& args, uint32_t& ret);
    bool handleGetObjectW(const GuestCallArgs& args, uint32_t& ret);
    bool handleSetDIBColorTable(const GuestCallArgs& args, uint32_t& ret);
    bool handleSetBitmapBits(const GuestCallArgs& args, uint32_t& ret);
    bool handleSetDIBitsToDevice(const GuestCallArgs& args, uint32_t& ret);
    bool drawHostTextToDc(const GuestDc& dc, int32_t x, int32_t y, uint32_t options,
                          uint32_t rectPtr, uint32_t textPtr, int32_t textChars,
                          uint32_t drawTextFormat, bool drawTextCall);
    bool readBitmapPixel(const GuestBitmap& bitmap, const std::vector<uint8_t>& bits,
                         int32_t height, int32_t x, int32_t y, uint32_t& pixel) const;
    bool writeBitmapPixel(const GuestBitmap& bitmap, std::vector<uint8_t>& bits,
                          int32_t height, int32_t x, int32_t y, uint32_t pixel) const;
    BitmapProbeStats bitmapProbeStats(const GuestBitmap& bitmap, int32_t x, int32_t y,
                                      int32_t width, int32_t height) const;
    void dumpGuestBitmapPpm(uint32_t bitmapHandle, const GuestBitmap& bitmap, const std::string& tag);
    void dumpFramebufferPpm(const std::string& tag);
    bool stretchDibToFramebuffer(const GuestDc& dc, int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                                 int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                 uint32_t bitsPtr, uint32_t infoPtr);
    bool stretchDibToBitmap(const GuestBitmap& dstBitmap, int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                            int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                            uint32_t bitsPtr, uint32_t infoPtr);
    bool bitBltToFramebuffer(const GuestDc& dstDc, const GuestBitmap& bitmap,
                             int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                             int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                             uint32_t rop);
    bool bitBltToBitmap(const GuestBitmap& dstBitmap, const GuestBitmap& srcBitmap,
                        int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                        int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                        uint32_t rop);
    bool transparentImageToFramebuffer(const GuestDc& dstDc, const GuestBitmap& srcBitmap,
                                       int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                                       int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                       uint32_t transparentColor);
    bool transparentImageToBitmap(const GuestBitmap& dstBitmap, const GuestBitmap& srcBitmap,
                                  int32_t dstX, int32_t dstY, int32_t dstW, int32_t dstH,
                                  int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH,
                                  uint32_t transparentColor);
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
    void beginInteractiveSlice(std::chrono::milliseconds wallBudget, const char* reason,
                               uint64_t instructionBudget);
    void endInteractiveSlice();
    uint32_t reg(int regId) const;
    void setReg(int regId, uint32_t value) const;
    uint32_t stackArg(uint32_t index) const;
    uint32_t allocate(uint32_t size, bool zeroFill);
    void releaseAllocation(uint32_t address);
    uint32_t allocationSize(uint32_t address) const;
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
    void refreshGuestMainModulePath();
    std::vector<std::string> virtualRootNames() const;
    bool isUnderFileSystemRoot(const std::filesystem::path& path) const;
    uint32_t normalizeVirtualFileMiss(const std::filesystem::path& hostPath, uint32_t error) const;
};
