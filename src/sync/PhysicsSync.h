#pragma once

#include "../net/Protocol.h"
#include "../net/NetThread.h"
#include "../session/Session.h"
#include <cstdint>
#include <atomic>

namespace cp {

class PhysicsSync {
public:
    void init(Session* session, net::NetThread* net);

    void tick();

    void onUdpDatagram(const uint8_t* data, size_t len);

    // Call whenever the physics master assignment changes so seq tracking resets.
    void onMasterChanged();

    void reset();

private:
    Session*         session_ = nullptr;
    net::NetThread*  net_     = nullptr;
    uint32_t         seq_     = 0;

    bool                  hasState_         = false;
    bool                  wasPhysicsMaster_ = false;
    proto::PhysicsState   latestState_ {};

    void sendState();
    void applyState(const proto::PhysicsState& s);
};

}
