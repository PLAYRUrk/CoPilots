#pragma once

#include "DatarefRegistry.h"
#include "AutoDatarefSync.h"
#include "../session/Session.h"
#include "../net/Protocol.h"
#include <vector>
#include <functional>
#include <cstdint>
#include <variant>
#include <string>

namespace cp {

struct DrValue {
    DrType type = DrType::UNKNOWN;
    int    i    = 0;
    float  f    = 0.f;
    double d    = 0.0;
    std::vector<int>   ia;
    std::vector<float> fa;
    std::vector<uint8_t> ba;

    bool approxEqual(const DrValue& o) const;
};

using DrChangedCb = std::function<void(uint16_t drIndex, const DrValue& val)>;
using CmdFiredCb  = std::function<void(uint16_t cmdIndex)>;

class SyncEngine {
public:
    void init(DatarefRegistry* reg, Session* session);

    void tick(DrChangedCb onChanged, CmdFiredCb onCmd);

    void applyIncoming(uint16_t drIndex, const DrValue& val, uint8_t senderParticipantId);

    void applyIncomingCommand(uint16_t cmdIndex, uint8_t senderParticipantId);

    void notifyCommandFired(uint16_t cmdIndex);

    void reset();

    // Re-read all datarefs into the cache without sending anything. Call this after
    // external writes (applyState, SASL callbacks) so SyncEngine doesn't treat those
    // writes as local user changes on the next tick. Echo-suppressed entries are skipped
    // so an in-flight echo suppression from applyIncoming() is not clobbered.
    void refreshCache();

    // Force all datarefs to be sent on the next tick (used when a new client joins)
    void requestFullSync() { fullSyncPending_ = true; }

    // In SmartCopilot mode any participant sends datarefs they own; updates from
    // any other participant are accepted.  Zone-based exclusivity applies only to
    // flight controls (PhysicsSync via UDP), not cockpit switches.  The suppress
    // window below acts as the last-writer-wins arbiter to kill ping-pong.
    void setSmartCopilotMode(bool v) { smartCopilotMode_ = v; }

private:
    bool fullSyncPending_   = false;
    bool smartCopilotMode_  = false;
    DatarefRegistry* reg_     = nullptr;
    Session*         session_ = nullptr;

    struct Cache {
        DrValue value;
        // After applyIncoming() writes a value, we suppress outbound sends for this
        // many frames so SASL (or X-Plane) has time to settle before we re-read and
        // compare.  During suppression, the cache tracks the settling value so that
        // when the window expires, the first comparison is against the final value
        // and no spurious retransmit occurs.
        int     suppressFrames = 0;
        bool    cmdPending     = false;
    };
    std::vector<Cache> cache_;
    std::vector<bool>  cmdPending_;
    std::vector<bool>  cmdEchoSuppressed_;

    DrValue readDr(const RegisteredDataref& rd) const;
    void    writeDr(const RegisteredDataref& rd, const DrValue& val);
};

}
