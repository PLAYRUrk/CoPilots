#pragma once

#include "../net/Protocol.h"
#include "../net/NetThread.h"
#include "../session/Session.h"
#include <cstdint>

namespace cp {

class WeatherSync {
public:
    void init(Session* session, net::NetThread* net);
    void tick(float dt);
    void onTcpMessage(const uint8_t* payload, size_t len);
    void reset();

private:
    Session*        session_   = nullptr;
    net::NetThread* net_       = nullptr;
    float           elapsed_   = 0.f;

    static constexpr float kSendInterval = 5.f;

    void sendState();
    void applyState(proto::MsgReader& r);
};

}
