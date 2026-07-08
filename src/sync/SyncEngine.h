#pragma once

#include "DatarefRegistry.h"
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

private:
    DatarefRegistry* reg_     = nullptr;
    Session*         session_ = nullptr;

    struct Cache {
        DrValue value;
        bool    echoSuppressed = false;
        bool    cmdPending     = false;
    };
    std::vector<Cache> cache_;
    std::vector<bool>  cmdPending_;

    DrValue readDr(const RegisteredDataref& rd) const;
    void    writeDr(const RegisteredDataref& rd, const DrValue& val);
};

}
