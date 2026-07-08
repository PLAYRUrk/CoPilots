#pragma once
// NetThread.h — background network I/O thread with thread-safe message queues.
// All socket operations happen here; main thread exchanges messages via InQueue/OutQueue.

#include "Transport.h"
#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <string>

namespace cp {
namespace net {

// A framed TCP message (already de-framed by TcpFramer)
struct InboundMsg {
    uint8_t              sender = 0;    // participant id (0 = server, on client side)
    uint8_t              type   = 0;
    std::vector<uint8_t> payload;
};

// An outbound TCP message (raw framed bytes, ready to send)
struct OutboundMsg {
    uint8_t              target = 0xFF; // 0xFF = broadcast (server only)
    std::vector<uint8_t> frame;         // includes header
};

// Thread-safe queue
template<typename T>
class SafeQueue {
public:
    void push(T item) {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(item));
    }
    bool pop(T& out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    bool empty() const {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(mu_));
        return q_.empty();
    }
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear();
    }
private:
    std::deque<T> q_;
    std::mutex    mu_;
};

// UDP datagram
struct UdpDatagram {
    std::vector<uint8_t> data;
    UdpEndpoint          from;
    UdpEndpoint          to;   // for outbound
};

// ── NetThread — runs in its own thread ────────────────────────────────────
// Usage:
//   NetThread t;
//   t.startServer(port);   OR   t.startClient(host, port);
//   // from main thread:
//   t.outTcp.push(msg);     // send
//   t.inTcp.pop(msg);       // receive
//   t.outUdp.push(dgram);   // send UDP
//   t.inUdp.pop(dgram);     // receive UDP

class NetThread {
public:
    SafeQueue<InboundMsg>  inTcp;
    SafeQueue<OutboundMsg> outTcp;
    SafeQueue<UdpDatagram> inUdp;
    SafeQueue<UdpDatagram> outUdp;

    // Atomic status flags (readable from main thread)
    std::atomic<bool> connected  {false};
    std::atomic<bool> running    {false};
    std::atomic<bool> hasError   {false};
    std::string       lastError;   // set before hasError

    bool startServer(uint16_t tcpPort, uint16_t udpPort);
    bool startClient(const std::string& host, uint16_t tcpPort, uint16_t udpPort);
    void stop();

    ~NetThread() { stop(); }

private:
    void serverLoop(uint16_t tcpPort, uint16_t udpPort);
    void clientLoop(const std::string& host, uint16_t tcpPort, uint16_t udpPort);

    std::thread               thread_;
    std::atomic<bool>         stopFlag_{false};

    // Server state (accessed only from net thread)
    struct ClientConn {
        uint8_t       id = 0;
        SocketHandle  sock = INVALID_SOCK;
        TcpFramer     framer;
        UdpEndpoint   udpEp;  // filled after first UDP packet from this client
    };

    bool isServer_ = false;
};

} // namespace net
} // namespace cp
