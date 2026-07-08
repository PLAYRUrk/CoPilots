#pragma once
// Config.h — loads copilots.json (or falls back to smartcopilot.cfg).

#include "Zone.h"
#include <string>
#include <vector>

namespace cp {

struct AircraftConfig {
    std::string             name;
    uint16_t                port      = 56900;
    bool                    fromSmartCopilot = false;  // loaded via compat fallback

    std::vector<Zone>        zones;
    std::vector<Role>        roles;
    std::vector<DatarefEntry> datarefs;
    std::vector<CommandEntry> commands;

    // Helpers
    const Zone* findZone(const std::string& id) const;
    const Role* findRole(const std::string& id) const;
};

class Config {
public:
    // Try copilots.json, then smartcopilot.cfg in aircraftDir.
    // Returns false if neither found / parse errors are fatal.
    bool load(const std::string& aircraftDir);

    const AircraftConfig& get() const { return cfg_; }
    bool                  loaded() const { return loaded_; }

    // Clear (e.g. aircraft change)
    void reset();

private:
    AircraftConfig cfg_;
    bool           loaded_ = false;

    bool loadJson(const std::string& path);
};

} // namespace cp
