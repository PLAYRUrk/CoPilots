#pragma once
// Transport.h — cross-platform TCP + UDP socket abstraction (Winsock / BSD).
// Non-blocking, no exceptions. Returns bool success and errno-style error.

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

// ── Initialisation ────────────────────────────────────────────────────────
bool InitNetwork();   // call once; inits Winsock on Windows
void ShutdownNetwork();

// ── TCP helpers ───────────────────────────────────────────────────────────

// Listening TCP socket (server side)
SocketHandle TcpListen(uint16_t port, int backlog = 8);

// Accept a pending connection (non-blocking; returns INVALID_SOCK if none)
SocketHandle TcpAccept(SocketHandle listener);

// Connect (blocking with timeout_ms; returns INVALID_SOCK on failure)
SocketHandle TcpConnect(const std::string& host, uint16_t port, int timeout_ms = 5000);

// Set socket to non-blocking mode
bool SetNonBlocking(SocketHandle sock);

// Send all bytes (blocks until done or error)
bool TcpSendAll(SocketHandle sock, const uint8_t* data, size_t len);

// Recv into buffer; returns bytes read (0 = closed, -1 = would-block, -2 = error)
int TcpRecv(SocketHandle sock, uint8_t* buf, int bufLen);

void CloseSocket(SocketHandle sock);

// ── UDP helpers ───────────────────────────────────────────────────────────

struct UdpEndpoint {
    std::string ip;
    uint16_t    port = 0;
};

// Bind a UDP socket for receiving on a port
SocketHandle UdpBind(uint16_t port);

// Send a UDP datagram to destination
bool UdpSendTo(SocketHandle sock, const uint8_t* data, size_t len,
               const UdpEndpoint& to);

// Recv a UDP datagram; returns bytes (0 = nothing, -1 = error); fills from
int UdpRecvFrom(SocketHandle sock, uint8_t* buf, int bufLen, UdpEndpoint& from);

// ── TCP stream re-assembly ─────────────────────────────────────────────────
// Accumulates raw bytes and extracts complete framed messages.
// Frame format: [uint8 type][uint32 payload_len LE][payload...]
class TcpFramer {
public:
    // Feed raw bytes; cb called for each complete message
    // cb receives: (type byte, payload ptr, payload_len)
    using MessageCb = std::function<void(uint8_t, const uint8_t*, uint32_t)>;
    void feed(const uint8_t* data, size_t len, MessageCb cb);
    void reset() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

} // namespace net
} // namespace cp
