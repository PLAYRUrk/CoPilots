#include "PhysicsSync.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <cstring>
#include <cmath>

namespace cp {

static const char* DR_LAT      = "sim/flightmodel/position/latitude";
static const char* DR_LON      = "sim/flightmodel/position/longitude";
static const char* DR_ELEV     = "sim/flightmodel/position/elevation";
static const char* DR_PITCH    = "sim/flightmodel/position/theta";
static const char* DR_ROLL     = "sim/flightmodel/position/phi";
static const char* DR_HDG      = "sim/flightmodel/position/psi";
static const char* DR_VX       = "sim/flightmodel/position/local_vx";
static const char* DR_VY       = "sim/flightmodel/position/local_vy";
static const char* DR_VZ       = "sim/flightmodel/position/local_vz";
static const char* DR_P        = "sim/flightmodel/position/P";
static const char* DR_Q        = "sim/flightmodel/position/Q";
static const char* DR_R        = "sim/flightmodel/position/R";

void PhysicsSync::init(Session* session, net::NetThread* net)
{
    session_ = session;
    net_     = net;
    reset();
}

void PhysicsSync::reset()
{
    seq_      = 0;
    hasState_ = false;
    memset(&latestState_, 0, sizeof(latestState_));
}

void PhysicsSync::tick()
{
    if (!session_ || !net_) return;

    if (session_->isPhysicsMaster()) {
        sendState();
    } else if (hasState_) {
        applyState(latestState_);
    }
}

void PhysicsSync::sendState()
{
    proto::PhysicsState s{};
    s.type = static_cast<uint8_t>(proto::UdpType::PHYSICS_STATE);
    s.seq  = seq_++;

    XPLMDataRef dr;
    auto rdbl = [&](const char* path, double& out) {
        dr = XPLMFindDataRef(path);
        if (dr) out = XPLMGetDatad(dr);
    };
    auto rflt = [&](const char* path, float& out) {
        dr = XPLMFindDataRef(path);
        if (dr) out = XPLMGetDataf(dr);
    };

    rdbl(DR_LAT,   s.lat);
    rdbl(DR_LON,   s.lon);
    rdbl(DR_ELEV,  s.alt);
    rflt(DR_PITCH, s.pitch);
    rflt(DR_ROLL,  s.roll);
    rflt(DR_HDG,   s.hdg);
    rflt(DR_VX,    s.vx);
    rflt(DR_VY,    s.vy);
    rflt(DR_VZ,    s.vz);
    rflt(DR_P,     s.p);
    rflt(DR_Q,     s.q);
    rflt(DR_R,     s.r);

    XPLMDataRef thrRef = XPLMFindDataRef("sim/flightmodel/engine/ENGN_thro_use");
    if (thrRef) XPLMGetDatavf(thrRef, s.throttle, 0, 8);

    XPLMDataRef ailRef  = XPLMFindDataRef("sim/flightmodel2/controls/aileron_avg");
    XPLMDataRef elvRef  = XPLMFindDataRef("sim/flightmodel2/controls/elevator_avg");
    XPLMDataRef rudRef  = XPLMFindDataRef("sim/flightmodel2/controls/rudder_avg");
    XPLMDataRef flapRef = XPLMFindDataRef("sim/cockpit2/controls/flap_handle_deploy_ratio");
    XPLMDataRef sbRef   = XPLMFindDataRef("sim/cockpit2/controls/speedbrake_ratio");
    if (ailRef)  s.aileron    = XPLMGetDataf(ailRef);
    if (elvRef)  s.elevator   = XPLMGetDataf(elvRef);
    if (rudRef)  s.rudder     = XPLMGetDataf(rudRef);
    if (flapRef) s.flap_ratio = XPLMGetDataf(flapRef);
    if (sbRef)   s.speedbrake = XPLMGetDataf(sbRef);

    net::UdpDatagram dg;
    dg.data.resize(sizeof(s));
    memcpy(dg.data.data(), &s, sizeof(s));
    net_->outUdp.push(std::move(dg));
}

void PhysicsSync::onUdpDatagram(const uint8_t* data, size_t len)
{
    if (len < sizeof(proto::PhysicsState)) return;
    if (data[0] != static_cast<uint8_t>(proto::UdpType::PHYSICS_STATE)) return;
    if (session_ && session_->isPhysicsMaster()) return;

    const proto::PhysicsState* s = reinterpret_cast<const proto::PhysicsState*>(data);
    if (hasState_ && s->seq <= latestState_.seq) return;
    latestState_ = *s;
    hasState_    = true;
}

void PhysicsSync::applyState(const proto::PhysicsState& s)
{
    auto wdbl = [](const char* path, double val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDatad(dr, val);
    };
    auto wflt = [](const char* path, float val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDataf(dr, val);
    };

    wdbl(DR_LAT,   s.lat);
    wdbl(DR_LON,   s.lon);
    wdbl(DR_ELEV,  s.alt);
    wflt(DR_PITCH, s.pitch);
    wflt(DR_ROLL,  s.roll);
    wflt(DR_HDG,   s.hdg);
    wflt(DR_VX,    s.vx);
    wflt(DR_VY,    s.vy);
    wflt(DR_VZ,    s.vz);
    wflt(DR_P,     s.p);
    wflt(DR_Q,     s.q);
    wflt(DR_R,     s.r);
}

}
