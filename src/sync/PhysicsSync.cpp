#include "PhysicsSync.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMGraphics.h>
#include <algorithm>
#include <cstring>
#include <cmath>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

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
    seq_          = 0;
    hasState_     = false;
    lastRecvSeq_  = 0xFFFFFFFF;
    deadReckonS_  = 0.0;
    rendInit_     = false;
    memset(&latestState_, 0, sizeof(latestState_));

    XPLMDataRef ov = XPLMFindDataRef("sim/operation/override/override_planepath");
    if (ov) { int z = 0; XPLMSetDatavi(ov, &z, 0, 1); }

    XPLMDataRef joyOvr = XPLMFindDataRef("sim/operation/override/override_joystick");
    if (joyOvr) { int z = 0; XPLMSetDatai(joyOvr, z); }
}

void PhysicsSync::tick(double dt)
{
    if (!session_ || !net_) return;

    bool amMaster = session_->isPhysicsMaster();

    if (amMaster && !wasPhysicsMaster_) {
        // Became physics master — discard state from the previous master.
        hasState_ = false;

        // Clear the custom yoke datarefs that applyState() was writing so they
        // don't conflict with the hardware once overrides are released.
        auto clearFlt = [](const char* path) {
            XPLMDataRef dr = XPLMFindDataRef(path);
            if (dr) XPLMSetDataf(dr, 0.f);
        };
        clearFlt("sim/custom/controlls/yoke_roll");
        clearFlt("sim/custom/controlls/yoke_pitch");
        clearFlt("sim/custom/SC/yoke_roll_ratio");
        clearFlt("sim/custom/SC/yoke_pitch_ratio");
        clearFlt("sim/custom/SC/yoke_heading_ratio");
        clearFlt("sim/cockpit2/controls/yoke_roll_ratio");
        clearFlt("sim/cockpit2/controls/yoke_pitch_ratio");
        clearFlt("sim/cockpit2/controls/yoke_heading_ratio");

        Log("PhysicsSync: became physics master — cleared applyState overrides");
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
        // We have state from the physics master.
        // Explicitly clear override_planepath so the local flight model can run freely
        // (engines, hydraulics, SASL systems all need the unfrozen model to work).
        // This must be cleared EVERY tick: the gap-branch below sets it to 1, and it
        // would otherwise stay frozen forever once we transition from gap→active follower.
        XPLMDataRef ovPath = XPLMFindDataRef("sim/operation/override/override_planepath");
        if (ovPath) { int z = 0; XPLMSetDatavi(ovPath, &z, 0, 1); }

        // Track time since a new packet arrived (reset on each new seq).
        // Feed-forward integration in applyState is capped at MAX_DEADRECKON_S
        // so the client does not fly off if UDP stalls.
        if (latestState_.seq != lastRecvSeq_) {
            lastRecvSeq_  = latestState_.seq;
            deadReckonS_  = 0.0;
        } else {
            deadReckonS_ += dt;
        }

        applyState(latestState_, dt);
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

    // Timestamp in master's sim time — used by clients to compute interpolation
    // parameters so that position and velocity are always C1-consistent.
    {
        XPLMDataRef tRef = XPLMFindDataRef("sim/time/total_running_time_sec");
        if (tRef) s.t_send = static_cast<double>(XPLMGetDataf(tRef));
    }

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
    // NOTE: this lives in sim/flightmodel2 (read-only actual door state); the old
    // sim/cockpit2/engine/actuators path does not exist in X-Plane 11, which left
    // reverser_ratio permanently zero and reverse invisible to clients.
    XPLMDataRef revRef = XPLMFindDataRef("sim/flightmodel2/engines/thrust_reverser_deploy_ratio");
    if (revRef) XPLMGetDatavf(revRef, s.reverser_ratio, 0, 8);

    // Prop pitch ratio (turboprops/pistons).
    XPLMDataRef propRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/prop_ratio");
    if (propRef) XPLMGetDatavf(propRef, s.prop_ratio, 0, 8);

    // Flight controls — read raw joystick ratios directly.
    // sim/flightmodel2/controls/*_avg does not exist in all XP11 installations and
    // returns null (zero) when checked, which would overwrite clients' yoke positions
    // with 0 every UDP frame. The joystick ratios are the actual hardware inputs when
    // override_joystick=0 (master), and are exactly what applyState() writes on clients.
    XPLMDataRef ailRef  = XPLMFindDataRef("sim/joystick/yoke_roll_ratio");
    XPLMDataRef elvRef  = XPLMFindDataRef("sim/joystick/yoke_pitch_ratio");
    XPLMDataRef rudRef  = XPLMFindDataRef("sim/joystick/yoke_heading_ratio");
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

    // Engine state: read the REAL flight-model datarefs (not just cockpit indicator copies)
    // so clients can bypass their local (frozen) SASL engine model and show correct RPM.
    // ENGN_N1_/N2_ are arrays; fall back to indicator copies if not available.
    XPLMDataRef n2Real = XPLMFindDataRef("sim/flightmodel/engine/ENGN_N2_");
    if (n2Real) XPLMGetDatavf(n2Real, s.engine_N2, 0, 8);
    else {
        XPLMDataRef n2Ind = XPLMFindDataRef("sim/cockpit2/engine/indicators/N2_percent_pilot");
        if (n2Ind) XPLMGetDatavf(n2Ind, s.engine_N2, 0, 8);
    }
    XPLMDataRef n1Real = XPLMFindDataRef("sim/flightmodel/engine/ENGN_N1_");
    if (n1Real) XPLMGetDatavf(n1Real, s.engine_N1, 0, 8);
    else {
        XPLMDataRef n1Ind = XPLMFindDataRef("sim/cockpit2/engine/indicators/N1_percent_pilot");
        if (n1Ind) XPLMGetDatavf(n1Ind, s.engine_N1, 0, 8);
    }
    // Engine running flag (1 = running) so clients can force the engine into 'running' state.
    XPLMDataRef engRun = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");
    if (engRun) {
        int running[8] = {};
        XPLMGetDatavi(engRun, running, 0, 8);
        for (int k = 0; k < 8; ++k) s.engine_running[k] = static_cast<uint8_t>(running[k]);
    }

    // Fuel per tank — clients' local burn rate diverges from the master's over time,
    // changing weight and therefore engine parameters; stream the true quantities.
    XPLMDataRef fuelRef = XPLMFindDataRef("sim/flightmodel/weight/m_fuel");
    if (fuelRef) XPLMGetDatavf(fuelRef, s.fuel_kg, 0, 9);

    // Parking brake — see Protocol.h for why this is UDP-rate rather than TCP.
    {
        XPLMDataRef pbRef = XPLMFindDataRef("sim/flightmodel/controls/parkbrake");
        if (pbRef) s.parkbrake_ratio = XPLMGetDataf(pbRef);
    }

    // Rate-limited counterpart of the client-side "fuel pin" diagnostic (~every 10 s):
    // what the master is actually streaming out.  Comparing this line in the host log
    // with the client's "fuel pin recv=" line pinpoints where fuel divergence begins.
    {
        static int fuelSendLog = 0;
        if (++fuelSendLog % 600 == 1) {
            float total = 0.f;
            for (int k = 0; k < 9; ++k) total += s.fuel_kg[k];
            Log("PhysicsSync: fuel send total=%.0f kg (seq=%u)", total, seq_);
        }
    }

    // Derived aerodynamic state: read here so clients' instruments (AUASP, AoA indicator,
    // accelerometer/перегрузкомер) display values consistent with the master rather than
    // values derived from their own locally-computed kinematics.
    //
    // Background: clients keep override_planepath=0 so their SASL engine/hydraulics run
    // freely, but the live flight-model derives G-load and AoA from position+velocity, which
    // diverges from the master's because client position is blended while velocity is raw.
    // Transmitting these derived scalars from the master and writing them on clients eliminates
    // the divergence seen as host ~1g vs client ~1.5g in flight.
    rflt("sim/flightmodel2/misc/gforce_normal", s.g_nrml);
    rflt("sim/flightmodel2/misc/gforce_axil",   s.g_axil);
    rflt("sim/flightmodel2/misc/gforce_side",   s.g_side);
    rflt("sim/flightmodel/position/alpha",       s.alpha);
    rflt("sim/flightmodel/position/beta",        s.beta);

    net::UdpDatagram dg;
    dg.data.resize(sizeof(s));
    memcpy(dg.data.data(), &s, sizeof(s));
    net_->outUdp.push(std::move(dg));
}

void PhysicsSync::onMasterChanged()
{
    // Discard stored state so the first packet from the new master (seq may start at 0)
    // is accepted regardless of what the previous master's last seq was.
    hasState_  = false;
    rendInit_  = false;   // reset position blend — new master may be at a different location
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
    if (!hasState_)
        Log("PhysicsSync: first packet from master id=%u seq=%u — applyState will start",
            s->sender_id, s->seq);
    latestState_ = *s;
    hasState_    = true;
}

void PhysicsSync::applyState(const proto::PhysicsState& s, double dt)
{
    // NOTE: override_planepath is intentionally NOT set here.
    //
    // Setting override_planepath=1 freezes the local flight-model, which prevents the
    // client's engine/systems simulation (including SASL custom models on aircraft like
    // the Tu-154) from running.  Result: engines never spin up on clients until control
    // is handed over and the freeze is lifted.
    //
    // Instead we rely on continuous hard-writes of position/attitude/velocity (below)
    // at 60 Hz to keep the client aircraft in sync with the master, while letting the
    // local flight-model run freely so engines, hydraulics, etc. behave correctly.
    //
    // override_planepath=1 is still applied during the "gap" period (no UDP state yet)
    // in tick() — see the !hasState_ branch there.

    // True received position in local OpenGL coordinates (no extrapolation term —
    // feed-forward below replaces it with zero steady-state lag).
    double lx, ly, lz;
    XPLMWorldToLocal(s.lat, s.lon, s.alt, &lx, &ly, &lz);

    if (!rendInit_) {
        // First packet: snap immediately to the true position.
        rendX_ = lx;  rendY_ = ly;  rendZ_ = lz;
        rendInit_ = true;
    } else {
        // --- Velocity feed-forward ---
        // Advance rendered position along the master's velocity vector so that
        // at constant speed the rendered position tracks the true position with
        // ZERO steady-state lag (replaces the old extrapolation-to-a-static-target
        // approach that caused ~28 m persistent lag at cruise speeds).
        // Feed-forward is capped once dead-reckon time exceeds MAX_DEADRECKON_S so
        // a UDP stall does not send the client aircraft flying off on stale velocity.
        if (deadReckonS_ <= MAX_DEADRECKON_S) {
            rendX_ += static_cast<double>(s.vx) * dt;
            rendY_ += static_cast<double>(s.vy) * dt;
            rendZ_ += static_cast<double>(s.vz) * dt;
        }

        // --- G-limited position correction ---
        // After the feed-forward step, apply a small adaptive correction toward the
        // true received position to absorb any residual drift (network jitter, float
        // precision, master frame-rate mismatch).
        //
        // Corrective acceleration: G = k² * err / (dt² * g)
        //   → k_max = dt * sqrt(MAX_BLEND_G * g / err)
        // This formula is correct at any frame rate (dt-accurate), and is equivalent
        // to the old sqrt(MAX_BLEND_G*g/(err*3600)) at exactly dt = 1/60 s.
        //
        // The master's actual G-load (gforce_normal/axil/side) is written over the
        // client's value each frame (see below), so AUASP sees the master's G — not
        // the corrective G here.  MAX_BLEND_G can therefore be set higher than the
        // AUASP trip point without triggering false alerts.
        double ex = lx - rendX_, ey = ly - rendY_, ez = lz - rendZ_;
        double err = std::sqrt(ex*ex + ey*ey + ez*ez);

        // --- Large-error snap ---
        // The G-limited blend below gets WEAKER as the error grows
        // (k ~ sqrt(1/err)), so after a big divergence — UDP stall beyond
        // MAX_DEADRECKON_S, a master frame-rate drop, or X-Plane shifting its
        // local coordinate origin (which invalidates rend* wholesale) — the
        // aircraft would crawl toward the true position for many seconds,
        // visibly flying SIDEWAYS relative to its own attitude.  Beyond the
        // threshold an honest teleport looks far better than the crab.
        if (err > SNAP_ERR_M) {
            static int snapLog = 0;
            if (snapLog < 50)
                { ++snapLog; Log("PhysicsSync: position error %.1f m > %.0f m — snapping to master position", err, SNAP_ERR_M); }
            rendX_ = lx;  rendY_ = ly;  rendZ_ = lz;
        } else {
            double k = (err > 1e-4)
                ? (std::min)(MAX_BLEND_K, dt * std::sqrt(MAX_BLEND_G * 9.81 / err))
                : MAX_BLEND_K;
            rendX_ += k * ex;
            rendY_ += k * ey;
            rendZ_ += k * ez;
        }
    }

    auto wdbl = [](const char* path, double val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr) XPLMSetDatad(dr, val);
    };
    auto wflt = [](const char* path, float val) {
        XPLMDataRef dr = XPLMFindDataRef(path);
        if (dr) XPLMSetDataf(dr, val);
    };

    wdbl("sim/flightmodel/position/local_x", rendX_);
    wdbl("sim/flightmodel/position/local_y", rendY_);
    wdbl("sim/flightmodel/position/local_z", rendZ_);
    wflt(DR_PITCH, s.pitch);
    wflt(DR_ROLL,  s.roll);
    wflt(DR_HDG,   s.hdg);

    // Write the quaternion sim/flightmodel/position/q which XP11 uses for
    // rendered orientation.  Euler-only writes (psi/theta/phi above) update the
    // physics-model angles but are ignored by the renderer, causing the visual
    // heading to diverge from the physics heading.  The quaternion is the
    // canonical source of orientation for the 3-D scene.
    //
    // Conversion: aerospace ZYX sequence (yaw→pitch→roll), half-angles.
    // In XP11's left-handed local frame (Y up, X east, Z south) this maps to:
    //   q[0] = w,  q[1] = x,  q[2] = y,  q[3] = z
    {
        static XPLMDataRef drQ = XPLMFindDataRef("sim/flightmodel/position/q");
        if (drQ) {
            static constexpr float DEG2RAD = static_cast<float>(M_PI / 180.0);
            float y2 = s.hdg   * DEG2RAD * 0.5f;   // heading (yaw)
            float p2 = s.pitch * DEG2RAD * 0.5f;   // pitch
            float r2 = s.roll  * DEG2RAD * 0.5f;   // roll
            float cy = cosf(y2), sy = sinf(y2);
            float cp = cosf(p2), sp = sinf(p2);
            float cr = cosf(r2), sr = sinf(r2);
            float qv[4];
            qv[0] = cr*cp*cy + sr*sp*sy;   // w
            qv[1] = sr*cp*cy - cr*sp*sy;   // x
            qv[2] = cr*sp*cy + sr*cp*sy;   // y
            qv[3] = cr*cp*sy - sr*sp*cy;   // z
            XPLMSetDatavf(drQ, qv, 0, 4);
        }
    }

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

    // Tu-154 Felis SASL uses custom datarefs for the 3D cockpit yoke animation.
    // Write them here so the UDP physics stream (not TCP SyncEngine) drives yoke visuals.
    wflt("sim/custom/controlls/yoke_roll",      s.aileron);
    wflt("sim/custom/controlls/yoke_pitch",     s.elevator);
    wflt("sim/custom/SC/yoke_roll_ratio",       s.aileron);
    wflt("sim/custom/SC/yoke_pitch_ratio",      s.elevator);
    wflt("sim/custom/SC/yoke_heading_ratio",    s.rudder);
    wflt("sim/cockpit2/controls/yoke_roll_ratio",    s.aileron);
    wflt("sim/cockpit2/controls/yoke_pitch_ratio",   s.elevator);
    wflt("sim/cockpit2/controls/yoke_heading_ratio", s.rudder);

    // Throttle, prop pitch, and thrust reverser: written at ~60 Hz via UDP so the
    // throttle levers on non-master screens track the master's hardware smoothly.
    // TCP-based updates alone (ONCHANGE) produce visible discrete jumps during
    // continuous lever movement.
    //
    // TCP interference is prevented by isPhysicsOnlyDr in SyncEngine: the whole
    // engine-actuator throttle family (throttle_ratio, throttle_beta_rev_ratio,
    // throttle_jet_rev_ratio, prop_ratio, thrust_reverser, + _all variants) is
    // excluded from TCP send AND receive on all participants.  Without that TCP
    // echo path, there is no feedback loop, and the UDP writes only travel
    // master → clients — a safe one-way stream.
    {
        XPLMDataRef thrRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/throttle_ratio");
        if (thrRef) XPLMSetDatavf(thrRef, const_cast<float*>(s.throttle), 0, 8);
    }
    {
        // Thrust reverser.  The true door state (sim/flightmodel2/engines/
        // thrust_reverser_deploy_ratio) is read-only, so reverse is engaged on
        // the client through the signed jet throttle handle instead: -1..0 is
        // reverse, 0..1 is forward.  The local engine model then deploys its
        // own reverser doors, and the aircraft's animation/sound follow.
        // While the master's doors are deployed, s.throttle carries the
        // reverse-thrust amount (throttle_ratio semantics during reverse).
        XPLMDataRef jetRevRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/throttle_jet_rev_ratio");
        if (jetRevRef) {
            float jr[8];
            for (int i = 0; i < 8; ++i)
                jr[i] = (s.reverser_ratio[i] > 0.05f) ? -s.throttle[i] : s.throttle[i];
            XPLMSetDatavf(jetRevRef, jr, 0, 8);
        }
    }
    {
        XPLMDataRef propRef = XPLMFindDataRef("sim/cockpit2/engine/actuators/prop_ratio");
        if (propRef) XPLMSetDatavf(propRef, const_cast<float*>(s.prop_ratio), 0, 8);
    }

    wflt("sim/cockpit2/controls/flap_handle_deploy_ratio", s.flap_ratio);
    wflt("sim/cockpit2/controls/speedbrake_ratio",         s.speedbrake);

    // NOTE: gear deploy_ratio is intentionally NOT written here.
    // The Tu-154 SASL model animates each gear leg independently based on the gear
    // handle state; forcing all 10 legs to the same value fights the animation and
    // produces jerky, slow-motion gear movement on clients.  The gear handle dataref
    // is already synced via DatarefRegistry (TCP), so SASL receives the correct command
    // and runs its own smooth animation without interference.

    // Toe brakes.
    wflt("sim/cockpit2/controls/left_brake_ratio",  s.left_brake);
    wflt("sim/cockpit2/controls/right_brake_ratio", s.right_brake);

    // Engine N1/N2/running: written every tick from the master's real flight-model
    // values.  Without this, each client's local engine simulation integrates its own
    // RPM from the synced throttle position, and two independent simulations diverge
    // over a flight (observed: master 80% vs client 92% N1 at the same lever position —
    // fuel-burn/weight divergence compounds the drift).
    //
    // Writing here is convergent, not oscillatory: this callback runs
    // AfterFlightModel, so each frame the local model computes its next state FROM
    // the master's value we wrote last frame instead of from its own diverged state.
    // The local simulation keeps running (spool dynamics, SASL systems), but is
    // re-anchored to the master's RPM at 60 Hz.
    {
        XPLMDataRef n1Real = XPLMFindDataRef("sim/flightmodel/engine/ENGN_N1_");
        if (n1Real) XPLMSetDatavf(n1Real, const_cast<float*>(s.engine_N1), 0, 8);
        XPLMDataRef n2Real = XPLMFindDataRef("sim/flightmodel/engine/ENGN_N2_");
        if (n2Real) XPLMSetDatavf(n2Real, const_cast<float*>(s.engine_N2), 0, 8);
        XPLMDataRef engRun = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");
        if (engRun) {
            int running[8];
            for (int k = 0; k < 8; ++k) running[k] = s.engine_running[k];
            XPLMSetDatavi(engRun, running, 0, 8);
        }
    }

    // Fuel per tank: pin to the master's quantities so weight (and everything derived
    // from it — trim, engine parameters, performance) stays converged across the crew.
    {
        XPLMDataRef fuelRef = XPLMFindDataRef("sim/flightmodel/weight/m_fuel");
        if (fuelRef) {
            XPLMSetDatavf(fuelRef, const_cast<float*>(s.fuel_kg), 0, 9);

            // Rate-limited pin diagnostic (~every 10 s at 60 fps): received total vs
            // post-write readback total.  Distinguishes the three fuel-divergence
            // failure modes: no lines at all = no UDP state applied; recv != master's
            // gauge = wrong sender; readback != recv = the write is rejected or the
            // aircraft's Lua rewrites the tanks after us.
            static int fuelLog = 0;
            if (++fuelLog % 600 == 1) {
                float rb[9] = {};
                XPLMGetDatavf(fuelRef, rb, 0, 9);
                float recv = 0.f, got = 0.f;
                for (int k = 0; k < 9; ++k) { recv += s.fuel_kg[k]; got += rb[k]; }
                Log("PhysicsSync: fuel pin recv=%.0f kg readback=%.0f kg (sender=%u seq=%u)",
                    recv, got, s.sender_id, s.seq);
            }
        }
    }

    // Parking brake: pinned at UDP rate so the aircraft's own Lua (which rewrites
    // this dataref from its internal state every frame) cannot flap it between
    // slower TCP heartbeats — the flapping blinked ANTI SKID INOP on clients and
    // a missed release left non-masters taxiing with brakes set (hot brakes).
    wflt("sim/flightmodel/controls/parkbrake", s.parkbrake_ratio);

    // Derived aerodynamic state: write the master's G-load and AoA so that the client's
    // instruments show values consistent with the master (fixes the ~1g vs ~1.5g divergence
    // seen when the client's flight-model computes its own G from blended-position kinematics).
    //
    // These are written AfterFlightModel (same phase as the rest of applyState) so the
    // freshly computed values from the master's struct overwrite whatever the client's own
    // flight-model just derived.  If a dataref is read-only in a particular X-Plane version
    // the write fails silently (XPLMSetDataf no-ops) — instruments will then fall back to
    // the local flight-model value, which is the pre-existing behaviour.
    wflt("sim/flightmodel2/misc/gforce_normal", s.g_nrml);
    wflt("sim/flightmodel2/misc/gforce_axil",   s.g_axil);
    wflt("sim/flightmodel2/misc/gforce_side",   s.g_side);
    wflt("sim/flightmodel/position/alpha",       s.alpha);
    wflt("sim/flightmodel/position/beta",        s.beta);
}

}
