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
};

struct CommandEntry {
    std::string path;
    std::string zoneId;
};

}
