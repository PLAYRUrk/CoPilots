#pragma once
// PhysicsSync.h — UDP stream of aircraft position/attitude from physics master to all.
// Physics master: reads sim datarefs and fills PhysicsState, sends via UDP each tick.
// Non-masters:    receive PhysicsState, apply position/attitude to override sim state.

#include "../net/Protocol.h"
#include "../net/NetThread.h"
#include "../session/Session.h"
#include <cstdint>
#include <atomic>

namespace cp {

class PhysicsSync {
public:
    void init(Session* session, net::NetThread* net);

    // Called from flight loop (~20 Hz):
    // - if physics master → builds state, enqueues UDP
    // - if not master    → applies latest received state
    void tick();

    // Feed an incoming UDP physics datagram (called from main thread after dequeue)
    void onUdpDatagram(const uint8_t* data, size_t len);

    void reset();

private:
    Session*         session_ = nullptr;
    net::NetThread*  net_     = nullptr;
    uint32_t         seq_     = 0;

    // Latest received state (may be slightly stale — applied each tick)
    bool                  hasState_    = false;
    proto::PhysicsState   latestState_ {};

    void sendState();
    void applyState(const proto::PhysicsState& s);
};

} // namespace cp
