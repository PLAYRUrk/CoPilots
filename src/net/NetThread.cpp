#include "NetThread.h"
#include "../Log.h"
#include <algorithm>
#include <cstring>
#include <chrono>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
#endif

namespace cp {
namespace net {

static uint64_t NowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

bool NetThread::startServer(uint16_t tcpPort, uint16_t udpPort)
{
    isServer_ = true;
    stopFlag_ = false;
    running   = false;
    connected = false;
    hasError  = false;

    thread_ = std::thread([this, tcpPort, udpPort]() {
        serverLoop(tcpPort, udpPort);
    });
    return true;
}

bool NetThread::startClient(const std::string& host, uint16_t tcpPort, uint16_t udpPort)
{
    isServer_ = false;
    stopFlag_ = false;
    running   = false;
    connected = false;
    hasError  = false;

    thread_ = std::thread([this, host, tcpPort, udpPort]() {
        clientLoop(host, tcpPort, udpPort);
    });
    return true;
}

void NetThread::stop()
{
    stopFlag_ = true;
    if (thread_.joinable()) thread_.join();
    running = false;
    connected = false;
}

void NetThread::serverLoop(uint16_t tcpPort, uint16_t udpPort)
{
    running = true;
    Log("NetThread(server): starting on TCP:%u UDP:%u", tcpPort, udpPort);

    SocketHandle listener = TcpListen(tcpPort);
    if (listener == INVALID_SOCK) {
        lastError = "Failed to bind TCP port";
        hasError  = true;
        running   = false;
        return;
    }
    SocketHandle udpSock = UdpBind(udpPort);
    if (udpSock == INVALID_SOCK) {
        lastError = "Failed to bind UDP port";
        hasError  = true;
        CloseSocket(listener);
        running   = false;
        return;
    }

    connected = true;

    std::vector<ClientConn> clients;
    uint8_t nextId = 1;

    uint8_t tcpBuf[4096];
    uint8_t udpBuf[4096];

    uint64_t lastHb = NowMs();

    while (!stopFlag_) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(listener, &rset);
        FD_SET(udpSock, &rset);
        int maxfd = static_cast<int>(listener > udpSock ? listener : udpSock);
        for (auto& c : clients) {
            FD_SET(c.sock, &rset);
            if (static_cast<int>(c.sock) > maxfd) maxfd = static_cast<int>(c.sock);
        }
        timeval tv{0, 10000};
        select(maxfd+1, &rset, nullptr, nullptr, &tv);

        if (FD_ISSET(listener, &rset)) {
            SocketHandle cs = TcpAccept(listener);
            if (cs != INVALID_SOCK) {
                ClientConn cc;
                cc.id   = nextId++;
                cc.sock = cs;
                clients.push_back(std::move(cc));
                Log("NetThread(server): client %u connected", clients.back().id);
            }
        }

        if (FD_ISSET(udpSock, &rset)) {
            UdpEndpoint from;
            int n = UdpRecvFrom(udpSock, udpBuf, sizeof(udpBuf), from);
            if (n > 0) {
                UdpDatagram dg;
                dg.data.assign(udpBuf, udpBuf+n);
                dg.from = from;
                inUdp.push(std::move(dg));

                if (n >= 1) {
                    for (auto& c : clients) {
                        if (c.udpEp.ip.empty()) {
                            c.udpEp = from;
                        }
                    }
                }
            }
        }

        std::vector<size_t> toRemove;
        for (size_t i = 0; i < clients.size(); ++i) {
            auto& c = clients[i];
            if (!FD_ISSET(c.sock, &rset)) continue;
            int n = TcpRecv(c.sock, tcpBuf, sizeof(tcpBuf));
            if (n == 0 || n == -2) {
                Log("NetThread(server): client %u disconnected", c.id);
                InboundMsg disc;
                disc.sender = c.id;
                disc.type   = 0xFE;
                inTcp.push(std::move(disc));
                CloseSocket(c.sock);
                toRemove.push_back(i);
            } else if (n > 0) {
                const uint8_t cid = c.id;
                c.framer.feed(tcpBuf, static_cast<size_t>(n),
                    [&](uint8_t type, const uint8_t* payload, uint32_t plen) {
                        InboundMsg msg;
                        msg.sender = cid;
                        msg.type   = type;
                        msg.payload.assign(payload, payload+plen);
                        inTcp.push(std::move(msg));
                    });
            }
        }
        for (int ri = static_cast<int>(toRemove.size())-1; ri >= 0; --ri)
            clients.erase(clients.begin() + toRemove[ri]);

        OutboundMsg out;
        while (outTcp.pop(out)) {
            if (out.target == 0xFF) {
                for (auto& c : clients)
                    TcpSendAll(c.sock, out.frame.data(), out.frame.size());
            } else {
                for (auto& c : clients) {
                    if (c.id == out.target)
                        TcpSendAll(c.sock, out.frame.data(), out.frame.size());
                }
            }
        }

        UdpDatagram dg;
        while (outUdp.pop(dg)) {
            UdpSendTo(udpSock, dg.data.data(), dg.data.size(), dg.to);
        }

        uint64_t now = NowMs();
        if (now - lastHb > 1000) {
            lastHb = now;
            uint8_t hb[5] = {0xF0, 0,0,0,0};
            for (auto& c : clients)
                TcpSendAll(c.sock, hb, sizeof(hb));
        }
    }

    for (auto& c : clients) CloseSocket(c.sock);
    CloseSocket(listener);
    CloseSocket(udpSock);
    connected = false;
    running   = false;
    Log("NetThread(server): stopped");
}

void NetThread::clientLoop(const std::string& host, uint16_t tcpPort, uint16_t udpPort)
{
    running = true;
    Log("NetThread(client): connecting to %s TCP:%u UDP:%u", host.c_str(), tcpPort, udpPort);

    SocketHandle tcpSock = TcpConnect(host, tcpPort, 5000);
    if (tcpSock == INVALID_SOCK) {
        lastError = "Cannot connect to server";
        hasError  = true;
        running   = false;
        return;
    }

    SocketHandle udpSock = UdpBind(0);
    UdpEndpoint serverUdp{host, udpPort};

    connected = true;
    Log("NetThread(client): connected");

    TcpFramer framer;
    uint8_t tcpBuf[4096];
    uint8_t udpBuf[4096];
    uint64_t lastHb = NowMs();

    while (!stopFlag_) {
        fd_set rset;
        FD_ZERO(&rset);
        FD_SET(tcpSock, &rset);
        if (udpSock != INVALID_SOCK) FD_SET(udpSock, &rset);
        int maxfd = static_cast<int>(tcpSock);
        if (udpSock != INVALID_SOCK && static_cast<int>(udpSock) > maxfd)
            maxfd = static_cast<int>(udpSock);

        timeval tv{0, 10000};
        select(maxfd+1, &rset, nullptr, nullptr, &tv);

        if (FD_ISSET(tcpSock, &rset)) {
            int n = TcpRecv(tcpSock, tcpBuf, sizeof(tcpBuf));
            if (n == 0 || n == -2) {
                Log("NetThread(client): server disconnected");
                InboundMsg disc;
                disc.sender = 0;
                disc.type   = 0xFE;
                inTcp.push(std::move(disc));
                break;
            } else if (n > 0) {
                framer.feed(tcpBuf, static_cast<size_t>(n),
                    [&](uint8_t type, const uint8_t* payload, uint32_t plen) {
                        InboundMsg msg;
                        msg.sender = 0;
                        msg.type   = type;
                        msg.payload.assign(payload, payload+plen);
                        inTcp.push(std::move(msg));
                    });
            }
        }

        if (udpSock != INVALID_SOCK && FD_ISSET(udpSock, &rset)) {
            UdpEndpoint from;
            int n = UdpRecvFrom(udpSock, udpBuf, sizeof(udpBuf), from);
            if (n > 0) {
                UdpDatagram dg;
                dg.data.assign(udpBuf, udpBuf+n);
                dg.from = from;
                inUdp.push(std::move(dg));
            }
        }

        OutboundMsg out;
        while (outTcp.pop(out)) {
            TcpSendAll(tcpSock, out.frame.data(), out.frame.size());
        }

        UdpDatagram dg;
        while (outUdp.pop(dg)) {
            UdpSendTo(udpSock, dg.data.data(), dg.data.size(), serverUdp);
        }

        uint64_t now = NowMs();
        if (now - lastHb > 1000) {
            lastHb = now;
            uint8_t hb[5] = {0xF0, 0,0,0,0};
            TcpSendAll(tcpSock, hb, sizeof(hb));
        }
    }

    CloseSocket(tcpSock);
    if (udpSock != INVALID_SOCK) CloseSocket(udpSock);
    connected = false;
    running   = false;
    Log("NetThread(client): stopped");
}

}
}
