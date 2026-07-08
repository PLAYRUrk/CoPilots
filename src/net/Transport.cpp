#include "Transport.h"
#include "../Log.h"

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <errno.h>
  #include <string.h>
  #define SOCKET_ERROR (-1)
#endif

namespace cp {
namespace net {

bool InitNetwork()
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        LogError("WSAStartup failed");
        return false;
    }
    Log("Winsock 2.2 initialised");
#endif
    return true;
}

void ShutdownNetwork()
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static int LastError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static bool WouldBlock()
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool SetNonBlocking(SocketHandle sock)
{
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

void CloseSocket(SocketHandle sock)
{
    if (sock == INVALID_SOCK) return;
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

SocketHandle TcpListen(uint16_t port, int backlog)
{
    SocketHandle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCK) {
        LogError("TcpListen: socket() failed err=%d", LastError());
        return INVALID_SOCK;
    }

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LogError("TcpListen: bind() port %u failed err=%d", port, LastError());
        CloseSocket(s);
        return INVALID_SOCK;
    }
    if (listen(s, backlog) == SOCKET_ERROR) {
        LogError("TcpListen: listen() failed err=%d", LastError());
        CloseSocket(s);
        return INVALID_SOCK;
    }
    SetNonBlocking(s);
    Log("TCP listening on port %u", port);
    return s;
}

SocketHandle TcpAccept(SocketHandle listener)
{
    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = sizeof(addr);
#else
    socklen_t addrLen = sizeof(addr);
#endif
    SocketHandle c = accept(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (c == INVALID_SOCK) return INVALID_SOCK;

    SetNonBlocking(c);

    char ipbuf[64] = "?";
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    Log("Accepted TCP connection from %s:%u", ipbuf, ntohs(addr.sin_port));
    return c;
}

SocketHandle TcpConnect(const std::string& host, uint16_t port, int timeout_ms)
{
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", port);

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        LogError("TcpConnect: getaddrinfo(%s) failed", host.c_str());
        return INVALID_SOCK;
    }

    SocketHandle s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCK) { freeaddrinfo(res); return INVALID_SOCK; }

    SetNonBlocking(s);
    connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    fd_set wset, eset;
    FD_ZERO(&wset); FD_ZERO(&eset);
    FD_SET(s, &wset); FD_SET(s, &eset);
    timeval tv{};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int sel = select(static_cast<int>(s)+1, nullptr, &wset, &eset, &tv);
    if (sel <= 0 || FD_ISSET(s, &eset)) {
        LogError("TcpConnect: connect to %s:%u timed out or failed", host.c_str(), port);
        CloseSocket(s);
        return INVALID_SOCK;
    }
    int err = 0;
#ifdef _WIN32
    int len = sizeof(err);
#else
    socklen_t len = sizeof(err);
#endif
    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0) {
        LogError("TcpConnect: SO_ERROR=%d", err);
        CloseSocket(s);
        return INVALID_SOCK;
    }
    Log("TCP connected to %s:%u", host.c_str(), port);
    return s;
}

bool TcpSendAll(SocketHandle sock, const uint8_t* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, reinterpret_cast<const char*>(data+sent),
                     static_cast<int>(len-sent), 0);
        if (n == SOCKET_ERROR) {
            if (WouldBlock()) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

int TcpRecv(SocketHandle sock, uint8_t* buf, int bufLen)
{
    int n = recv(sock, reinterpret_cast<char*>(buf), bufLen, 0);
    if (n == 0) return 0;
    if (n < 0) {
        if (WouldBlock()) return -1;
        return -2;
    }
    return n;
}

SocketHandle UdpBind(uint16_t port)
{
    SocketHandle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK) {
        LogError("UdpBind: socket() failed err=%d", LastError());
        return INVALID_SOCK;
    }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LogError("UdpBind: bind() port %u failed err=%d", port, LastError());
        CloseSocket(s);
        return INVALID_SOCK;
    }
    SetNonBlocking(s);
    return s;
}

bool UdpSendTo(SocketHandle sock, const uint8_t* data, size_t len,
               const UdpEndpoint& to)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(to.port);
    inet_pton(AF_INET, to.ip.c_str(), &addr.sin_addr);
    int n = sendto(sock, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                   reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(len);
}

int UdpRecvFrom(SocketHandle sock, uint8_t* buf, int bufLen, UdpEndpoint& from)
{
    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = sizeof(addr);
#else
    socklen_t addrLen = sizeof(addr);
#endif
    int n = recvfrom(sock, reinterpret_cast<char*>(buf), bufLen, 0,
                     reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (n <= 0) return (WouldBlock() ? 0 : -1);
    char ipbuf[64];
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    from.ip   = ipbuf;
    from.port = ntohs(addr.sin_port);
    return n;
}

void TcpFramer::feed(const uint8_t* data, size_t len, MessageCb cb)
{
    buf_.insert(buf_.end(), data, data + len);

    while (buf_.size() >= 5) {
        uint32_t plen =  static_cast<uint32_t>(buf_[1])
                      | (static_cast<uint32_t>(buf_[2]) <<  8)
                      | (static_cast<uint32_t>(buf_[3]) << 16)
                      | (static_cast<uint32_t>(buf_[4]) << 24);
        if (plen > 1024*1024) {
            buf_.clear();
            LogError("TcpFramer: oversized payload %u — resetting", plen);
            return;
        }
        if (buf_.size() < 5 + plen) break;

        uint8_t type = buf_[0];
        cb(type, buf_.data() + 5, plen);
        buf_.erase(buf_.begin(), buf_.begin() + 5 + plen);
    }
}

}
}
