#include "synthetic_dll.h"

#include <algorithm>
#include <vector>

#include <spdlog/spdlog.h>

namespace {

uint32_t heapFlags(uint32_t flags) {
    return (flags & 0x00000008u) != 0;
}

} // namespace

void SyntheticDllRuntime::registerCoredllMemoryExports(SyntheticModule& module) {
    struct CoreDllMemory {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.memory",
                {
                    {0x0021, {"LocalAlloc", Code::CoreDllLocalAlloc, &SyntheticDllRuntime::handleLocalAlloc}},
                    {0x0022, {"LocalReAlloc", Code::CoreDllLocalReAlloc, &SyntheticDllRuntime::handleLocalReAlloc}},
                    {0x0023, {"LocalSize", Code::CoreDllLocalSize, &SyntheticDllRuntime::handleLocalSize}},
                    {0x0024, {"LocalFree", Code::CoreDllLocalFree, &SyntheticDllRuntime::handleLocalFree}},
                    {0x0026, {"RemoteLocalReAlloc", Code::CoreDllRemoteLocalReAlloc, &SyntheticDllRuntime::handleRemoteLocalReAlloc}},
                    {0x002C, {"HeapCreate", Code::CoreDllHeapCreate, &SyntheticDllRuntime::handleHeapCreate}},
                    {0x002D, {"HeapDestroy", Code::CoreDllHeapDestroy, &SyntheticDllRuntime::handleHeapDestroy}},
                    {0x002E, {"HeapAlloc", Code::CoreDllHeapAlloc, &SyntheticDllRuntime::handleHeapAlloc}},
                    {0x002F, {"HeapReAlloc", Code::CoreDllHeapReAlloc, &SyntheticDllRuntime::handleHeapReAlloc}},
                    {0x0030, {"HeapSize", Code::CoreDllHeapSize, &SyntheticDllRuntime::handleHeapSize}},
                    {0x0031, {"HeapFree", Code::CoreDllHeapFree, &SyntheticDllRuntime::handleHeapFree}},
                    {0x0032, {"GetProcessHeap", Code::CoreDllGetProcessHeap, &SyntheticDllRuntime::handleGetProcessHeap}},
                    {0x020C, {"VirtualAlloc", Code::CoreDllVirtualAlloc, &SyntheticDllRuntime::handleVirtualAlloc}},
                    {0x020D, {"VirtualFree", Code::CoreDllVirtualFree, &SyntheticDllRuntime::handleVirtualFree}},
                    {0x02DD, {"LocalAllocTrace", Code::CoreDllLocalAllocTrace, &SyntheticDllRuntime::handleLocalAllocTrace}},
                    {0x03FA, {"free", Code::CoreDllFree, &SyntheticDllRuntime::handleFree}},
                    {0x0411, {"malloc", Code::CoreDllMalloc, &SyntheticDllRuntime::handleMalloc}},
                    {0x0419, {"_msize", Code::CoreDllMsize, &SyntheticDllRuntime::handleMsize}},
                    {0x041E, {"realloc", Code::CoreDllRealloc, &SyntheticDllRuntime::handleRealloc}},
                    {0x0446, {"operator_delete", Code::CoreDllOperatorDelete, &SyntheticDllRuntime::handleOperatorDelete}},
                    {0x0447, {"operator_new", Code::CoreDllOperatorNew, &SyntheticDllRuntime::handleOperatorNew}},
                    {0x0531, {"operator_delete", Code::CoreDllOperatorDelete, &SyntheticDllRuntime::handleOperatorDelete}},
                    {0x0532, {"operator_new", Code::CoreDllOperatorNew, &SyntheticDllRuntime::handleOperatorNew}},
                    {0x0533, {"operator_vector_new", Code::CoreDllOperatorVectorNew, &SyntheticDllRuntime::handleOperatorVectorNew}},
                    {0x0534, {"operator_vector_delete", Code::CoreDllOperatorVectorDelete, &SyntheticDllRuntime::handleOperatorVectorDelete}},
                    {0x0535, {"operator_new_nothrow", Code::CoreDllOperatorNewNoThrow, &SyntheticDllRuntime::handleOperatorNewNoThrow}},
                    {0x0536, {"operator_vector_new_nothrow", Code::CoreDllOperatorVectorNewNoThrow, &SyntheticDllRuntime::handleOperatorVectorNewNoThrow}},
                    {0x0537, {"operator_delete_nothrow", Code::CoreDllOperatorDeleteNoThrow, &SyntheticDllRuntime::handleOperatorDeleteNoThrow}},
                    {0x0538, {"operator_vector_delete_nothrow", Code::CoreDllOperatorVectorDeleteNoThrow, &SyntheticDllRuntime::handleOperatorVectorDeleteNoThrow}},
                    {0x05B0, {"operator_vector_new", Code::CoreDllOperatorVectorNew, &SyntheticDllRuntime::handleOperatorVectorNew}},
                    {0x05B1, {"operator_vector_delete", Code::CoreDllOperatorVectorDelete, &SyntheticDllRuntime::handleOperatorVectorDelete}},
                    {0x0646, {"RemoteHeapFree", Code::CoreDllRemoteHeapFree, &SyntheticDllRuntime::handleRemoteHeapFree}},
                },
            };
        }
    };

    const CoreDllMemory memory;
    registerHandlers(module, memory.group());
}

bool SyntheticDllRuntime::handleLocalAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocate(args.a1, (args.a0 & 0x0040u) != 0);
    return true;
}

bool SyntheticDllRuntime::handleMalloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocate(args.a0, false);
    if (args.ra == 0x00327e60u || args.ra == 0x00327e6cu) {
        spdlog::info("diag double-buffer malloc ra=0x{:08x} size=0x{:x} -> 0x{:08x} activeThread=0x{:08x}",
                     args.ra, args.a0, ret, activeGuestThread_);
    }
    return true;
}

bool SyntheticDllRuntime::handleLocalAllocTrace(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (args.a2 == 0x38 && args.a1) {
        uint32_t firstWord = 0;
        ret = uc_mem_read(uc_, args.a1, &firstWord, sizeof(firstWord)) == UC_ERR_OK ? firstWord : 0;
    } else {
        ret = allocate(args.a1 ? args.a1 : 1, false);
    }
    return true;
}

bool SyntheticDllRuntime::handleLocalFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    releaseAllocation(args.a0);
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    if (args.ra == 0x00327ee8u || args.ra == 0x00327ef0u) {
        spdlog::info("diag double-buffer free ra=0x{:08x} ptr=0x{:08x} size={} activeThread=0x{:08x}",
                     args.ra, args.a0, allocationSize(args.a0), activeGuestThread_);
    }
    return handleLocalFree(code, args, ret);
}

bool SyntheticDllRuntime::handleLocalSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocationSize(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleHeapSize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocationSize(args.a2);
    return true;
}

bool SyntheticDllRuntime::handleLocalReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (!args.a1) {
        releaseAllocation(args.a0);
        ret = 0;
        return true;
    }
    const uint32_t oldSize = allocationSize(args.a0);
    ret = allocate(args.a1, (args.a2 & 0x0040u) != 0);
    if (args.a0 && ret && oldSize) {
        std::vector<uint8_t> bytes(std::min(oldSize, args.a1));
        uc_mem_read(uc_, args.a0, bytes.data(), bytes.size());
        uc_mem_write(uc_, ret, bytes.data(), bytes.size());
        releaseAllocation(args.a0);
    }
    return true;
}

bool SyntheticDllRuntime::handleRemoteLocalReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleLocalReAlloc(code, args, ret);
}

bool SyntheticDllRuntime::handleRealloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    if (!args.a1) {
        releaseAllocation(args.a0);
        ret = 0;
        return true;
    }
    const uint32_t oldSize = allocationSize(args.a0);
    ret = allocate(args.a1, false);
    if (args.a0 && ret && oldSize) {
        std::vector<uint8_t> bytes(std::min(oldSize, args.a1));
        uc_mem_read(uc_, args.a0, bytes.data(), bytes.size());
        uc_mem_write(uc_, ret, bytes.data(), bytes.size());
        releaseAllocation(args.a0);
    }
    return true;
}

bool SyntheticDllRuntime::handleMsize(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocationSize(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleHeapCreate(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = makeGuestHandle({GuestHandle::Kind::GuestHeap, 0, 0});
    return true;
}

bool SyntheticDllRuntime::handleHeapDestroy(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto it = ceKernel_.handles().find(args.a0);
    if (it == ceKernel_.handles().end() || it->second.kind != GuestHandle::Kind::GuestHeap ||
        args.a0 == processHeapHandle_) {
        lastError_ = 6;
        ret = 0;
    } else {
        ceKernel_.handles().erase(it);
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleGetProcessHeap(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    if (!processHeapHandle_) {
        processHeapHandle_ = makeGuestHandle({GuestHandle::Kind::GuestHeap, 0, 0});
    }
    ret = processHeapHandle_;
    return true;
}

bool SyntheticDllRuntime::handleHeapAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* heap = lookupGuestHandle(args.a0);
    if (!heap || heap->kind != GuestHandle::Kind::GuestHeap) {
        lastError_ = 6;
        ret = 0;
    } else {
        ret = allocate(args.a2, heapFlags(args.a1) != 0);
        lastError_ = ret ? 0 : 8;
    }
    return true;
}

bool SyntheticDllRuntime::handleHeapReAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* heap = lookupGuestHandle(args.a0);
    if (!heap || heap->kind != GuestHandle::Kind::GuestHeap || !args.a2) {
        lastError_ = 6;
        ret = 0;
    } else {
        const uint32_t oldSize = allocationSize(args.a2);
        ret = allocate(args.a3, heapFlags(args.a1) != 0);
        if (ret && oldSize) {
            std::vector<uint8_t> bytes(std::min(oldSize, args.a3));
            uc_mem_read(uc_, args.a2, bytes.data(), bytes.size());
            uc_mem_write(uc_, ret, bytes.data(), bytes.size());
            releaseAllocation(args.a2);
        }
        lastError_ = ret ? 0 : 8;
    }
    return true;
}

bool SyntheticDllRuntime::handleHeapFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* heap = lookupGuestHandle(args.a0);
    if (!heap || heap->kind != GuestHandle::Kind::GuestHeap || !args.a2) {
        lastError_ = 6;
        ret = 0;
    } else {
        releaseAllocation(args.a2);
        lastError_ = 0;
        ret = 1;
    }
    return true;
}

bool SyntheticDllRuntime::handleVirtualAlloc(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocate(args.a1, true);
    return true;
}

bool SyntheticDllRuntime::handleVirtualFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    releaseAllocation(args.a0);
    ret = 1;
    return true;
}

bool SyntheticDllRuntime::handleOperatorNew(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = allocate(args.a0, false);
    return true;
}

bool SyntheticDllRuntime::handleOperatorVectorNew(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorNew(code, args, ret);
}

bool SyntheticDllRuntime::handleOperatorNewNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorNew(code, args, ret);
}

bool SyntheticDllRuntime::handleOperatorVectorNewNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorNew(code, args, ret);
}

bool SyntheticDllRuntime::handleOperatorDelete(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    releaseAllocation(args.a0);
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleOperatorVectorDelete(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorDelete(code, args, ret);
}

bool SyntheticDllRuntime::handleOperatorDeleteNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorDelete(code, args, ret);
}

bool SyntheticDllRuntime::handleOperatorVectorDeleteNoThrow(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    return handleOperatorDelete(code, args, ret);
}

bool SyntheticDllRuntime::handleRemoteHeapFree(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = copyGuest(args.a0, args.a2, 0x38) ? args.a0 : 1;
    return true;
}
