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
};

struct DatarefEntry {
    std::string path;
    std::string zoneId;
    SyncMode    mode;
    // Optional: command fired on the RECEIVER when a written value is reverted by
    // the aircraft's own logic (Lua-owned state like Zibo's parking brake).  The
    // toggle flips the aircraft's internal state so it converges to the wire value.
    std::string toggleCmd;
};

struct CommandEntry {
    std::string path;
    std::string zoneId;
};

}
