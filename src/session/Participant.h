#pragma once
// Participant.h — a single connected crew member.

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

namespace cp {

using ParticipantId = uint8_t;
constexpr ParticipantId INVALID_PARTICIPANT_ID = 0xFF;

struct Participant {
    ParticipantId         id   = INVALID_PARTICIPANT_ID;
    std::string           nick;             // display name chosen by user
    std::string           roleId;           // assigned role id (or "" = none)
    std::vector<std::string> zoneIds;       // effective zones (from role + overrides)
    bool                  isPhysicsMaster = false;
    uint32_t              ping_ms = 0;      // last measured round-trip ping

    bool ownsZone(const std::string& zoneId) const {
        for (const auto& z : zoneIds)
            if (z == zoneId) return true;
        return false;
    }
};

} // namespace cp
