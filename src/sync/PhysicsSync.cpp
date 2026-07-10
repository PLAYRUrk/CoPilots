#include "PhysicsSync.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMGraphics.h>
#include <algorithm>
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

    XPLMDataRef ov = XPLMFindDataRef("sim/operation/override/override_planepath");
    if (ov) { int z = 0; XPLMSetDatavi(ov, &z, 0, 1); }

    XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
    if (joyOvr) { int z = 0; XPLMSetDatai(joyOvr, z); }
}

void PhysicsSync::tick()
{
    if (!session_ || !net_) return;

    bool amMaster = session_->isPhysicsMaster();

    if (amMaster && !wasPhysicsMaster_) {
        // Became physics master — discard state from the previous master.
        hasState_ = false;
    }
    if (!amMaster && wasPhysicsMaster_) {
        // Lost master role — immediately block local joystick and planepath so there
        // is no gap between the role change and the first UDP packet from the new master.
        XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
        if (joyOvr) { int v = 1; XPLMSetDatai(joyOvr, v); }
        XPLMDataRef ovPath = XPLMFindDataRef("sim/operation/override/override_planepath");
        if (ovPath) { int v = 1; XPLMSetDatavi(ovPath, &v, 0, 1); }
    }
    wasPhysicsMaster_ = amMaster;

    if (amMaster) {
        // Clear joystick/planepath overrides EVERY tick, not just on role transition.
        // If anything external (auto-sync, SmartCopilot) re-enables override_joystick,
        // this ensures the master's hardware input is restored within one flight loop frame.
        XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
        if (joyOvr) { int z = 0; XPLMSetDatai(joyOvr, z); }
        XPLMDataRef ovPath = XPLMFindDataRef("sim/operation/override/override_planepath");
        if (ovPath) { int z = 0; XPLMSetDatavi(ovPath, &z, 0, 1); }

        sendState();
    } else if (hasState_) {
        applyState(latestState_);
    } else {
        // Non-master with no state from the current master yet — keep joystick and
        // planepath blocked so local hardware cannot affect the plane during the gap.
        XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
        if (joyOvr) { int v = 1; XPLMSetDatai(joyOvr, v); }
        XPLMDataRef ovPath = XPLMFindDataRef("sim/operation/override/override_planepath");
        if (ovPath) { int v = 1; XPLMSetDatavi(ovPath, &v, 0, 1); }
    }
}

void PhysicsSync::sendState()
{
    proto::PhysicsState s{};
    s.type      = static_cast<uint8_t>(proto::UdpType::PHYSICS_STATE);
    s.seq       = seq_++;
    s.sender_id = static_cast<uint8_t>(session_->myId());

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

    // Throttle lever position (0–1). During reverse, this is the reverse-thrust amount.
    XPLMDataRef thrRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/throttle_ratio");
    if (thrRef) XPLMGetDatavf(thrRef, s.throttle, 0, 8);

    // Thrust reverser deployment (0=stowed, 1=fully deployed).
    // Without this, clients see throttle=1 as full FORWARD power when reverser is at max.
    XPLMDataRef revRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/thrust_reverser_deploy_ratio");
    if (revRef) XPLMGetDatavf(revRef, s.reverser_ratio, 0, 8);

    // Prop pitch ratio (turboprops/pistons).
    XPLMDataRef propRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/prop_ratio");
    if (propRef) XPLMGetDatavf(propRef, s.prop_ratio, 0, 8);

    // Flight controls — read combined surface deflection (captures joystick + mouse + AP).
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

    // Gear animation position (deploy_ratio, first leg; handle state is synced via datarefs).
    XPLMDataRef gearRef = XPLMFindDataRef("sim/flightmodel2/gear/deploy_ratio");
    if (gearRef) XPLMGetDatavf(gearRef, &s.gear_ratio, 0, 1);

    // Toe brakes.
    XPLMDataRef lBrRef = XPLMFindDataRef("sim/cockpit2/controls/left_brake_ratio");
    XPLMDataRef rBrRef = XPLMFindDataRef("sim/cockpit2/controls/right_brake_ratio");
    if (lBrRef) s.left_brake  = XPLMGetDataf(lBrRef);
    if (rBrRef) s.right_brake = XPLMGetDataf(rBrRef);

    net::UdpDatagram dg;
    dg.data.resize(sizeof(s));
    memcpy(dg.data.data(), &s, sizeof(s));
    net_->outUdp.push(std::move(dg));
}

void PhysicsSync::onMasterChanged()
{
    // Discard stored state so the first packet from the new master (seq may start at 0)
    // is accepted regardless of what the previous master's last seq was.
    hasState_ = false;
}

void PhysicsSync::onUdpDatagram(const uint8_t* data, size_t len)
{
    if (len < sizeof(proto::PhysicsState)) return;
    if (data[0] != static_cast<uint8_t>(proto::UdpType::PHYSICS_STATE)) return;
    if (session_ && session_->isPhysicsMaster()) return;

    const proto::PhysicsState* s = reinterpret_cast<const proto::PhysicsState*>(data);

    // Reject packets from the wrong sender to prevent stale packets from the previous
    // physics master corrupting state after a master change. Without this, onMasterChanged()
    // resets hasState_=false, the first stale packet from the old master is accepted (setting
    // hasState_=true with old seq), and then the new master's packets (seq starts at 0) are
    // all rejected because 0 <= old_seq.
    if (session_) {
        ParticipantId expected = session_->physicsMasterId();
        if (expected != INVALID_PARTICIPANT_ID && s->sender_id != expected) return;
    }

    if (hasState_ && s->seq <= latestState_.seq) return;
    latestState_ = *s;
    hasState_    = true;
}

void PhysicsSync::applyState(const proto::PhysicsState& s)
{
    // Enable flight model override so X-Plane lets us drive position
    XPLMDataRef overrideRef = XPLMFindDataRef("sim/operation/override/override_planepath");
    if (overrideRef) { int v = 1; XPLMSetDatavi(overrideRef, &v, 0, 1); }

    // lat/lon/alt datarefs are read-only without override; use local OpenGL coords instead
    double lx, ly, lz;
    XPLMWorldToLocal(s.lat, s.lon, s.alt, &lx, &ly, &lz);

    auto wdbl = [](const char* path, double val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr) XPLMSetDatad(dr, val);
    };
    auto wflt = [](const char* path, float val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr) XPLMSetDataf(dr, val);
    };

    wdbl("sim/flightmodel/position/local_x", lx);
    wdbl("sim/flightmodel/position/local_y", ly);
    wdbl("sim/flightmodel/position/local_z", lz);
    wflt(DR_PITCH, s.pitch);
    wflt(DR_ROLL,  s.roll);
    wflt(DR_HDG,   s.hdg);
    wflt(DR_VX,    s.vx);
    wflt(DR_VY,    s.vy);
    wflt(DR_VZ,    s.vz);
    wflt(DR_P,     s.p);
    wflt(DR_Q,     s.q);
    wflt(DR_R,     s.r);

    // Override local joystick so incoming control values from physics master take effect
    XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
    if (joyOvr) { int v = 1; XPLMSetDatai(joyOvr, v); }

    wflt("sim/joystick/yoke_roll_ratio",    s.aileron);
    wflt("sim/joystick/yoke_pitch_ratio",   s.elevator);
    wflt("sim/joystick/yoke_heading_ratio", s.rudder);

    // Throttle lever.
    XPLMDataRef thrRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/throttle_ratio");
    if (thrRef) XPLMSetDatavf(thrRef, const_cast<float*>(s.throttle), 0, 8);

    // Thrust reverser deployment — CRITICAL: without this, clients see throttle=1 as
    // full forward thrust when the master has reverser at maximum.
    XPLMDataRef revRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/thrust_reverser_deploy_ratio");
    if (revRef) XPLMSetDatavf(revRef, const_cast<float*>(s.reverser_ratio), 0, 8);

    // Prop pitch ratio.
    XPLMDataRef propRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/prop_ratio");
    if (propRef) XPLMSetDatavf(propRef, const_cast<float*>(s.prop_ratio), 0, 8);

    wflt("sim/cockpit2/controls/flap_handle_deploy_ratio", s.flap_ratio);
    wflt("sim/cockpit2/controls/speedbrake_ratio",         s.speedbrake);

    // Gear animation position (visual smoothness; actual deployment is driven by the
    // gear handle dataref which is synced separately via DatarefRegistry).
    XPLMDataRef gearRef = XPLMFindDataRef("sim/flightmodel2/gear/deploy_ratio");
    if (gearRef) {
        float gears[10];
        std::fill(gears, gears + 10, s.gear_ratio);
        XPLMSetDatavf(gearRef, gears, 0, 10);
    }

    // Toe brakes.
    wflt("sim/cockpit2/controls/left_brake_ratio",  s.left_brake);
    wflt("sim/cockpit2/controls/right_brake_ratio", s.right_brake);
}

}
