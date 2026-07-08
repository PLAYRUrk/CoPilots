#pragma once

#include "../config/Zone.h"
#include <string>
#include <vector>
#include <unordered_set>

namespace cp {

// Zone ID used for auto-discovered datarefs.
// Only the physics master sends datarefs in this zone; all others receive them.
constexpr const char* AUTO_ZONE_ID = "_AUTO";

class AutoDatarefSync {
public:
    // Parse X-Plane's DataRefs.txt and return DatarefEntries for every writable,
    // non-excluded dataref not already present in `existing`.
    //
    // xplanePath: value returned by XPLMGetSystemPath() — ends with a path separator.
    // existing:   paths already registered by the manual config (deduplicated out).
    static std::vector<DatarefEntry> discover(
        const std::string& xplanePath,
        const std::unordered_set<std::string>& existing);

private:
    static bool isAllowedPath(const std::string& path);
};

} // namespace cp
