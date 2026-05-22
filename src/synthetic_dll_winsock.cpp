#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "synthetic_dll.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
bool sameModuleName(std::string name, std::string_view wanted) {
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    return name == wanted;
}
}

std::optional<SyntheticModule> SyntheticDllRuntime::createWinsock(const std::string& dllName) {
    struct WinsockDll {
        explicit WinsockDll(const std::string& requestedName)
            : name(sameModuleName(requestedName, "ws2.dll") ? "WS2.dll" : "WINSOCK.dll") {}

        SyntheticDllSpec spec() const {
            using Code = SyntheticExportCode;
            return SyntheticDllSpec{
                name,
                128,
                {
                    {0x0001, {"WSACleanup", Code::WinsockCleanup, &SyntheticDllRuntime::handleWinsockCleanup}},
                    {0x0003, {"WSAStartup", Code::WinsockStartup, &SyntheticDllRuntime::handleWinsockStartup}},
                    {0x0005, {"__WSAFDIsSet", Code::WinsockFdIsSet, &SyntheticDllRuntime::handleWinsockFdIsSet}},
                    {0x0006, {"accept", Code::WinsockAccept, &SyntheticDllRuntime::handleWinsockAccept}},
                    {0x0007, {"bind", Code::WinsockBind, &SyntheticDllRuntime::handleWinsockBind}},
                    {0x0008, {"closesocket", Code::WinsockCloseSocket, &SyntheticDllRuntime::handleWinsockCloseSocket}},
                    {0x0009, {"connect", Code::WinsockConnect, &SyntheticDllRuntime::handleWinsockConnect}},
                    {0x000B, {"gethostbyname", Code::WinsockGetHostByName, &SyntheticDllRuntime::handleWinsockGetHostByName}},
                    {0x000C, {"gethostname", Code::WinsockGetHostName, &SyntheticDllRuntime::handleWinsockGetHostName}},
                    {0x0010, {"htonl", Code::WinsockHtonl, &SyntheticDllRuntime::handleWinsockHtonl}},
                    {0x0011, {"htons", Code::WinsockHtons, &SyntheticDllRuntime::handleWinsockHtons}},
                    {0x0012, {"inet_addr", Code::WinsockInetAddr, &SyntheticDllRuntime::handleWinsockInetAddr}},
                    {0x0013, {"inet_ntoa", Code::WinsockInetNtoa, &SyntheticDllRuntime::handleWinsockInetNtoa}},
                    {0x0014, {"ioctlsocket", Code::WinsockIoctlSocket, &SyntheticDllRuntime::handleWinsockIoctlSocket}},
                    {0x0015, {"listen", Code::WinsockListen, &SyntheticDllRuntime::handleWinsockListen}},
                    {0x0016, {"ntohl", Code::WinsockNtohl, &SyntheticDllRuntime::handleWinsockNtohl}},
                    {0x0017, {"ntohs", Code::WinsockNtohs, &SyntheticDllRuntime::handleWinsockNtohs}},
                    {0x0018, {"recv", Code::WinsockRecv, &SyntheticDllRuntime::handleWinsockRecv}},
                    {0x001A, {"select", Code::WinsockSelect, &SyntheticDllRuntime::handleWinsockSelect}},
                    {0x001B, {"send", Code::WinsockSend, &SyntheticDllRuntime::handleWinsockSend}},
                    {0x001E, {"setsockopt", Code::WinsockSetSockOpt, &SyntheticDllRuntime::handleWinsockSetSockOpt}},
                    {0x0020, {"socket", Code::WinsockSocket, &SyntheticDllRuntime::handleWinsockSocket}},
                },
            };
        }

        const char* name;
    };

    const WinsockDll winsock(dllName);
    return createGenericOrdinalDll(winsock.spec());
}

bool SyntheticDllRuntime::handleWinsockStartup(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    WSADATA data{};
    ret = WSAStartup(MAKEWORD(uint8_t(args.a0), uint8_t(args.a0 >> 8)), &data);
    if (args.a1) {
        std::array<uint8_t, 400> guest{};
        std::memcpy(guest.data(), &data, std::min(sizeof(data), guest.size()));
        uc_mem_write(uc_, args.a1, guest.data(), guest.size());
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockCleanup(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    (void)args;
    ret = WSACleanup();
    if (ret) lastError_ = WSAGetLastError();
    return true;
}

bool SyntheticDllRuntime::handleWinsockSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    const SOCKET host = ::socket(int(args.a0), int(args.a1), int(args.a2));
    if (host == INVALID_SOCKET) {
        lastError_ = WSAGetLastError();
        ret = 0xffffffffu;
    } else {
        ret = makeGuestHandle({GuestHandle::Kind::HostSocket, uintptr_t(host), 0});
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockCloseSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* handle = lookupGuestHandle(args.a0);
    if (!handle || handle->kind != GuestHandle::Kind::HostSocket) {
        lastError_ = WSAENOTSOCK;
        ret = 0xffffffffu;
    } else {
        ret = closesocket(SOCKET(handle->hostValue));
        guestHandles_.erase(args.a0);
        lastError_ = ret ? WSAGetLastError() : 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockConnect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    sockaddr_storage storage{};
    int length = int(args.a2);
    if (s == INVALID_SOCKET || !args.a1 || !args.a2 || args.a2 > sizeof(storage) ||
        uc_mem_read(uc_, args.a1, &storage, args.a2) != UC_ERR_OK) {
        lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
        ret = 0xffffffffu;
    } else {
        ret = ::connect(s, reinterpret_cast<sockaddr*>(&storage), length);
        lastError_ = ret ? WSAGetLastError() : 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockBind(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    sockaddr_storage storage{};
    int length = int(args.a2);
    if (s == INVALID_SOCKET || !args.a1 || !args.a2 || args.a2 > sizeof(storage) ||
        uc_mem_read(uc_, args.a1, &storage, args.a2) != UC_ERR_OK) {
        lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
        ret = 0xffffffffu;
    } else {
        ret = ::bind(s, reinterpret_cast<sockaddr*>(&storage), length);
        lastError_ = ret ? WSAGetLastError() : 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockListen(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    ret = s == INVALID_SOCKET ? SOCKET_ERROR : ::listen(s, int(args.a1));
    if (ret) lastError_ = WSAGetLastError();
    return true;
}

bool SyntheticDllRuntime::handleWinsockAccept(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    sockaddr_storage storage{};
    int length = args.a2 ? int(readU32(args.a2)) : sizeof(storage);
    const SOCKET accepted = s == INVALID_SOCKET
        ? INVALID_SOCKET
        : ::accept(s, reinterpret_cast<sockaddr*>(&storage), args.a1 ? &length : nullptr);
    if (accepted == INVALID_SOCKET) {
        lastError_ = WSAGetLastError();
        ret = 0xffffffffu;
    } else {
        if (args.a1 && args.a2 && length > 0) {
            uc_mem_write(uc_, args.a1, &storage, size_t(length));
            writeU32(args.a2, uint32_t(length));
        }
        ret = makeGuestHandle({GuestHandle::Kind::HostSocket, uintptr_t(accepted), 0});
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockRecv(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    if (s == INVALID_SOCKET || (args.a2 && !args.a1)) {
        lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
        ret = 0xffffffffu;
    } else {
        std::vector<char> bytes(args.a2);
        const int got = ::recv(s, bytes.data(), int(bytes.size()), int(args.a3));
        if (got >= 0 && got) uc_mem_write(uc_, args.a1, bytes.data(), size_t(got));
        ret = got < 0 ? 0xffffffffu : uint32_t(got);
        lastError_ = got < 0 ? WSAGetLastError() : 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockSend(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    if (s == INVALID_SOCKET || (args.a2 && !args.a1)) {
        lastError_ = s == INVALID_SOCKET ? WSAENOTSOCK : WSAEFAULT;
        ret = 0xffffffffu;
    } else {
        std::vector<char> bytes(args.a2);
        if (args.a2) uc_mem_read(uc_, args.a1, bytes.data(), bytes.size());
        const int sent = ::send(s, bytes.data(), int(bytes.size()), int(args.a3));
        ret = sent < 0 ? 0xffffffffu : uint32_t(sent);
        lastError_ = sent < 0 ? WSAGetLastError() : 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockIoctlSocket(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    u_long value = args.a2 ? readU32(args.a2) : 0;
    ret = s == INVALID_SOCKET ? SOCKET_ERROR : ::ioctlsocket(s, long(args.a1), &value);
    if (args.a2) writeU32(args.a2, uint32_t(value));
    if (ret) lastError_ = WSAGetLastError();
    return true;
}

bool SyntheticDllRuntime::handleWinsockSetSockOpt(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto* guest = lookupGuestHandle(args.a0);
    const SOCKET s = guest && guest->kind == GuestHandle::Kind::HostSocket
        ? SOCKET(guest->hostValue)
        : INVALID_SOCKET;
    const uint32_t optLen = stackArg(4);
    std::vector<char> value(optLen);
    if (args.a3 && optLen) uc_mem_read(uc_, args.a3, value.data(), value.size());
    ret = s == INVALID_SOCKET
        ? SOCKET_ERROR
        : ::setsockopt(s, int(args.a1), int(args.a2), value.data(), int(value.size()));
    if (ret) lastError_ = WSAGetLastError();
    return true;
}

bool SyntheticDllRuntime::handleWinsockSelect(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    auto guestSocket = [&](uint32_t handle) -> SOCKET {
        auto* guest = lookupGuestHandle(handle);
        return guest && guest->kind == GuestHandle::Kind::HostSocket
            ? SOCKET(guest->hostValue)
            : INVALID_SOCKET;
    };
    auto buildSet = [&](uint32_t ptr, fd_set& host, std::vector<uint32_t>& guestHandles) {
        FD_ZERO(&host);
        guestHandles.clear();
        if (!ptr) return;
        const uint32_t count = std::min<uint32_t>(readU32(ptr), FD_SETSIZE);
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t guest = readU32(ptr + 4 + i * 4);
            const SOCKET s = guestSocket(guest);
            if (s != INVALID_SOCKET) {
                FD_SET(s, &host);
                guestHandles.push_back(guest);
            }
        }
    };
    auto writeSet = [&](uint32_t ptr, const fd_set& host, const std::vector<uint32_t>& guestHandles) {
        if (!ptr) return;
        uint32_t count = 0;
        for (uint32_t guest : guestHandles) {
            const SOCKET s = guestSocket(guest);
            if (s != INVALID_SOCKET && FD_ISSET(s, const_cast<fd_set*>(&host))) {
                writeU32(ptr + 4 + count * 4, guest);
                ++count;
            }
        }
        writeU32(ptr, count);
    };
    fd_set readSet{}, writeSetHost{}, exceptSet{};
    std::vector<uint32_t> readGuest, writeGuest, exceptGuest;
    buildSet(args.a1, readSet, readGuest);
    buildSet(args.a2, writeSetHost, writeGuest);
    buildSet(args.a3, exceptSet, exceptGuest);
    timeval timeout{};
    timeval* timeoutPtr = nullptr;
    const uint32_t timeoutGuest = stackArg(4);
    if (timeoutGuest) {
        timeout.tv_sec = long(readU32(timeoutGuest));
        timeout.tv_usec = long(readU32(timeoutGuest + 4));
        timeoutPtr = &timeout;
    }
    const int selected = ::select(0, args.a1 ? &readSet : nullptr, args.a2 ? &writeSetHost : nullptr,
                                  args.a3 ? &exceptSet : nullptr, timeoutPtr);
    if (selected == SOCKET_ERROR) {
        lastError_ = WSAGetLastError();
        ret = 0xffffffffu;
    } else {
        writeSet(args.a1, readSet, readGuest);
        writeSet(args.a2, writeSetHost, writeGuest);
        writeSet(args.a3, exceptSet, exceptGuest);
        ret = uint32_t(selected);
        lastError_ = 0;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockFdIsSet(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = 0;
    if (args.a1) {
        const uint32_t count = readU32(args.a1);
        for (uint32_t i = 0; i < count; ++i) {
            if (readU32(args.a1 + 4 + i * 4) == args.a0) {
                ret = 1;
                break;
            }
        }
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockHtonl(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = htonl(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleWinsockNtohl(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = ntohl(args.a0);
    return true;
}

bool SyntheticDllRuntime::handleWinsockHtons(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = htons(uint16_t(args.a0));
    return true;
}

bool SyntheticDllRuntime::handleWinsockNtohs(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = ntohs(uint16_t(args.a0));
    return true;
}

bool SyntheticDllRuntime::handleWinsockInetAddr(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    ret = inet_addr(readAscii(args.a0).c_str());
    return true;
}

bool SyntheticDllRuntime::handleWinsockInetNtoa(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    in_addr addr{};
    addr.S_un.S_addr = args.a0;
    const char* text = inet_ntoa(addr);
    ret = allocate(uint32_t(std::strlen(text ? text : "") + 1), true);
    writeAscii(ret, text ? text : "");
    return true;
}

bool SyntheticDllRuntime::handleWinsockGetHostName(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    std::vector<char> buffer(args.a1 ? args.a1 : 1);
    const int ok = ::gethostname(buffer.data(), int(buffer.size()));
    if (!ok) {
        size_t copyLen = 0;
        while (copyLen < buffer.size() && buffer[copyLen]) ++copyLen;
        if (copyLen < buffer.size()) ++copyLen;
        if (args.a0 && args.a1) uc_mem_write(uc_, args.a0, buffer.data(), copyLen);
        ret = 0;
        lastError_ = 0;
    } else {
        lastError_ = WSAGetLastError();
        ret = 0xffffffffu;
    }
    return true;
}

bool SyntheticDllRuntime::handleWinsockGetHostByName(SyntheticExportCode code, const GuestCallArgs& args, uint32_t& ret) {
    (void)code;
    hostent* host = ::gethostbyname(readAscii(args.a0).c_str());
    if (!host || !host->h_addr_list || !host->h_addr_list[0]) {
        lastError_ = WSAGetLastError();
        ret = 0;
    } else {
        const std::string hostName = host->h_name ? host->h_name : readAscii(args.a0);
        const uint32_t namePtr = allocate(uint32_t(hostName.size() + 1), true);
        writeAscii(namePtr, hostName);
        const uint32_t aliasesPtr = allocate(4, true);
        const uint32_t addrPtr = allocate(4, false);
        uc_mem_write(uc_, addrPtr, host->h_addr_list[0], 4);
        const uint32_t addrListPtr = allocate(8, true);
        writeU32(addrListPtr, addrPtr);
        ret = allocate(16, true);
        writeU32(ret, namePtr);
        writeU32(ret + 4, aliasesPtr);
        uint16_t family = uint16_t(host->h_addrtype);
        uint16_t length = uint16_t(host->h_length);
        uc_mem_write(uc_, ret + 8, &family, sizeof(family));
        uc_mem_write(uc_, ret + 10, &length, sizeof(length));
        writeU32(ret + 12, addrListPtr);
        lastError_ = 0;
    }
    return true;
}
