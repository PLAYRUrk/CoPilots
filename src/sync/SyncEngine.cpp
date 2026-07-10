#include "SyncEngine.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>
#include <cmath>
#include <cstring>

namespace cp {

static constexpr float FLOAT_EPS   = 1e-5f;
static constexpr double DOUBLE_EPS = 1e-9;

// Returns true for datarefs that PhysicsSync manages exclusively via UDP.
// Syncing these via TCP as well would cause UDP writes on clients to be
// echoed back to the master, creating an oscillation that freezes the yoke.
static bool isPhysicsOnlyDr(const std::string& path)
{
    // Use strlen so the compiler derives the correct length from the string
    // literal — avoids the off-by-one bugs that arise from hand-counted magic numbers.
    auto startsWith = [&](const char* prefix) {
        size_t n = std::strlen(prefix);
        return path.size() >= n && path.compare(0, n, prefix, n) == 0;
    };
    if (startsWith("sim/joystick/yoke_"))           return true;  // 18 chars
    if (startsWith("sim/cockpit2/controls/yoke_"))  return true;  // 27 chars
    if (startsWith("sim/custom/controlls/yoke_"))   return true;  // 26 chars
    if (startsWith("sim/custom/SC/yoke_"))          return true;  // 19 chars
    if (path == "sim/operation/override/override_joystick")  return true;
    if (path == "sim/operation/override/override_planepath") return true;
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
        if (rd.handle && cache_[i].suppressFrames == 0)
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

        // Zone ownership:
        //   _AUTO zone   → any participant sends; cockpit switches are not flight controls
        //   SmartCopilot → any participant sends (same as original smartcopilot.cfg behaviour)
        //   normal       → only the participant who owns the zone sends
        // Flight-control exclusivity (yoke, pedals, position) is enforced by PhysicsSync
        // via UDP — those datarefs are outside the _AUTO namespace and not in DataRefs.txt.
        bool iOwn;
        bool isAutoZone = (rd.zoneId == AUTO_ZONE_ID);

        // Yoke and override datarefs are owned exclusively by PhysicsSync (UDP).
        // Sending them via TCP as well causes the UDP write on clients to be picked
        // up as a "local change" on the next tick and echoed back, oscillating the yoke.
        if ((isAutoZone || smartCopilotMode_) && isPhysicsOnlyDr(rd.path))
            continue;
        if (isAutoZone || smartCopilotMode_)
            iOwn = true;
        else
            iOwn = session_->iOwnZone(rd.zoneId);
        if (!iOwn) continue;
        ++ownCount;

        Cache& c = cache_[i];

        if (c.suppressFrames > 0) {
            // While the suppress window is active, keep the cache up-to-date with the
            // settling value so no spurious send occurs when the window expires.
            --c.suppressFrames;
            if (rd.handle) c.value = readDr(rd);
            continue;
        }

        DrValue cur = readDr(rd);

        bool changed = !cur.approxEqual(c.value);
        // CONTINUOUS mode only applies to manual-config datarefs in normal mode;
        // _AUTO and SmartCopilot datarefs always use ONCHANGE to avoid flooding TCP.
        bool send    = doFull || (!smartCopilotMode_ && !isAutoZone && rd.mode == SyncMode::CONTINUOUS) || changed;

        if (send) {
            c.value = cur;
            onChanged(static_cast<uint16_t>(i), cur);
            ++sentCount;
        }
    }

    static int tickLog = 0;
    if (++tickLog % 300 == 1)
        Log("SyncEngine::tick own=%d sent=%d/%zu full=%d",
            ownCount, sentCount, drs.size(), (int)doFull);

    const auto& cmds = reg_->commands();
    if (cmdPending_.size() != cmds.size())
        cmdPending_.resize(cmds.size(), false);

    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmdPending_[i]) {
            cmdPending_[i] = false;
            onCmd(static_cast<uint16_t>(i));
        }
    }
}

void SyncEngine::applyIncoming(uint16_t drIndex, const DrValue& val,
                                uint8_t senderParticipantId)
{
    if (!reg_ || !session_) return;
    const RegisteredDataref* rd = reg_->getDr(drIndex);
    if (!rd || !rd->handle) return;

    if (rd->zoneId == AUTO_ZONE_ID || smartCopilotMode_) {
        // _AUTO / SmartCopilot: accept from any other participant.
        // Echo from ourselves is dropped here; the cache echoSuppressed flag handles
        // the case where we just wrote this value ourselves via applyIncoming.
        if (senderParticipantId == session_->myId()) return;

        // Yoke and override datarefs are driven exclusively by PhysicsSync via UDP.
        // Reject TCP copies to prevent the UDP write from being treated as a local
        // change on the next tick and echoed back (which would freeze the yoke at 0).
        if (isPhysicsOnlyDr(rd->path)) return;
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

    static int applyLog = 0;
    if (++applyLog <= 10 || applyLog % 600 == 0) {
        // Log path and value to help diagnose which datarefs flow through TCP
        // (e.g. fire_valve linking, yoke leak) without scanning by index.
        float fval = (val.type == DrType::FLOAT) ? val.f
                   : (val.type == DrType::INT)   ? (float)val.i : 0.f;
        Log("SyncEngine::applyIncoming idx=%u path=%s zone=%s val=%.4f writable=%d sender=%u",
            drIndex, rd->path.c_str(), rd->zoneId.c_str(),
            fval, (int)rd->writable, senderParticipantId);
    }

    // Extra log for yoke/override datarefs to diagnose animation issues
    {
        const std::string& p = rd->path;
        bool isYoke = p.find("yoke") != std::string::npos
                   || p.find("override_joystick") != std::string::npos;
        if (isYoke) {
            float fval = (val.type == DrType::FLOAT) ? val.f
                       : (val.type == DrType::INT)   ? (float)val.i : 0.f;
            static int yokeLog = 0;
            if (++yokeLog <= 20 || yokeLog % 120 == 0)
                Log("SyncEngine::applyIncoming YOKE path=%s val=%.4f sender=%u",
                    p.c_str(), fval, senderParticipantId);
        }
    }

    writeDr(*rd, val);

    if (drIndex < cache_.size()) {
        // Read back the actual value after the write.  SASL may quantise/clamp the value
        // synchronously; storing the post-write readback prevents the slightly different
        // cached value from triggering a retransmit on the next tick.
        cache_[drIndex].value = readDr(*rd);
        // Suppress outbound sends for ~0.5 s (30 frames) so SASL side-effect cascades
        // (fire-valve linking, etc.) have time to settle before we re-read and compare.
        static constexpr int SUPPRESS_WINDOW = 30;
        cache_[drIndex].suppressFrames = SUPPRESS_WINDOW;
    }
}

void SyncEngine::applyIncomingCommand(uint16_t cmdIndex, uint8_t senderParticipantId)
{
    if (!reg_ || !session_) return;
    const RegisteredCommand* rc = reg_->getCmd(cmdIndex);
    if (!rc || !rc->handle) return;

    ParticipantId auth = session_->authorityFor(rc->zoneId);
    if (auth != senderParticipantId && auth != INVALID_PARTICIPANT_ID) return;
    if (senderParticipantId == session_->myId()) return;

    // Suppress the echo: XPLMCommandOnce triggers our registered handler
    // synchronously, which would re-queue the command for re-sending.
    if (cmdEchoSuppressed_.size() <= cmdIndex)
        cmdEchoSuppressed_.resize(cmdIndex + 1, false);
    cmdEchoSuppressed_[cmdIndex] = true;

    XPLMCommandOnce(static_cast<XPLMCommandRef>(rc->handle));
}

void SyncEngine::notifyCommandFired(uint16_t cmdIndex)
{
    if (cmdEchoSuppressed_.size() > cmdIndex && cmdEchoSuppressed_[cmdIndex]) {
        cmdEchoSuppressed_[cmdIndex] = false;
        return;
    }
    if (cmdPending_.size() <= cmdIndex)
        cmdPending_.resize(cmdIndex+1, false);
    cmdPending_[cmdIndex] = true;
}

}
