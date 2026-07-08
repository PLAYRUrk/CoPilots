#pragma once
// SyncEngine.h — detects changed datarefs, applies incoming values, enforces authority.
//
// Rules:
//   1. Only write outbound if I own the zone.
//   2. Only apply incoming value if sender owns the zone.
//   3. Echo-suppression: when applying an incoming value, mark it so we don't re-send it.

#include "DatarefRegistry.h"
#include "../session/Session.h"
#include "../net/Protocol.h"
#include <vector>
#include <functional>
#include <cstdint>
#include <variant>
#include <string>

namespace cp {

// A serialised dataref value (what goes on the wire and what comes off it)
struct DrValue {
    DrType type = DrType::UNKNOWN;
    int    i    = 0;
    float  f    = 0.f;
    double d    = 0.0;
    std::vector<int>   ia;
    std::vector<float> fa;
    std::vector<uint8_t> ba;

    // Approximate equality check (to detect changes)
    bool approxEqual(const DrValue& o) const;
};

// Callback: a dataref changed and needs to be sent to the network
using DrChangedCb = std::function<void(uint16_t drIndex, const DrValue& val)>;
using CmdFiredCb  = std::function<void(uint16_t cmdIndex)>;

class SyncEngine {
public:
    void init(DatarefRegistry* reg, Session* session);

    // Called from flight loop (main thread), ~20 Hz.
    // Checks for changed datarefs and fires callbacks for ones we own.
    void tick(DrChangedCb onChanged, CmdFiredCb onCmd);

    // Apply an incoming dataref value from the network (authority-checked).
    // sender = participant id of the originator.
    void applyIncoming(uint16_t drIndex, const DrValue& val, uint8_t senderParticipantId);

    // Apply an incoming command fire from the network (authority-checked).
    void applyIncomingCommand(uint16_t cmdIndex, uint8_t senderParticipantId);

    // Signal that a command was locally fired (e.g. user clicked something in our zone)
    void notifyCommandFired(uint16_t cmdIndex);

    void reset();

private:
    DatarefRegistry* reg_     = nullptr;
    Session*         session_ = nullptr;

    // Last known values (for change detection)
    struct Cache {
        DrValue value;
        bool    echoSuppressed = false; // true for one tick after applying incoming
        bool    cmdPending     = false; // command to send this tick
    };
    std::vector<Cache> cache_;
    std::vector<bool>  cmdPending_;  // one flag per registered command

    DrValue readDr(const RegisteredDataref& rd) const;
    void    writeDr(const RegisteredDataref& rd, const DrValue& val);
};

} // namespace cp
