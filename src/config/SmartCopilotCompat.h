#pragma once

#include "Zone.h"
#include <string>

namespace cp {

struct AircraftConfig;

class SmartCopilotCompat {
public:
    static bool parse(const std::string& filePath, AircraftConfig& out);
};

}
