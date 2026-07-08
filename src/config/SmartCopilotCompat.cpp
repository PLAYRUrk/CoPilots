#include "SmartCopilotCompat.h"
#include "Config.h"
#include "../Log.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace cp {

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b-a+1);
}

static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    return s;
}

bool SmartCopilotCompat::parse(const std::string& filePath, AircraftConfig& out)
{
    std::ifstream f(filePath);
    if (!f.is_open()) return false;

    out.name  = "Unknown (smartcopilot.cfg)";
    out.port  = 56900;

    Zone shared;
    shared.id   = "SHARED";
    shared.name = "Shared (SmartCopilot fallback)";
    out.zones.push_back(shared);

    Role defaultRole;
    defaultRole.id   = "pilot";
    defaultRole.name = "Pilot";
    defaultRole.zoneIds.push_back("SHARED");
    out.roles.push_back(defaultRole);

    enum class Section { NONE, CONTINUED, CLICKS, OVERRIDE } section = Section::NONE;

    std::string line;
    int lineNo = 0;

    while (std::getline(f, line)) {
        ++lineNo;
        line = trim(line);

        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        std::string lower = toLower(line);
        if (lower == "[continued]")  { section = Section::CONTINUED; continue; }
        if (lower == "[clicks]")     { section = Section::CLICKS;     continue; }
        if (lower == "[override]")   { section = Section::OVERRIDE;   continue; }
        if (line[0] == '[')          { section = Section::NONE;       continue; }

        if (section == Section::NONE) continue;

        std::string path;
        {
            size_t pos = line.find_first_of(",= \t");
            path = (pos == std::string::npos) ? line : trim(line.substr(0, pos));
        }
        if (path.empty()) continue;

        if (section == Section::CLICKS) {
            CommandEntry cmd;
            cmd.path   = path;
            cmd.zoneId = "SHARED";
            bool dup = false;
            for (const auto& c : out.commands)
                if (c.path == path) { dup = true; break; }
            if (!dup) out.commands.push_back(std::move(cmd));
        } else if (section == Section::CONTINUED || section == Section::OVERRIDE) {
            DatarefEntry dr;
            dr.path   = path;
            dr.zoneId = "SHARED";
            dr.mode   = (section == Section::CONTINUED) ? SyncMode::CONTINUOUS
                                                         : SyncMode::ONCHANGE;
            bool dup = false;
            for (const auto& d : out.datarefs)
                if (d.path == path) { dup = true; break; }
            if (!dup) out.datarefs.push_back(std::move(dr));
        }
    }

    Log("SmartCopilotCompat: parsed %zu datarefs, %zu commands from '%s'",
        out.datarefs.size(), out.commands.size(), filePath.c_str());
    return !out.datarefs.empty() || !out.commands.empty();
}

}
