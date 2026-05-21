#pragma once

#include <unicorn/unicorn.h>

#include <array>
#include <cstdint>
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
        enum class Kind { HostFile, HostFind, HostWaveIn, HostEvent, HostMutex, Pseudo };

        Kind kind{Kind::Pseudo};
        uintptr_t hostValue{};
        uint32_t filePointer{};
    };
    struct GuestWindowClass {
        std::array<uint8_t, 40> bytes{};
        std::string name;
        uint16_t atom{};
    };
    struct HostWaveBuffer {
        std::vector<uint8_t> data;
        std::array<uint8_t, 64> header{};
    };

    uc_engine* uc_{};
    uint32_t nextModuleBase_ = 0x70000000;
    uint32_t heapBase_ = 0x30000000;
    uint32_t heapLimit_ = 0x34000000;
    uint32_t nextHeap_ = 0x30010000;
    uint32_t lastError_ = 0;
    uint32_t nextHandle_ = 0x10000;
    uint64_t tick_ = 0;
    std::string mainModulePath_ = "\\INavi\\INavi.exe";
    std::filesystem::path hostBaseDir_;
    uint16_t nextAtom_ = 0xc000;
    std::map<uint32_t, ExportEntry> exportsByAddress_;
    std::map<uint32_t, uint32_t> allocationSizes_;
    std::map<uint32_t, uint32_t> tlsValues_;
    std::map<uint32_t, uint32_t> syntheticHandleValues_;
    std::map<uint32_t, GuestHandle> guestHandles_;
    std::map<std::string, GuestWindowClass> windowClassesByName_;
    std::map<uint16_t, std::string> windowClassNamesByAtom_;
    std::map<uint32_t, HostWaveBuffer> hostWaveBuffers_;
    std::map<std::string, uint16_t> atomsByName_;
    std::map<uint16_t, std::string> atomNames_;

    std::optional<SyntheticModule> createCoredll();
    std::optional<SyntheticModule> createGenericOrdinalDll(const std::string& moduleName, uint16_t maxOrdinal);
    void registerExport(SyntheticModule& module, uint16_t ordinal, const std::string& name);
    void writeStub(uint32_t address);
    void dispatch(const ExportEntry& entry);
    bool dispatchHostWin32(const std::string& name, const GuestCallArgs& args, uint32_t& ret);
    bool dispatchGuestMemoryApi(const std::string& name, const GuestCallArgs& args, uint32_t& ret);

    uint32_t makeGuestHandle(GuestHandle handle);
    GuestHandle* lookupGuestHandle(uint32_t guestHandle);
    uint32_t closeGuestHandle(uint32_t guestHandle);
    uint32_t reg(int regId) const;
    void setReg(int regId, uint32_t value) const;
    uint32_t stackArg(uint32_t index) const;
    uint32_t allocate(uint32_t size, bool zeroFill);
    void writeU32(uint32_t address, uint32_t value) const;
    bool copyGuest(uint32_t dst, uint32_t src, uint32_t size) const;
    bool fillGuest(uint32_t dst, uint8_t value, uint32_t size) const;
    std::string readAscii(uint32_t address, size_t maxChars = 512) const;
    void writeAscii(uint32_t address, const std::string& value) const;
    std::string readUtf16(uint32_t address, size_t maxChars = 512) const;
    uint32_t writeUtf16(uint32_t address, const std::string& value, uint32_t maxChars) const;
    std::filesystem::path resolveGuestPath(const std::string& guestPath) const;
};
