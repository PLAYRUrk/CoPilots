#include "SyncEngine.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>
#include <cmath>
#include <cstring>

namespace cp {

static constexpr float FLOAT_EPS   = 1e-5f;
static constexpr double DOUBLE_EPS = 1e-9;

bool DrValue::approxEqual(const DrValue& o) const
{
    if (type != o.type) return false;
    switch (type) {
        case DrType::INT:    return i == o.i;
        case DrType::FLOAT:  return std::fabs(f - o.f) < FLOAT_EPS;
        case DrType::DOUBLE: return std::fabs(d - o.d) < DOUBLE_EPS;
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
        case DrType::INT:    v.i = XPLMGetDatai(rd.handle); break;
        case DrType::FLOAT:  v.f = XPLMGetDataf(rd.handle); break;
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
        case DrType::INT:    XPLMSetDatai(rd.handle, val.i); break;
        case DrType::FLOAT:  XPLMSetDataf(rd.handle, val.f); break;
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
}

void SyncEngine::tick(DrChangedCb onChanged, CmdFiredCb onCmd)
{
    if (!reg_ || !session_) return;

    const auto& drs = reg_->datarefs();
    for (size_t i = 0; i < drs.size(); ++i) {
        const auto& rd = drs[i];
        if (!rd.handle) continue;

        bool iOwn = session_->iOwnZone(rd.zoneId);
        if (!iOwn) continue;

        Cache& c = cache_[i];

        if (c.echoSuppressed) {
            c.echoSuppressed = false;
            continue;
        }

        DrValue cur = readDr(rd);

        bool changed = !cur.approxEqual(c.value);
        bool send    = (rd.mode == SyncMode::CONTINUOUS) || changed;

        if (send) {
            c.value = cur;
            onChanged(static_cast<uint16_t>(i), cur);
        }
    }

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

    ParticipantId auth = session_->authorityFor(rd->zoneId);
    if (auth != senderParticipantId && auth != INVALID_PARTICIPANT_ID) {
        return;
    }

    if (senderParticipantId == session_->myId()) return;

    writeDr(*rd, val);

    if (drIndex < cache_.size()) {
        cache_[drIndex].value          = val;
        cache_[drIndex].echoSuppressed = true;
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

    XPLMCommandOnce(static_cast<XPLMCommandRef>(rc->handle));
}

void SyncEngine::notifyCommandFired(uint16_t cmdIndex)
{
    if (cmdPending_.size() <= cmdIndex)
        cmdPending_.resize(cmdIndex+1, false);
    cmdPending_[cmdIndex] = true;
}

}
