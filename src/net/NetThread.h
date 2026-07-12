#pragma once

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

struct InboundMsg {
    uint8_t              sender = 0;
    uint8_t              type   = 0;
    std::vector<uint8_t> payload;
};

struct OutboundMsg {
    uint8_t              target = 0xFF;
    // When target==0xFF (broadcast), skip the client with this connId.
    // Set to msg.sender on host relays so the originating client does not
    // receive its own message back (prevents three-position switch overshoot).
    // 0 means no exclusion (all clients receive the broadcast).
    uint8_t              excludeTarget = 0;
    std::vector<uint8_t> frame;
};

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

struct UdpDatagram {
    std::vector<uint8_t> data;
    UdpEndpoint          from;
    UdpEndpoint          to;
};

class NetThread {
public:
    SafeQueue<InboundMsg>  inTcp;
    SafeQueue<OutboundMsg> outTcp;
    SafeQueue<UdpDatagram> inUdp;
    SafeQueue<UdpDatagram> outUdp;
    // main thread pushes {connId, endpoint} after learning client UDP address
    SafeQueue<std::pair<uint8_t, UdpEndpoint>> clientUdpEpUpdates;

    std::atomic<bool> connected  {false};
    std::atomic<bool> running    {false};
    std::atomic<bool> hasError   {false};
    std::string       lastError;

    // bindIp: optional local interface IP the server sockets bind to (empty =
    // all interfaces). Lets the host bypass an active VPN — see Transport.h.
    bool startServer(uint16_t tcpPort, uint16_t udpPort, const std::string& bindIp = {});
    bool startClient(const std::string& host, uint16_t tcpPort, uint16_t udpPort);
    void stop();

    ~NetThread() { stop(); }

private:
    void serverLoop(uint16_t tcpPort, uint16_t udpPort, const std::string& bindIp);
    void clientLoop(const std::string& host, uint16_t tcpPort, uint16_t udpPort);

    std::thread               thread_;
    std::atomic<bool>         stopFlag_{false};

    struct ClientConn {
        uint8_t       id = 0;
        SocketHandle  sock = INVALID_SOCK;
        TcpFramer     framer;
        UdpEndpoint   udpEp;
        std::string   tcpIp;  // source IP from the TCP connection (for ANNOUNCE matching)
    };

    bool isServer_ = false;
};

}
}
