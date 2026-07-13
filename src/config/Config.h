#pragma once

#include "Zone.h"
#include <string>
#include <vector>

namespace cp {

struct AircraftConfig {
    std::string             name;
    uint16_t                port      = 56900;
    bool                    fromSmartCopilot = false;
    // True when a native copilots.json was found.  applyAutoSync() is a no-op in
    // this case (auto-discovery is only the fallback when no native config exists),
    // unless the JSON sets "autoSync": true.
    bool                    nativeConfig = false;
    // "autoSync": true in copilots.json — run DataRefs.txt auto-discovery IN ADDITION
    // to the manual entries.  Lets aircraft-specific configs cover only the custom
    // (laminar/…) datarefs while standard sim/cockpit2/… switches keep syncing.
    bool                    autoSync = false;
    uint32_t                drListHash = 0; // FNV-1a over ordered dataref paths

    std::vector<Zone>        zones;
    std::vector<Role>        roles;
    std::vector<DatarefEntry> datarefs;
    std::vector<CommandEntry> commands;

    const Zone* findZone(const std::string& id) const;
    const Role* findRole(const std::string& id) const;
};

class Config {
public:
    bool load(const std::string& aircraftDir);

    // Call after load() to append auto-discovered datarefs from DataRefs.txt.
    // xplanePath: value from XPLMGetSystemPath() — ends with a path separator.
    void applyAutoSync(const std::string& xplanePath);

    const AircraftConfig& get() const { return cfg_; }
    bool                  loaded() const { return loaded_; }

    void reset();

private:
    AircraftConfig cfg_;
    bool           loaded_ = false;

    bool loadJson(const std::string& path);
};

}
