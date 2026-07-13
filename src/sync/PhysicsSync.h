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

    void tick(double dt);

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

    // Sequence number of the last processed UDP packet.
    uint32_t lastRecvSeq_ = 0xFFFFFFFF;

    // Seconds elapsed since a new UDP packet was received (monotone counter, reset
    // on each new seq).  Feed-forward integration is capped at MAX_DEADRECKON_S to
    // prevent the client aircraft from flying off on stale velocity data if UDP stalls.
    double deadReckonS_ = 0.0;

    // Rendered position accumulator.  Each frame we advance this by the master's
    // velocity (feed-forward, zero lag at constant speed), then apply a small
    // G-limited correction toward the true received position.
    double rendX_ = 0.0, rendY_ = 0.0, rendZ_ = 0.0;
    bool   rendInit_ = false;

    // Maximum per-frame blend coefficient and corrective G limit.
    // MAX_BLEND_G can be increased to speed up post-manoeuvre convergence; the
    // master's actual G-load is written over the client's value every frame via
    // applyState (gforce_normal/axil/side), so AUASP sees the master's G, not ours.
    static constexpr double MAX_BLEND_K    = 0.15;
    static constexpr double MAX_BLEND_G    = 1.5;   // max corrective G (was 0.8)
    static constexpr double MAX_DEADRECKON_S = 1.0; // cap feed-forward on UDP stall

    // Above this position error the G-limited blend is abandoned and the aircraft
    // snaps directly to the master's position.  The blend coefficient shrinks as
    // the error grows (k ~ sqrt(1/err)), so large divergences (UDP stall, master
    // fps drop, local-origin shift) would otherwise take many seconds to converge
    // with the aircraft visibly flying sideways relative to its attitude.
    static constexpr double SNAP_ERR_M     = 100.0;

    void sendState();
    void applyState(const proto::PhysicsState& s, double dt);
};

}
