#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "synthetic_dll.h"

void SyntheticDllRuntime::registerCoredllTimeExports(SyntheticModule& module) {
    struct CoreDllTime {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.time",
                {
                    {0x0013, {"SystemTimeToFileTime", Code::CoreDllSystemTimeToFileTime, &SyntheticDllRuntime::handleSystemTimeToFileTime}},
                    {0x0014, {"FileTimeToSystemTime", Code::CoreDllFileTimeToSystemTime, &SyntheticDllRuntime::handleFileTimeToSystemTime}},
                    {0x0017, {"GetLocalTime", Code::CoreDllGetLocalTime, &SyntheticDllRuntime::handleGetLocalTime}},
                    {0x0019, {"GetSystemTime", Code::CoreDllGetSystemTime, &SyntheticDllRuntime::handleGetSystemTime}},
                    {0x001B, {"GetTimeZoneInformation", Code::CoreDllGetTimeZoneInformation, &SyntheticDllRuntime::handleGetTimeZoneInformation}},
                    {0x0025, {"GetSystemTime", Code::CoreDllGetSystemTime, &SyntheticDllRuntime::handleGetSystemTime}},
                    {0x01F0, {"Sleep", Code::CoreDllSleep, &SyntheticDllRuntime::handleSleep}},
                    {0x0217, {"GetTickCount", Code::CoreDllGetTickCount, &SyntheticDllRuntime::handleGetTickCount}},
                    {0x021A, {"QueryPerformanceCounter", Code::CoreDllQueryPerformanceCounter, &SyntheticDllRuntime::handleQueryPerformanceCounter}},
                    {0x021B, {"QueryPerformanceFrequency", Code::CoreDllQueryPerformanceFrequency, &SyntheticDllRuntime::handleQueryPerformanceFrequency}},
                },
            };
        }
    };

    const CoreDllTime time;
    registerHandlers(module, time.group());
}

bool SyntheticDllRuntime::handleSystemTimeToFileTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    SYSTEMTIME systemTime{};
    FILETIME fileTime{};
    if (args.a0) uc_mem_read(uc_, args.a0, &systemTime, sizeof(systemTime));
    const BOOL ok = args.a0 && args.a1 && ::SystemTimeToFileTime(&systemTime, &fileTime);
    if (ok) uc_mem_write(uc_, args.a1, &fileTime, sizeof(fileTime));
    ret = ok ? 1 : 0;
    if (!ret) lastError_ = GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleFileTimeToSystemTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    FILETIME fileTime{};
    SYSTEMTIME systemTime{};
    if (args.a0) uc_mem_read(uc_, args.a0, &fileTime, sizeof(fileTime));
    const BOOL ok = args.a0 && args.a1 && ::FileTimeToSystemTime(&fileTime, &systemTime);
    if (ok) uc_mem_write(uc_, args.a1, &systemTime, sizeof(systemTime));
    ret = ok ? 1 : 0;
    if (!ret) lastError_ = GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleGetLocalTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    SYSTEMTIME systemTime{};
    ::GetLocalTime(&systemTime);
    if (args.a0) uc_mem_write(uc_, args.a0, &systemTime, sizeof(systemTime));
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetSystemTime(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    SYSTEMTIME systemTime{};
    ::GetSystemTime(&systemTime);
    if (args.a0) uc_mem_write(uc_, args.a0, &systemTime, sizeof(systemTime));
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetTimeZoneInformation(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    TIME_ZONE_INFORMATION timeZone{};
    ret = ::GetTimeZoneInformation(&timeZone);
    if (args.a0) uc_mem_write(uc_, args.a0, &timeZone, sizeof(timeZone));
    return true;
}

bool SyntheticDllRuntime::handleSleep(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ::Sleep(args.a0);
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleGetTickCount(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = ::GetTickCount();
    return true;
}

bool SyntheticDllRuntime::handleQueryPerformanceCounter(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    LARGE_INTEGER value{};
    const BOOL ok = ::QueryPerformanceCounter(&value);
    if (args.a0) {
        writeU32(args.a0, uint32_t(value.QuadPart));
        writeU32(args.a0 + 4, uint32_t(uint64_t(value.QuadPart) >> 32));
    }
    ret = ok ? 1 : 0;
    if (!ret) lastError_ = GetLastError();
    return true;
}

bool SyntheticDllRuntime::handleQueryPerformanceFrequency(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    LARGE_INTEGER value{};
    const BOOL ok = ::QueryPerformanceFrequency(&value);
    if (args.a0) {
        writeU32(args.a0, uint32_t(value.QuadPart));
        writeU32(args.a0 + 4, uint32_t(uint64_t(value.QuadPart) >> 32));
    }
    ret = ok ? 1 : 0;
    if (!ret) lastError_ = GetLastError();
    return true;
}
