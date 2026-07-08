#pragma once
// SmartCopilotCompat.h — parses a smartcopilot.cfg file and converts it to an
// AircraftConfig with a single zone "SHARED" (zone-based features unavailable).

#include "Zone.h"
#include <string>

namespace cp {

struct AircraftConfig;

class SmartCopilotCompat {
public:
    // Returns false if the file cannot be opened or has no recognisable content.
    static bool parse(const std::string& filePath, AircraftConfig& out);
};

} // namespace cp
