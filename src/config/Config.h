#pragma once

#include "Zone.h"
#include <string>
#include <vector>

namespace cp {

struct AircraftConfig {
    std::string             name;
    uint16_t                port      = 56900;
    bool                    fromSmartCopilot = false;

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

    const AircraftConfig& get() const { return cfg_; }
    bool                  loaded() const { return loaded_; }

    void reset();

private:
    AircraftConfig cfg_;
    bool           loaded_ = false;

    bool loadJson(const std::string& path);
};

}
