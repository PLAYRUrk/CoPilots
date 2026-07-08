#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>

namespace cp {

using ParticipantId = uint8_t;
constexpr ParticipantId INVALID_PARTICIPANT_ID = 0xFF;

struct Participant {
    ParticipantId         id   = INVALID_PARTICIPANT_ID;
    std::string           nick;
    std::string           roleId;
    std::vector<std::string> zoneIds;
    bool                  isPhysicsMaster  = false;
    bool                  isWeatherMaster  = false;
    uint32_t              ping_ms = 0;

    bool ownsZone(const std::string& zoneId) const {
        for (const auto& z : zoneIds)
            if (z == zoneId) return true;
        return false;
    }
};

}
