#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  constexpr SocketHandle INVALID_SOCK = INVALID_SOCKET;
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  using SocketHandle = int;
  constexpr SocketHandle INVALID_SOCK = -1;
#endif

namespace cp {
namespace net {

bool InitNetwork();
void ShutdownNetwork();

SocketHandle TcpListen(uint16_t port, int backlog = 8);

SocketHandle TcpAccept(SocketHandle listener);

SocketHandle TcpConnect(const std::string& host, uint16_t port, int timeout_ms = 5000);

bool SetNonBlocking(SocketHandle sock);

bool TcpSendAll(SocketHandle sock, const uint8_t* data, size_t len);

int TcpRecv(SocketHandle sock, uint8_t* buf, int bufLen);

void CloseSocket(SocketHandle sock);

struct UdpEndpoint {
    std::string ip;
    uint16_t    port = 0;
};

SocketHandle UdpBind(uint16_t port);

bool UdpSendTo(SocketHandle sock, const uint8_t* data, size_t len,
               const UdpEndpoint& to);

int UdpRecvFrom(SocketHandle sock, uint8_t* buf, int bufLen, UdpEndpoint& from);

class TcpFramer {
public:
    using MessageCb = std::function<void(uint8_t, const uint8_t*, uint32_t)>;
    void feed(const uint8_t* data, size_t len, MessageCb cb);
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

}
}
