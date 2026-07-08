#include "Config.h"
#include "SmartCopilotCompat.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

namespace cp {

static SyncMode parseSyncMode(const std::string& s)
{
    if (s == "continuous")  return SyncMode::CONTINUOUS;
    if (s == "onchange")    return SyncMode::ONCHANGE;
    if (s == "command")     return SyncMode::COMMAND;
    return SyncMode::ONCHANGE;
}

const Zone* AircraftConfig::findZone(const std::string& id) const
{
    for (const auto& z : zones)
        if (z.id == id) return &z;
    return nullptr;
}

const Role* AircraftConfig::findRole(const std::string& id) const
{
    for (const auto& r : roles)
        if (r.id == id) return &r;
    return nullptr;
}

void Config::reset()
{
    cfg_    = AircraftConfig{};
    loaded_ = false;
}

bool Config::load(const std::string& aircraftDir)
{
    reset();

    std::string jsonPath = aircraftDir + "/copilots.json";
    {
        std::ifstream f(jsonPath);
        if (f.good()) {
            f.close();
            if (loadJson(jsonPath)) {
                loaded_ = true;
                Log("Config: loaded copilots.json for '%s'", cfg_.name.c_str());
                return true;
            }
        }
    }

    std::string scPath = aircraftDir + "/smartcopilot.cfg";
    {
        std::ifstream f(scPath);
        if (f.good()) {
            f.close();
            if (SmartCopilotCompat::parse(scPath, cfg_)) {
                cfg_.fromSmartCopilot = true;
                loaded_ = true;
                Log("Config: loaded smartcopilot.cfg fallback (zones disabled)");
                return true;
            }
        }
    }

    LogWarning("Config: no copilots.json or smartcopilot.cfg found in '%s'",
               aircraftDir.c_str());
    return false;
}

bool Config::loadJson(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        LogError("Config: cannot open '%s'", path.c_str());
        return false;
    }

    try {
        json j;
        f >> j;

        cfg_.name = j.value("aircraft", "Unknown Aircraft");
        cfg_.port = j.value("port", 56900);

        for (const auto& jz : j.value("zones", json::array())) {
            Zone z;
            z.id   = jz.at("id").get<std::string>();
            z.name = jz.at("name").get<std::string>();
            cfg_.zones.push_back(std::move(z));
        }

        for (const auto& jr : j.value("roles", json::array())) {
            Role r;
            r.id   = jr.at("id").get<std::string>();
            r.name = jr.at("name").get<std::string>();
            for (const auto& zid : jr.value("zones", json::array()))
                r.zoneIds.push_back(zid.get<std::string>());
            cfg_.roles.push_back(std::move(r));
        }

        for (const auto& jd : j.value("datarefs", json::array())) {
            DatarefEntry d;
            d.path   = jd.at("path").get<std::string>();
            d.zoneId = jd.at("zone").get<std::string>();
            d.mode   = parseSyncMode(jd.value("mode", "onchange"));
            cfg_.datarefs.push_back(std::move(d));
        }

        for (const auto& jc : j.value("commands", json::array())) {
            CommandEntry c;
            c.path   = jc.at("path").get<std::string>();
            c.zoneId = jc.at("zone").get<std::string>();
            cfg_.commands.push_back(std::move(c));
        }

        return true;
    } catch (const std::exception& ex) {
        LogError("Config: JSON parse error in '%s': %s", path.c_str(), ex.what());
        return false;
    }
}

}
