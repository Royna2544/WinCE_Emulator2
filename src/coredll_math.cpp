#include "synthetic_dll.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <spdlog/spdlog.h>

namespace {

float guestFloat(uint32_t value) {
    float result = 0.0f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

uint32_t guestFloatBits(float value) {
    uint32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

double doubleFromGuestPair(uint32_t low, uint32_t high) {
    const uint64_t bits = (uint64_t(high) << 32) | uint64_t(low);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void setGuestDoubleReturn(uc_engine* uc, double value, uint32_t& ret) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    ret = uint32_t(bits);
    const uint32_t high = uint32_t(bits >> 32);
    uc_reg_write(uc, UC_MIPS_REG_V1, &high);
}

}

void SyntheticDllRuntime::registerCoredllMathExports(SyntheticModule& module) {
    struct CoreDllMath {
        OrdinalHandlerGroup group() const {
            using Code = SyntheticExportCode;
            return OrdinalHandlerGroup{
                "coredll.math",
                {
                    {0x03E1, {"atoi", Code::CoreDllAtoi, &SyntheticDllRuntime::handleAtoi}},
                    {0x03E3, {"atof", Code::CoreDllAtof, &SyntheticDllRuntime::handleAtof}},
                    {0x03DF, {"atan", Code::CoreDllAtan, &SyntheticDllRuntime::handleAtan}},
                    {0x03EC, {"cos", Code::CoreDllCos, &SyntheticDllRuntime::handleCos}},
                    {0x03FF, {"_hypot", Code::CoreDllHypot, &SyntheticDllRuntime::handleHypot}},
                    {0x041D, {"rand", Code::CoreDllRand, &SyntheticDllRuntime::handleRand}},
                    {0x0422, {"sin", Code::CoreDllSin, &SyntheticDllRuntime::handleSin}},
                    {0x0424, {"sqrt", Code::CoreDllSqrt, &SyntheticDllRuntime::handleSqrt}},
                    {0x0425, {"srand", Code::CoreDllSrand, &SyntheticDllRuntime::handleSrand}},
                    {0x043B, {"wcstoul", Code::CoreDllWcstoul, &SyntheticDllRuntime::handleWcstoul}},
                    {0x057C, {"strtol", Code::CoreDllStrtol, &SyntheticDllRuntime::handleStrtol}},
                    {0x057D, {"strtoul", Code::CoreDllStrtoul, &SyntheticDllRuntime::handleStrtoul}},
                    {0x066B, {"fmodf", Code::CoreDllFmodf, &SyntheticDllRuntime::handleFmodf}},
                    {0x07D5, {"__ll_div", Code::CoreDllLlDiv, &SyntheticDllRuntime::handleLlDiv}},
                    {0x07E2, {"__f_to_ll", Code::CoreDllFloatToLongLong, &SyntheticDllRuntime::handleFloatToLongLong}},
                    {0x07E6, {"__fpadd", Code::CoreDllFloatAdd, &SyntheticDllRuntime::handleFloatAdd}},
                    {0x07E7, {"__dpadd", Code::CoreDllDoubleAdd, &SyntheticDllRuntime::handleDoubleAdd}},
                    {0x07E8, {"__fpsub", Code::CoreDllFloatSub, &SyntheticDllRuntime::handleFloatSub}},
                    {0x07E9, {"__dpsub", Code::CoreDllDoubleSub, &SyntheticDllRuntime::handleDoubleSub}},
                    {0x07EA, {"__fpmul", Code::CoreDllFloatMul, &SyntheticDllRuntime::handleFloatMul}},
                    {0x07EB, {"__dpmul", Code::CoreDllDoubleMul, &SyntheticDllRuntime::handleDoubleMul}},
                    {0x07EC, {"__fpdiv", Code::CoreDllFloatDiv, &SyntheticDllRuntime::handleFloatDiv}},
                    {0x07ED, {"__dpdiv", Code::CoreDllDoubleDiv, &SyntheticDllRuntime::handleDoubleDiv}},
                    {0x07EE, {"__fptoli", Code::CoreDllFloatToLong, &SyntheticDllRuntime::handleFloatToLong}},
                    {0x07EF, {"__fptoul", Code::CoreDllFloatToUnsignedLong, &SyntheticDllRuntime::handleFloatToUnsignedLong}},
                    {0x07F0, {"__litofp", Code::CoreDllLongToFloat, &SyntheticDllRuntime::handleLongToFloat}},
                    {0x07F2, {"__dptoli", Code::CoreDllDoubleToLong, &SyntheticDllRuntime::handleDoubleToLong}},
                    {0x07F3, {"__dptoul", Code::CoreDllDoubleToUnsignedLong, &SyntheticDllRuntime::handleDoubleToUnsignedLong}},
                    {0x07F4, {"__litodp", Code::CoreDllLongToDouble, &SyntheticDllRuntime::handleLongToDouble}},
                    {0x07F5, {"__ultodp", Code::CoreDllUnsignedLongToDouble, &SyntheticDllRuntime::handleUnsignedLongToDouble}},
                    {0x07F6, {"__fptodp", Code::CoreDllFloatToDouble, &SyntheticDllRuntime::handleFloatToDouble}},
                    {0x07F7, {"__dptofp", Code::CoreDllDoubleToFloat, &SyntheticDllRuntime::handleDoubleToFloat}},
                    {0x07FA, {"__lts", Code::CoreDllFloatLessThan, &SyntheticDllRuntime::handleFloatLessThan}},
                    {0x07FB, {"__les", Code::CoreDllFloatLessEqual, &SyntheticDllRuntime::handleFloatLessEqual}},
                    {0x07FC, {"__eqs", Code::CoreDllFloatEqual, &SyntheticDllRuntime::handleFloatEqual}},
                    {0x07FD, {"__ges", Code::CoreDllFloatGreaterEqual, &SyntheticDllRuntime::handleFloatGreaterEqual}},
                    {0x07FE, {"__gts", Code::CoreDllFloatGreaterThan, &SyntheticDllRuntime::handleFloatGreaterThan}},
                    {0x07FF, {"__nes", Code::CoreDllFloatNotEqual, &SyntheticDllRuntime::handleFloatNotEqual}},
                    {0x0800, {"__ltd", Code::CoreDllDoubleLessThan, &SyntheticDllRuntime::handleDoubleLessThan}},
                    {0x0801, {"__led", Code::CoreDllDoubleLessEqual, &SyntheticDllRuntime::handleDoubleLessEqual}},
                    {0x0802, {"__eqd", Code::CoreDllDoubleEqual, &SyntheticDllRuntime::handleDoubleEqual}},
                    {0x0803, {"__ged", Code::CoreDllDoubleGreaterEqual, &SyntheticDllRuntime::handleDoubleGreaterEqual}},
                    {0x0804, {"__gtd", Code::CoreDllDoubleGreaterThan, &SyntheticDllRuntime::handleDoubleGreaterThan}},
                },
            };
        }
    };

    const CoreDllMath math;
    registerHandlers(module, math.group());
}

bool SyntheticDllRuntime::handleAtoi(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = uint32_t(std::atoi(readAscii(args.a0, 256).c_str()));
    return true;
}

bool SyntheticDllRuntime::handleAtof(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string source = readAscii(args.a0, 256);
    char* end = nullptr;
    setGuestDoubleReturn(uc_, std::strtod(source.c_str(), &end), ret);
    return true;
}

bool SyntheticDllRuntime::handleAtan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, std::atan(doubleFromGuestPair(args.a0, args.a1)), ret);
    return true;
}

bool SyntheticDllRuntime::handleCos(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, std::cos(doubleFromGuestPair(args.a0, args.a1)), ret);
    return true;
}

bool SyntheticDllRuntime::handleHypot(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, std::hypot(doubleFromGuestPair(args.a0, args.a1),
                                         doubleFromGuestPair(args.a2, args.a3)), ret);
    return true;
}

bool SyntheticDllRuntime::handleRand(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = uint32_t(std::rand());
    return true;
}

bool SyntheticDllRuntime::handleSqrt(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, std::sqrt(doubleFromGuestPair(args.a0, args.a1)), ret);
    return true;
}

bool SyntheticDllRuntime::handleSrand(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    std::srand(args.a0);
    ret = 0;
    return true;
}

bool SyntheticDllRuntime::handleSin(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, std::sin(doubleFromGuestPair(args.a0, args.a1)), ret);
    return true;
}

bool SyntheticDllRuntime::handleStrtol(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string source = readAscii(args.a0);
    char* end = nullptr;
    const long value = std::strtol(source.c_str(), &end, int(args.a2 ? args.a2 : 10));
    if (args.a1) writeU32(args.a1, args.a0 + uint32_t(end ? end - source.c_str() : 0));
    ret = uint32_t(value);
    return true;
}

bool SyntheticDllRuntime::handleStrtoul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string source = readAscii(args.a0);
    char* end = nullptr;
    const unsigned long value = std::strtoul(source.c_str(), &end, int(args.a2));
    if (args.a1) writeU32(args.a1, args.a0 + uint32_t(end ? end - source.c_str() : 0));
    ret = uint32_t(value);
    spdlog::debug("strtoul source=\"{}\" base={} -> 0x{:08x}", source, args.a2, ret);
    return true;
}

bool SyntheticDllRuntime::handleWcstoul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const std::string source = readUtf16(args.a0);
    char* end = nullptr;
    const unsigned long value = std::strtoul(source.c_str(), &end, int(args.a2 ? args.a2 : 10));
    if (args.a1) writeU32(args.a1, args.a0 + uint32_t(end ? (end - source.c_str()) * 2 : 0));
    ret = uint32_t(value);
    spdlog::debug("wcstoul source=\"{}\" base={} -> 0x{:08x}", source, args.a2, ret);
    return true;
}

bool SyntheticDllRuntime::handleFmodf(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const float left = guestFloat(args.a0);
    const float right = guestFloat(args.a1);
    ret = guestFloatBits(right == 0.0f ? 0.0f : std::fmod(left, right));
    return true;
}

bool SyntheticDllRuntime::handleLlDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const int64_t dividend = (int64_t(uint64_t(args.a1)) << 32) | uint64_t(args.a0);
    const int64_t divisor = (int64_t(uint64_t(args.a3)) << 32) | uint64_t(args.a2);
    const int64_t quotient = divisor ? (dividend / divisor) : 0;
    ret = uint32_t(uint64_t(quotient));
    setReg(UC_MIPS_REG_V1, uint32_t(uint64_t(quotient) >> 32));
    return true;
}

bool SyntheticDllRuntime::handleFloatToLongLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const float value = guestFloat(args.a0);
    const int64_t converted = std::isfinite(value) ? static_cast<int64_t>(value) : 0;
    ret = uint32_t(uint64_t(converted));
    setReg(UC_MIPS_REG_V1, uint32_t(uint64_t(converted) >> 32));
    return true;
}

bool SyntheticDllRuntime::handleFloatAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloatBits(guestFloat(args.a0) + guestFloat(args.a1));
    return true;
}

bool SyntheticDllRuntime::handleDoubleAdd(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, doubleFromGuestPair(args.a0, args.a1) + doubleFromGuestPair(args.a2, args.a3), ret);
    return true;
}

bool SyntheticDllRuntime::handleFloatSub(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloatBits(guestFloat(args.a0) - guestFloat(args.a1));
    return true;
}

bool SyntheticDllRuntime::handleDoubleSub(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, doubleFromGuestPair(args.a0, args.a1) - doubleFromGuestPair(args.a2, args.a3), ret);
    return true;
}

bool SyntheticDllRuntime::handleFloatMul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloatBits(guestFloat(args.a0) * guestFloat(args.a1));
    return true;
}

bool SyntheticDllRuntime::handleDoubleMul(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, doubleFromGuestPair(args.a0, args.a1) * doubleFromGuestPair(args.a2, args.a3), ret);
    return true;
}

bool SyntheticDllRuntime::handleFloatDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const float right = guestFloat(args.a1);
    ret = guestFloatBits(right == 0.0f ? 0.0f : guestFloat(args.a0) / right);
    return true;
}

bool SyntheticDllRuntime::handleDoubleDiv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const double right = doubleFromGuestPair(args.a2, args.a3);
    setGuestDoubleReturn(uc_, right == 0.0 ? 0.0 : doubleFromGuestPair(args.a0, args.a1) / right, ret);
    return true;
}

bool SyntheticDllRuntime::handleFloatToLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = uint32_t(int32_t(guestFloat(args.a0)));
    return true;
}

bool SyntheticDllRuntime::handleFloatToUnsignedLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = uint32_t(guestFloat(args.a0));
    return true;
}

bool SyntheticDllRuntime::handleDoubleToLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = uint32_t(int32_t(doubleFromGuestPair(args.a0, args.a1)));
    return true;
}

bool SyntheticDllRuntime::handleDoubleToUnsignedLong(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = uint32_t(doubleFromGuestPair(args.a0, args.a1));
    return true;
}

bool SyntheticDllRuntime::handleLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, static_cast<double>(int32_t(args.a0)), ret);
    return true;
}

bool SyntheticDllRuntime::handleUnsignedLongToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, static_cast<double>(args.a0), ret);
    return true;
}

bool SyntheticDllRuntime::handleFloatToDouble(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    setGuestDoubleReturn(uc_, static_cast<double>(guestFloat(args.a0)), ret);
    return true;
}

bool SyntheticDllRuntime::handleDoubleToFloat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloatBits(static_cast<float>(doubleFromGuestPair(args.a0, args.a1)));
    return true;
}

bool SyntheticDllRuntime::handleLongToFloat(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloatBits(static_cast<float>(int32_t(args.a0)));
    return true;
}

bool SyntheticDllRuntime::handleFloatLessThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) < guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleFloatLessEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) <= guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleFloatEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) == guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleFloatGreaterEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) >= guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleFloatGreaterThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) > guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleFloatNotEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = guestFloat(args.a0) != guestFloat(args.a1) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleDoubleLessThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = doubleFromGuestPair(args.a0, args.a1) < doubleFromGuestPair(args.a2, args.a3) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleDoubleLessEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = doubleFromGuestPair(args.a0, args.a1) <= doubleFromGuestPair(args.a2, args.a3) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleDoubleEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = doubleFromGuestPair(args.a0, args.a1) == doubleFromGuestPair(args.a2, args.a3) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleDoubleGreaterEqual(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = doubleFromGuestPair(args.a0, args.a1) >= doubleFromGuestPair(args.a2, args.a3) ? 1 : 0;
    return true;
}

bool SyntheticDllRuntime::handleDoubleGreaterThan(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = doubleFromGuestPair(args.a0, args.a1) > doubleFromGuestPair(args.a2, args.a3) ? 1 : 0;
    return true;
}
