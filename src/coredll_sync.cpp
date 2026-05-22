#include "synthetic_dll.h"

void SyntheticDllRuntime::registerCoredllSyncExports(SyntheticModule& module) {
    struct CoreDllSync {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.sync",
                {
                    {0x0002, {"InitializeCriticalSection", Code::CoreDllInitializeCriticalSection, &SyntheticDllRuntime::handleInitializeCriticalSection}},
                    {0x0003, {"DeleteCriticalSection", Code::CoreDllDeleteCriticalSection, &SyntheticDllRuntime::handleDeleteCriticalSection}},
                    {0x0004, {"EnterCriticalSection", Code::CoreDllEnterCriticalSection, &SyntheticDllRuntime::handleEnterCriticalSection}},
                    {0x0005, {"LeaveCriticalSection", Code::CoreDllLeaveCriticalSection, &SyntheticDllRuntime::handleLeaveCriticalSection}},
                    {0x0009, {"InterlockedTestExchange", Code::CoreDllInterlockedTestExchange, &SyntheticDllRuntime::handleInterlockedTestExchange}},
                    {0x000A, {"InterlockedIncrement", Code::CoreDllInterlockedIncrement, &SyntheticDllRuntime::handleInterlockedIncrement}},
                    {0x000B, {"InterlockedDecrement", Code::CoreDllInterlockedDecrement, &SyntheticDllRuntime::handleInterlockedDecrement}},
                    {0x000C, {"InterlockedExchange", Code::CoreDllInterlockedExchange, &SyntheticDllRuntime::handleInterlockedExchange}},
                    {0x000F, {"TlsGetValue", Code::CoreDllTlsGetValue, &SyntheticDllRuntime::handleTlsGetValue}},
                    {0x0010, {"TlsSetValue", Code::CoreDllTlsSetValue, &SyntheticDllRuntime::handleTlsSetValue}},
                    {0x0208, {"TlsCall", Code::CoreDllTlsCall, &SyntheticDllRuntime::handleTlsCall}},
                    {0x04D1, {"TryEnterCriticalSection", Code::CoreDllTryEnterCriticalSection, &SyntheticDllRuntime::handleTryEnterCriticalSection}},
                    {0x05D3, {"InterlockedExchangeAdd", Code::CoreDllInterlockedExchangeAdd, &SyntheticDllRuntime::handleInterlockedExchangeAdd}},
                    {0x05D4, {"InterlockedCompareExchange", Code::CoreDllInterlockedCompareExchange, &SyntheticDllRuntime::handleInterlockedCompareExchange}},
                },
            };
        }
    };

    const CoreDllSync sync;
    registerHandlers(module, sync.group());
}

bool SyntheticDllRuntime::handleInitializeCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    criticalSectionDepth_[args.a0] = 0;
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleDeleteCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    criticalSectionDepth_.erase(args.a0);
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleEnterCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ++criticalSectionDepth_[args.a0];
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleLeaveCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = criticalSectionDepth_.find(args.a0);
    if (it != criticalSectionDepth_.end() && it->second) --it->second;
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleTryEnterCriticalSection(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleEnterCriticalSection(code, args, ret);
}

bool SyntheticDllRuntime::handleTlsGetValue(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = tlsValues_.find(args.a0);
    ret = it == tlsValues_.end() ? 0 : it->second;
    return true;
}

bool SyntheticDllRuntime::handleTlsSetValue(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    tlsValues_[args.a0] = args.a1;
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleTlsCall(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = tlsValues_[args.a0];
    return true;
}

bool SyntheticDllRuntime::handleInterlockedIncrement(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t value = 0;
    uc_mem_read(uc_, args.a0, &value, sizeof(value));
    ++value;
    uc_mem_write(uc_, args.a0, &value, sizeof(value));
    ret = value;
    return true;
}

bool SyntheticDllRuntime::handleInterlockedDecrement(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t value = 0;
    uc_mem_read(uc_, args.a0, &value, sizeof(value));
    --value;
    uc_mem_write(uc_, args.a0, &value, sizeof(value));
    ret = value;
    return true;
}

bool SyntheticDllRuntime::handleInterlockedExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t old = 0;
    uc_mem_read(uc_, args.a0, &old, sizeof(old));
    uc_mem_write(uc_, args.a0, &args.a1, sizeof(args.a1));
    ret = old;
    return true;
}

bool SyntheticDllRuntime::handleInterlockedExchangeAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t old = 0;
    uc_mem_read(uc_, args.a0, &old, sizeof(old));
    const uint32_t next = old + args.a1;
    uc_mem_write(uc_, args.a0, &next, sizeof(next));
    ret = old;
    return true;
}

bool SyntheticDllRuntime::handleInterlockedTestExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t old = 0;
    uc_mem_read(uc_, args.a0, &old, sizeof(old));
    if (old == args.a1) uc_mem_write(uc_, args.a0, &args.a2, sizeof(args.a2));
    ret = old;
    return true;
}

bool SyntheticDllRuntime::handleInterlockedCompareExchange(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    uint32_t old = 0;
    uc_mem_read(uc_, args.a0, &old, sizeof(old));
    if (old == args.a2) uc_mem_write(uc_, args.a0, &args.a1, sizeof(args.a1));
    ret = old;
    return true;
}
