#include "SyncEngine.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>
#include <cmath>
#include <cstring>

namespace cp {

static constexpr float FLOAT_EPS   = 1e-5f;
static constexpr double DOUBLE_EPS = 1e-9;
// Echo-absorption window after applyIncoming.  Resets while value keeps settling.
static constexpr int ECHO_WINDOW   = 20;  // ~0.33 s at 60 fps; resets each absorbed frame

// Returns true if the difference between 'cur' and 'baseline' exceeds the
// synchronisation threshold — i.e. the change is large enough to be a real
// user input rather than SASL quantization/settling noise.
//
// Threshold: 0.05% relative (5e-4 * |baseline|) with an absolute floor of 5e-4.
// Rationale: the smallest real cockpit input (e.g. 1° heading step = 0.0175 rad ≈
// 35× the 5e-4 floor) always passes; sub-threshold quantization noise is suppressed.
// INT datarefs are exact — any difference is a real change.
//
// Used by BOTH the change-detector (auto/_SHARED/smartcopilot branch) and the
// echo absorber (isSettlingEcho) so the two thresholds are guaranteed to match.
static bool exceedsSyncThreshold(const DrValue& cur, const DrValue& baseline)
{
    if (cur.type != baseline.type) return true;
    switch (cur.type) {
        case DrType::INT:
            return cur.i != baseline.i;
        case DrType::FLOAT: {
            float diff  = std::fabs(cur.f - baseline.f);
            float scale = std::fabs(baseline.f);
            return diff >= (scale > 0.1f ? 5e-4f * scale : 5e-4f);
        }
        case DrType::DOUBLE: {
            double diff  = std::fabs(cur.d - baseline.d);
            double scale = std::fabs(baseline.d);
            return diff >= (scale > 0.1 ? 5e-4 * scale : 5e-4);
        }
        case DrType::FLOAT_ARR: {
            if (cur.fa.size() != baseline.fa.size()) return true;
            for (size_t k = 0; k < cur.fa.size(); ++k) {
                float diff  = std::fabs(cur.fa[k] - baseline.fa[k]);
                float scale = std::fabs(baseline.fa[k]);
                if (diff >= (scale > 0.1f ? 5e-4f * scale : 5e-4f)) return true;
            }
            return false;
        }
        case DrType::INT_ARR:
            if (cur.ia.size() != baseline.ia.size()) return true;
            return memcmp(cur.ia.data(), baseline.ia.data(),
                          cur.ia.size() * sizeof(int)) != 0;
        case DrType::DATA:
            return cur.ba != baseline.ba;
        default:
            return !cur.approxEqual(baseline);
    }
}

// Returns true if 'cur' looks like SASL quantization/settling noise relative to
// 'lastApplied' — delegates to exceedsSyncThreshold so change-detection and
// echo-absorption always use the identical threshold.
static bool isSettlingEcho(const DrValue& cur, const DrValue& lastApplied)
{
    return !exceedsSyncThreshold(cur, lastApplied);
}

// Returns true if this dataref is a simulation OUTPUT — a value computed by the
// physics master's simulation engine and streamed to passive clients.  Outputs are
// authoritative from the physics master only; non-masters must never send them back,
// or they fight the master's live simulation (APU write-war, engine RPM crawl).
//
// Classification sources:
//   1. SmartCopilot mode ([continued] section): SyncMode::CONTINUOUS signals that the
//      original smartcopilot.cfg designated this as a "master sends" value.  This covers
//      APU/engine RPM, voltages, temperatures, hydraulic pressure, and instrument reads.
//      (SmartCopilot's own semantics: only the designated master sends [continued] data.)
//   2. _AUTO zone: heuristic path analysis for standard sim/ datarefs that slip through
//      the namespace filter with output-like names (rare, but possible on modded aircraft).
//
// Note: isAutoZone datarefs from DataRefs.txt are already filtered to writable cockpit
// switches, so false-positive risk is low for the _AUTO branch.
static bool classifyAsOutput(const RegisteredDataref& rd, bool isAutoZone)
{
    // SmartCopilot [continued] → simulation output; [triggers]/[send_back] → INPUT.
    if (!isAutoZone && rd.mode == SyncMode::CONTINUOUS)
        return true;

    // _AUTO zone: heuristic on path tokens/suffixes for output-like standard datarefs.
    if (isAutoZone) {
        const std::string& p = rd.path;
        auto endsWith = [&](const char* suffix) -> bool {
            size_t n = std::strlen(suffix);
            return p.size() >= n && p.compare(p.size() - n, n, suffix, n) == 0;
        };
        auto hasToken = [&](const char* tok) -> bool {
            return p.find(tok) != std::string::npos;
        };
        if (endsWith("_rpm") || endsWith("_n1") || endsWith("_n2") ||
            endsWith("_egt") || endsWith("_oil_t") || endsWith("_oil_p") ||
            endsWith("_oil_q"))  return true;
        if (endsWith("_amp") || endsWith("_volt")) return true;
        if (hasToken("gforce") || hasToken("/gauges/") || hasToken("/indicators/"))
            return true;
    }
    return false;
}

// Returns true for datarefs that PhysicsSync manages exclusively via UDP.
// Syncing these via TCP as well causes dual-write conflicts or echo loops.
static bool isPhysicsOnlyDr(const std::string& path)
{
    auto startsWith = [&](const char* prefix) {
        size_t n = std::strlen(prefix);
        return path.size() >= n && path.compare(0, n, prefix, n) == 0;
    };
    auto hasToken = [&](const char* tok) -> bool {
        return path.find(tok) != std::string::npos;
    };

    // Yoke and override datarefs — exclusively driven by PhysicsSync via UDP.
    // Sending via TCP causes the UDP write on clients to be picked up as a
    // "local change" on the next tick and echoed back, oscillating the yoke.
    if (startsWith("sim/joystick/yoke_"))           return true;
    if (startsWith("sim/cockpit2/controls/yoke_"))  return true;
    if (startsWith("sim/custom/controlls/yoke_"))   return true;
    if (startsWith("sim/custom/SC/yoke_"))          return true;
    if (path == "sim/operation/override/override_joystick")  return true;
    if (path == "sim/operation/override/override_planepath") return true;

    // Throttle levers, prop pitch, and thrust reversers — synced exclusively
    // via UDP (applyState, ~60 Hz).  Adding a parallel TCP channel produces a
    // dual-write conflict: tick() samples throttle at one point in the flight
    // loop and sendState() samples it again slightly later; during active lever
    // movement these two samples differ, so non-masters receive inconsistent
    // values from both channels and the rendered lever oscillates.  All engine
    // actuator tokens (including *_all gang variants) are covered by hasToken.
    if (hasToken("throttle_ratio") || hasToken("prop_ratio") || hasToken("thrust_reverser"))
        return true;

    // The ENTIRE engine-actuator throttle family must be UDP-only, not just
    // "throttle_ratio": X-Plane derives throttle_beta_rev_ratio and
    // throttle_jet_rev_ratio (+ _all gang variants) from throttle_ratio every
    // flight-model frame.  On a non-master, applyState() writes throttle_ratio,
    // the flight model then updates the derived datarefs BETWEEN flight-loop
    // callbacks (after refreshCache already ran), so tick() sees them as local
    // user changes and echoes the stale values back to the master via TCP —
    // snapping the master's levers back and producing the throttle jitter loop.
    if (startsWith("sim/cockpit2/engine/actuators/throttle"))
        return true;

    // CUSTOM aircraft throttle/reverser lever datarefs — generic patterns.
    // SmartCopilot configs (Zibo, LevelUp, IXEG, …) list the aircraft's own lever
    // datarefs, e.g. laminar/B738/engine/thrust1_leveler, laminar/B738/axis/throttle1,
    // laminar/B738/flt_ctrls/reverse_lever1, laminar/B738/throttle_override.  The
    // aircraft's plugin derives these from (or feeds them into) the sim throttle that
    // PhysicsSync already streams via UDP, so TCP-syncing them re-creates the same
    // dual-write jitter with custom paths.  Blocking here makes stock SmartCopilot
    // configs usable without a hand-tuned copilots.json.
    //
    // "autothrottle" is explicitly allowed through: A/T arm switches and status flags
    // (sim/cockpit2/autopilot/autothrottle_on, custom *autothrottle* toggles) are
    // cockpit switches that MUST keep syncing via TCP.
    if (hasToken("throttle") && !hasToken("autothrottle"))  return true;
    if (hasToken("thrust") && (hasToken("lever") || hasToken("leveler")))
        return true;
    if (hasToken("reverse_lever") || hasToken("reverser") || hasToken("thrust_reverse"))
        return true;
    if (hasToken("prop_lever"))  return true;

    // Custom yoke datarefs (animation copies, hide/show checklists) — either driven
    // by PhysicsSync UDP or per-pilot cosmetic state that must stay local.
    if (hasToken("yoke"))  return true;

    // Custom toe-brake datarefs — PhysicsSync streams left/right_brake_ratio via UDP.
    if (hasToken("toe_brake"))  return true;

    // mixture_ratio_all: gang write sets ALL engines at once, overriding
    // per-engine values already correctly delivered by UDP.
    if (path == "sim/cockpit2/engine/actuators/mixture_ratio_all")
        return true;

    return false;
}

bool DrValue::approxEqual(const DrValue& o) const
{
    if (type != o.type) return false;
    switch (type) {
        case DrType::INT:    return i == o.i;
        case DrType::FLOAT:  return std::fabs(f - o.f) < FLOAT_EPS;
        case DrType::DOUBLE: {
            // Use a relative epsilon so large values (lat/lon, nav frequencies) don't
            // generate spurious sends.  DOUBLE_EPS (1e-9) stays as an absolute floor.
            double diff = std::fabs(d - o.d);
            double ad = std::fabs(d), ao = std::fabs(o.d);
            double mag  = ad > ao ? ad : ao;
            double eps  = 1e-7 * mag;
            return diff <= (eps > DOUBLE_EPS ? eps : DOUBLE_EPS);
        }
        case DrType::INT_ARR:
            if (ia.size() != o.ia.size()) return false;
            return memcmp(ia.data(), o.ia.data(), ia.size()*sizeof(int)) == 0;
        case DrType::FLOAT_ARR:
            if (fa.size() != o.fa.size()) return false;
            for (size_t i=0; i<fa.size(); ++i)
                if (std::fabs(fa[i]-o.fa[i]) >= FLOAT_EPS) return false;
            return true;
        case DrType::DATA:
            return ba == o.ba;
        default: return true;
    }
}

void SyncEngine::init(DatarefRegistry* reg, Session* session)
{
    reg_     = reg;
    session_ = session;
    reset();
}

void SyncEngine::reset()
{
    // Release any commands still held down by the network so a disconnect
    // mid-hold does not leave e.g. a fire-test switch stuck forever.
    if (reg_) {
        for (size_t i = 0; i < heldByNet_.size() && i < reg_->commands().size(); ++i) {
            if (heldByNet_[i] && reg_->commands()[i].handle)
                XPLMCommandEnd(static_cast<XPLMCommandRef>(reg_->commands()[i].handle));
        }
    }
    cmdEvents_.clear();
    suppressBegin_.clear();
    suppressEnd_.clear();
    heldByNet_.clear();

    if (!reg_) { cache_.clear(); return; }
    cache_.resize(reg_->datarefs().size());
    for (size_t i = 0; i < reg_->datarefs().size(); ++i) {
        const auto& rd = reg_->datarefs()[i];
        if (rd.handle)
            cache_[i].value = readDr(rd);
    }
}

DrValue SyncEngine::readDr(const RegisteredDataref& rd) const
{
    DrValue v;
    v.type = rd.type;
    switch (rd.type) {
        case DrType::INT:
            if (rd.arrayIndex >= 0) {
                int tmp = 0;
                XPLMGetDatavi(rd.handle, &tmp, rd.arrayIndex, 1);
                v.i = tmp;
            } else {
                v.i = XPLMGetDatai(rd.handle);
            }
            break;
        case DrType::FLOAT:
            if (rd.arrayIndex >= 0) {
                float tmp = 0.f;
                XPLMGetDatavf(rd.handle, &tmp, rd.arrayIndex, 1);
                v.f = tmp;
            } else {
                v.f = XPLMGetDataf(rd.handle);
            }
            break;
        case DrType::DOUBLE: v.d = XPLMGetDatad(rd.handle); break;
        case DrType::INT_ARR: {
            int count = XPLMGetDatavi(rd.handle, nullptr, 0, 0);
            if (count > 0) {
                v.ia.resize(count);
                XPLMGetDatavi(rd.handle, v.ia.data(), 0, count);
            }
            break;
        }
        case DrType::FLOAT_ARR: {
            int count = XPLMGetDatavf(rd.handle, nullptr, 0, 0);
            if (count > 0) {
                v.fa.resize(count);
                XPLMGetDatavf(rd.handle, v.fa.data(), 0, count);
            }
            break;
        }
        case DrType::DATA: {
            int count = XPLMGetDatab(rd.handle, nullptr, 0, 0);
            if (count > 0) {
                v.ba.resize(count);
                XPLMGetDatab(rd.handle, v.ba.data(), 0, count);
            }
            break;
        }
        default: break;
    }
    return v;
}

void SyncEngine::writeDr(const RegisteredDataref& rd, const DrValue& val)
{
    if (!rd.handle || !rd.writable) return;
    switch (rd.type) {
        case DrType::INT:
            if (rd.arrayIndex >= 0) {
                int tmp = val.i;
                XPLMSetDatavi(rd.handle, &tmp, rd.arrayIndex, 1);
            } else {
                XPLMSetDatai(rd.handle, val.i);
            }
            break;
        case DrType::FLOAT:
            if (rd.arrayIndex >= 0) {
                float tmp = val.f;
                XPLMSetDatavf(rd.handle, &tmp, rd.arrayIndex, 1);
            } else {
                XPLMSetDataf(rd.handle, val.f);
            }
            break;
        case DrType::DOUBLE: XPLMSetDatad(rd.handle, val.d); break;
        case DrType::INT_ARR:
            if (!val.ia.empty())
                XPLMSetDatavi(rd.handle,
                              const_cast<int*>(val.ia.data()), 0,
                              static_cast<int>(val.ia.size()));
            break;
        case DrType::FLOAT_ARR:
            if (!val.fa.empty())
                XPLMSetDatavf(rd.handle,
                              const_cast<float*>(val.fa.data()), 0,
                              static_cast<int>(val.fa.size()));
            break;
        case DrType::DATA:
            if (!val.ba.empty())
                XPLMSetDatab(rd.handle,
                             const_cast<uint8_t*>(val.ba.data()), 0,
                             static_cast<int>(val.ba.size()));
            break;
        default: break;
    }

    // Verify the write was accepted (log first few failures to diagnose read-only datarefs)
    static int writeVerifyLog = 0;
    if (writeVerifyLog < 20) {
        DrValue after = readDr(rd);
        if (!after.approxEqual(val)) {
            ++writeVerifyLog;
            Log("SyncEngine::writeDr VERIFY FAILED path=%s type=%d",
                rd.path.c_str(), (int)rd.type);
        }
    }
}

void SyncEngine::refreshCache()
{
    if (!reg_) return;
    const auto& drs = reg_->datarefs();
    for (size_t i = 0; i < drs.size() && i < cache_.size(); ++i) {
        const auto& rd = drs[i];
        if (!rd.handle) continue;
        // Skip datarefs still in the echo-absorption window: their cache holds the
        // wire value that was just received; overwriting it with the live SASL readback
        // would cause tick() to compare SASL-vs-SASL (always equal) and never detect
        // the real applied value — defeating echo suppression entirely.
        // Note: throttle/prop/reverser are now isPhysicsOnlyDr (UDP-only), so they
        // never pass through applyIncoming and always have echoFrames == 0; they are
        // read back here normally, but tick() skips them before the cache is used.
        if (cache_[i].echoFrames > 0) continue;
        cache_[i].value = readDr(rd);
    }
}

void SyncEngine::tick(DrChangedCb onChanged, CmdFiredCb onCmd)
{
    if (!reg_ || !session_) return;

    bool doFull = fullSyncPending_;
    fullSyncPending_ = false;

    const auto& drs = reg_->datarefs();
    int sentCount = 0;
    int ownCount  = 0;
    for (size_t i = 0; i < drs.size(); ++i) {
        const auto& rd = drs[i];
        if (!rd.handle) continue;

        // Zone ownership determines whether this participant sends a dataref:
        //
        //   _AUTO zone / SmartCopilot — two sub-cases:
        //     INPUT  (ONCHANGE / [triggers]):  any participant sends — these are cockpit
        //            switches/knobs/buttons that any crew member may operate.
        //     OUTPUT (CONTINUOUS / [continued]): only the physics master sends — these are
        //            simulation-computed values (engine RPM, voltages, temperatures, gauges).
        //            Non-masters never send OUTPUT datarefs, which would fight the master's
        //            live simulation (APU write-war, engine RPM crawl bugs).
        //
        //   Normal (copilots.json) — only the zone owner sends (unchanged behaviour).
        //
        // Flight-control exclusivity (yoke, pedals, position) is enforced by PhysicsSync
        // via UDP — those datarefs are outside the _AUTO namespace and not in DataRefs.txt.
        bool iOwn;
        bool isAutoZone   = (rd.zoneId == AUTO_ZONE_ID);
        bool isSharedZone = (rd.zoneId == SHARED_ZONE_ID);

        // Yoke and override datarefs are owned exclusively by PhysicsSync (UDP).
        // Sending them via TCP as well causes the UDP write on clients to be picked
        // up as a "local change" on the next tick and echoed back, oscillating the yoke.
        if ((isAutoZone || isSharedZone || smartCopilotMode_) && isPhysicsOnlyDr(rd.path))
            continue;
        if (isAutoZone || isSharedZone || smartCopilotMode_) {
            if (classifyAsOutput(rd, isAutoZone)) {
                // OUTPUT datarefs (simulation-computed values): only the physics
                // master is the authoritative sender.
                iOwn = session_->isPhysicsMaster();
            } else {
                // INPUT dataref: any crew member may send cockpit switch/knob changes.
                iOwn = true;
            }
        } else {
            iOwn = session_->iOwnZone(rd.zoneId);
        }
        if (!iOwn) continue;
        ++ownCount;

        Cache& c = cache_[i];

        // Tick echo-absorption window every frame regardless of whether we send.
        if (c.echoFrames > 0) --c.echoFrames;
        ++c.framesSinceLocalChange;

        DrValue cur = readDr(rd);

        // For _AUTO / smartcopilot datarefs use the same relative threshold as the
        // echo absorber so the two can never diverge and produce a feedback loop.
        // For manual copilots.json datarefs keep the original absolute approxEqual
        // to avoid any behaviour change in normal-mode sessions.
        bool changed = (isAutoZone || isSharedZone || smartCopilotMode_)
                           ? exceedsSyncThreshold(cur, c.value)
                           : !cur.approxEqual(c.value);
        // CONTINUOUS mode only applies to manual-config datarefs in normal mode;
        // _AUTO and SmartCopilot datarefs always use ONCHANGE to avoid flooding TCP.
        bool isContinuous = !smartCopilotMode_ && !isAutoZone
                            && rd.mode == SyncMode::CONTINUOUS;
        bool send = doFull || isContinuous || changed;

        if (!send) continue;

        // Value-based echo absorption: if a network value was just applied and the
        // current change looks like SASL quantization noise (small relative to the
        // applied value), absorb it silently without sending.  This replaces the old
        // frame-count suppression window that blocked ALL outbound sends for 0.5–2 s
        // and caused three-position switches to "fly past" and stop responding.
        if (changed && c.echoFrames > 0 && isSettlingEcho(cur, c.lastApplied)) {
            c.value = cur;          // track settling value; do not send
            c.echoFrames = ECHO_WINDOW; // reset window so SASL settling is absorbed until stable
            continue;
        }

        // Revert absorption: the aircraft's own logic (SASL/Lua that owns this
        // dataref's state) rejected the network write and restored the PREVIOUS
        // value.  Sending that revert back would snap the originator's switch
        // back — the "switch flips back even on the host" write-war.  Absorb it
        // silently; the two sims simply disagree on this dataref (only a command
        // can move such a switch — datarefs like this belong in the commands list).
        if (changed && c.echoFrames > 0 && isSettlingEcho(cur, c.preApply)) {
            c.value = cur;
            c.echoFrames = ECHO_WINDOW;
            continue;
        }

        c.value = cur;
        if (changed) {
            // Real local change: clear echo state so subsequent sends are not absorbed.
            c.echoFrames = 0;
            c.framesSinceLocalChange = 0;
        }

        onChanged(static_cast<uint16_t>(i), cur);
        ++sentCount;
    }

    // Rate-limited tick diagnostic (every ~5 s)
    static int tickLog = 0;
    if (++tickLog % 300 == 1 && (sentCount > 0 || doFull))
        Log("SyncEngine::tick own=%d sent=%d/%zu full=%d",
            ownCount, sentCount, drs.size(), (int)doFull);

    // Flush queued local command edges in the order they were fired so a quick
    // click (Begin immediately followed by End) arrives in the right order.
    for (const auto& ev : cmdEvents_)
        onCmd(ev.first, ev.second);
    cmdEvents_.clear();
}

void SyncEngine::applyIncoming(uint16_t drIndex, const DrValue& val,
                                uint8_t senderParticipantId)
{
    if (!reg_ || !session_) return;
    const RegisteredDataref* rd = reg_->getDr(drIndex);
    if (!rd || !rd->handle) return;

    if (rd->zoneId == AUTO_ZONE_ID || rd->zoneId == SHARED_ZONE_ID || smartCopilotMode_) {
        // Echo from ourselves is always dropped.
        if (senderParticipantId == session_->myId()) return;

        // Yoke and override datarefs are driven exclusively by PhysicsSync via UDP.
        // Reject TCP copies to prevent the UDP write from being treated as a local
        // change on the next tick and echoed back (which would freeze the yoke at 0).
        if (isPhysicsOnlyDr(rd->path)) return;

        // OUTPUT datarefs (simulation-computed values such as engine RPM, voltages,
        // temperatures, and instrument readings) are authoritative from the physics master
        // only.  If we ARE the physics master we must not let an incoming write overwrite
        // our own simulation state — that is the root cause of the APU write-war bug where
        // a client's diverged RPM continuously stomps the host's ramping value.
        bool isAutoZone = (rd->zoneId == AUTO_ZONE_ID);
        if (classifyAsOutput(*rd, isAutoZone)) {
            if (session_->isPhysicsMaster())
                return;  // I'm the master; I produce these, I don't consume them.
            // Non-master: accept OUTPUT datarefs from the physics master only.
            // senderParticipantId==0 means the message was relayed by the host whose
            // authority was already verified on that end — trust the relay.
            ParticipantId masterId = session_->physicsMasterId();
            if (masterId != INVALID_PARTICIPANT_ID && senderParticipantId != 0
                && senderParticipantId != masterId)
                return;  // Output from a non-master participant — reject.
        }
    } else {
        // Zone-authority mode: only apply if we don't own the zone.
        if (session_->iOwnZone(rd->zoneId)) return;

        // senderParticipantId == 0 means the message was relayed by the server.
        if (senderParticipantId != 0) {
            ParticipantId auth = session_->authorityFor(rd->zoneId);
            if (auth != senderParticipantId && auth != INVALID_PARTICIPANT_ID) return;
            if (senderParticipantId == session_->myId()) return;
        }
    }

    // Remember the pre-write value so tick() can recognise a revert (aircraft's own
    // logic restoring the old value after our write) and absorb it instead of
    // echoing it back to the sender.
    DrValue preApply = readDr(*rd);

    writeDr(*rd, val);

    if (drIndex < cache_.size()) {
        cache_[drIndex].preApply = std::move(preApply);
        // Store the wire value as the authoritative cache entry.  Using the exact
        // wire value (not a post-write readback) guarantees that host and client
        // converge on the same number: the readback could have been quantised by SASL
        // to a slightly different float, causing both sides to disagree and ping-pong.
        cache_[drIndex].value       = val;
        cache_[drIndex].lastApplied = val;

        // Arm a short echo-absorption window for float/array types so that SASL
        // quantization noise (small deviations from the applied value) in the next
        // few frames is absorbed silently rather than sent back to the sender.
        // INT datarefs are written exactly — no echo window needed.
        bool needsEchoWindow = (val.type == DrType::FLOAT   ||
                                 val.type == DrType::DOUBLE  ||
                                 val.type == DrType::FLOAT_ARR ||
                                 val.type == DrType::INT_ARR);
        cache_[drIndex].echoFrames = needsEchoWindow ? ECHO_WINDOW : 0;
        // Do NOT reset framesSinceLocalChange — an incoming network write does not
        // count as a local user interaction for reconciliation purposes.
    }
}

void SyncEngine::applyIncomingCommand(uint16_t cmdIndex, uint8_t phase,
                                       uint8_t senderParticipantId)
{
    if (!reg_ || !session_) return;
    const RegisteredCommand* rc = reg_->getCmd(cmdIndex);
    if (!rc || !rc->handle) return;

    // Shared-cockpit command zones: _AUTO, _SHARED, and everything in SmartCopilot
    // mode.  Any crew member may fire these; only the self-echo is dropped.  The
    // zone-authority check below must NOT run for them: a role in copilots.json
    // (e.g. pilot → ["_SHARED"]) makes authorityFor() return the first participant
    // holding the zone (the host), which silently dropped every command from
    // clients — and relayed commands (sender==0) from anyone.
    bool sharedZone = (rc->zoneId == AUTO_ZONE_ID || rc->zoneId == SHARED_ZONE_ID
                       || smartCopilotMode_);
    if (sharedZone) {
        if (senderParticipantId == session_->myId()) return;
    } else {
        // Zone-authority mode (senderParticipantId == 0 = relayed by the server,
        // authority already verified on the host — trust the relay).
        if (senderParticipantId != 0) {
            ParticipantId auth = session_->authorityFor(rc->zoneId);
            if (auth != senderParticipantId && auth != INVALID_PARTICIPANT_ID) return;
            if (senderParticipantId == session_->myId()) return;
        }
    }

    // Suppress the echo: XPLMCommandBegin/End/Once trigger our registered handler
    // synchronously with the corresponding phase(s); those self-echoes must not be
    // re-queued for re-sending.
    if (suppressBegin_.size() <= cmdIndex) suppressBegin_.resize(cmdIndex + 1, 0);
    if (suppressEnd_.size()   <= cmdIndex) suppressEnd_.resize(cmdIndex + 1, 0);
    if (heldByNet_.size()     <= cmdIndex) heldByNet_.resize(cmdIndex + 1, false);

    XPLMCommandRef h = static_cast<XPLMCommandRef>(rc->handle);
    switch (phase) {
        case CMD_PHASE_BEGIN:
            ++suppressBegin_[cmdIndex];
            heldByNet_[cmdIndex] = true;
            XPLMCommandBegin(h);
            break;
        case CMD_PHASE_END:
            ++suppressEnd_[cmdIndex];
            heldByNet_[cmdIndex] = false;
            XPLMCommandEnd(h);
            break;
        default: // CMD_PHASE_ONCE — legacy/compat: a full press in one message
            ++suppressBegin_[cmdIndex];
            ++suppressEnd_[cmdIndex];
            XPLMCommandOnce(h);
            break;
    }
}

void SyncEngine::notifyCommandFired(uint16_t cmdIndex, uint8_t phase)
{
    if (phase == CMD_PHASE_BEGIN) {
        if (suppressBegin_.size() > cmdIndex && suppressBegin_[cmdIndex] > 0) {
            --suppressBegin_[cmdIndex];
            return;
        }
    } else if (phase == CMD_PHASE_END) {
        if (suppressEnd_.size() > cmdIndex && suppressEnd_[cmdIndex] > 0) {
            --suppressEnd_[cmdIndex];
            return;
        }
    }
    cmdEvents_.emplace_back(cmdIndex, phase);
}

}
