#pragma once
// Zone.h — Zone and Role definitions.
// A Zone is a functional group of cockpit controls (e.g. MCP, OVERHEAD).
// A Role is a named set of zones assigned to a crew member.

#include <string>
#include <vector>
#include <unordered_set>

namespace cp {

struct Zone {
    std::string id;    // unique identifier, e.g. "MCP"
    std::string name;  // display name, e.g. "Mode Control Panel / AFS"
};

struct Role {
    std::string id;    // unique identifier, e.g. "captain"
    std::string name;  // display name, e.g. "Captain (PF)"
    std::vector<std::string> zoneIds;  // zones this role owns

    bool ownsZone(const std::string& zoneId) const {
        for (const auto& z : zoneIds)
            if (z == zoneId) return true;
        return false;
    }
};

// How a dataref/command is synchronised
enum class SyncMode {
    ONCHANGE,    // sent only when value changes (toggles, mode switches)
    CONTINUOUS,  // sent every sync tick regardless (knobs, analog values)
    COMMAND,     // one-shot command (clicked button)
};

// Config entry for a single dataref to sync
struct DatarefEntry {
    std::string path;     // e.g. "sim/cockpit/autopilot/heading_mag"
    std::string zoneId;   // owning zone
    SyncMode    mode;
    // value type (resolved at runtime via XPLM)
};

// Config entry for a command to sync
struct CommandEntry {
    std::string path;    // e.g. "sim/autopilot/NAV"
    std::string zoneId;
};

} // namespace cp
