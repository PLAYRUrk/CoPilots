#pragma once

#include <string>
#include <vector>
#include <unordered_set>

namespace cp {

struct Zone {
    std::string id;
    std::string name;
};

struct Role {
    std::string id;
    std::string name;
    std::vector<std::string> zoneIds;

    bool ownsZone(const std::string& zoneId) const {
        for (const auto& z : zoneIds)
            if (z == zoneId) return true;
        return false;
    }
};

enum class SyncMode {
    ONCHANGE,
    CONTINUOUS,
    COMMAND,
    // Sent ONLY during a join-time full resync (host → new client), never on
    // change detection.  For switch-position datarefs whose runtime channel is a
    // toggle COMMAND: aligns a joining client's switches with the host so that
    // subsequently relayed toggles flip everyone the same way, without creating
    // a runtime dataref+command double channel (write wars, echo overshoot).
    SNAPSHOT,
};

struct DatarefEntry {
    std::string path;
    std::string zoneId;
    SyncMode    mode;
};

struct CommandEntry {
    std::string path;
    std::string zoneId;
};

}
