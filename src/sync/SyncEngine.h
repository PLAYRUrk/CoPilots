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
using CmdFiredCb  = std::function<void(uint16_t cmdIndex, uint8_t phase)>;

class SyncEngine {
public:
    void init(DatarefRegistry* reg, Session* session);

    void tick(DrChangedCb onChanged, CmdFiredCb onCmd);

    void applyIncoming(uint16_t drIndex, const DrValue& val, uint8_t senderParticipantId);

    void applyIncomingCommand(uint16_t cmdIndex, uint8_t phase, uint8_t senderParticipantId);

    void notifyCommandFired(uint16_t cmdIndex, uint8_t phase);

    void reset();

    // Re-read all datarefs into the cache without sending anything. Call this after
    // external writes (applyState, SASL callbacks) so SyncEngine doesn't treat those
    // writes as local user changes on the next tick.
    void refreshCache();

    // Force all datarefs to be sent on the next tick (used when a new client joins)
    void requestFullSync() { fullSyncPending_ = true; }

    // In SmartCopilot mode any participant sends datarefs they own; updates from
    // any other participant are accepted.  Zone-based exclusivity applies only to
    // flight controls (PhysicsSync via UDP), not cockpit switches.  The suppress
    // window below acts as the last-writer-wins arbiter to kill ping-pong.
    void setSmartCopilotMode(bool v) { smartCopilotMode_ = v; }

private:
    // CONTINUOUS datarefs: heartbeat period in flight-loop frames (~0.33 s at 60 fps).
    // Each dataref's heartbeat is staggered by its index so the load spreads evenly.
    static constexpr uint32_t CONT_HEARTBEAT = 20;

    bool fullSyncPending_   = false;
    bool smartCopilotMode_  = false;
    uint32_t tickCounter_   = 0;
    DatarefRegistry* reg_     = nullptr;
    Session*         session_ = nullptr;

    struct Cache {
        DrValue value;          // last sent/received value (authoritative)
        DrValue lastApplied;    // last value written from the network (for echo detection)
        // Value this dataref held BEFORE the last network write.  If the aircraft's
        // own logic (SASL/Lua) rejects our write and restores this value, the revert
        // must be absorbed silently — echoing it back would snap the sender's switch
        // back (write-war with the aircraft's internal state).
        DrValue preApply;
        // Short window (frames) during which small deviations from lastApplied are
        // treated as SASL quantization noise rather than real user input.
        // Only used for FLOAT/DOUBLE/array types; INT is exact so no window needed.
        int     echoFrames = 0;
        // Frames elapsed since this side last made a real local change.
        // Used by the reconciliation path to avoid overwriting recent user input.
        int     framesSinceLocalChange = 9999;
        bool    cmdPending     = false;
    };
    std::vector<Cache> cache_;
    // Ordered queue of locally fired command edges (index, phase) awaiting send.
    std::vector<std::pair<uint16_t, uint8_t>> cmdEvents_;
    // Per-command echo suppression counters: applying an incoming Begin/End fires
    // our own registered handler synchronously; those self-echoes must not be
    // re-queued for sending.
    std::vector<uint8_t> suppressBegin_;
    std::vector<uint8_t> suppressEnd_;
    // Commands currently held down BY THE NETWORK (XPLMCommandBegin issued without
    // a matching End yet).  Released in reset() so a disconnect mid-hold does not
    // leave a command stuck down forever.
    std::vector<bool>    heldByNet_;
    // Frames since the last Begin (or Begin-refresh keepalive) arrived for a
    // net-held command.  If no keepalive arrives within NET_HOLD_TIMEOUT_FRAMES the
    // hold is force-released: a lost/eaten End edge (e.g. Begin/End pairs corrupted
    // by aircraft Lua cross-firing a sibling command) then self-heals in ~2 s
    // instead of leaving a trim/test switch running forever.
    std::vector<int>     netHoldFrames_;
    // Commands currently held down BY THE LOCAL USER (a non-suppressed Begin edge
    // was seen without its End yet).  While held, a Begin keepalive is re-sent
    // every HOLD_REFRESH_FRAMES so receivers keep the hold alive past the timeout.
    std::vector<bool>    userHeld_;

    // Held-command keepalive tuning (frames at ~60 fps).
    static constexpr int HOLD_REFRESH_FRAMES    = 30;   // re-send Begin every ~0.5 s
    static constexpr int NET_HOLD_TIMEOUT_FRAMES = 120; // force-release after ~2 s

    DrValue readDr(const RegisteredDataref& rd) const;
    void    writeDr(const RegisteredDataref& rd, const DrValue& val);
};

}
