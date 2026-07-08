#include "AutoDatarefSync.h"
#include "../Log.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>

namespace cp {

// Only auto-sync datarefs inside these top-level namespaces.
// Everything else (sim/flightmodel/, sim/graphics/, sim/operation/override/, etc.)
// is either computed by the physics engine, view-local, or managed by PhysicsSync.
static const char* ALLOWED_PREFIXES[] = {
    "sim/cockpit/",
    "sim/cockpit2/",
    "sim/GPS/",
    nullptr
};

// Excluded sub-patterns within allowed namespaces: these are read-only computed
// outputs (engine gauges, nav indicators, etc.) that the sim recalculates every
// frame from the physical state — syncing them is redundant and wastes bandwidth.
static const char* BLOCKED_SUBS[] = {
    "/indicators/",
    "/gauges/",
    "/outputs/",
    nullptr
};

bool AutoDatarefSync::isAllowedPath(const std::string& path)
{
    bool inAllowed = false;
    for (int i = 0; ALLOWED_PREFIXES[i]; ++i) {
        if (path.compare(0, strlen(ALLOWED_PREFIXES[i]), ALLOWED_PREFIXES[i]) == 0) {
            inAllowed = true;
            break;
        }
    }
    if (!inAllowed) return false;

    for (int i = 0; BLOCKED_SUBS[i]; ++i) {
        if (path.find(BLOCKED_SUBS[i]) != std::string::npos)
            return false;
    }
    return true;
}

std::vector<DatarefEntry> AutoDatarefSync::discover(
    const std::string& xplanePath,
    const std::unordered_set<std::string>& existing)
{
    std::vector<DatarefEntry> result;
    std::unordered_set<std::string> seen(existing);

    // X-Plane ships DataRefs.txt in a few possible locations depending on version.
    const std::string candidates[] = {
        xplanePath + "Resources/plugins/DataRefs.txt",
        xplanePath + "Resources/DataRefs.txt",
        // Windows path separator variant
        xplanePath + "Resources\\plugins\\DataRefs.txt",
    };

    std::ifstream f;
    std::string usedPath;
    for (const auto& c : candidates) {
        f.open(c);
        if (f.is_open()) { usedPath = c; break; }
    }
    if (!f.is_open()) {
        LogWarning("AutoDatarefSync: DataRefs.txt not found under '%s'", xplanePath.c_str());
        return result;
    }

    int linesParsed = 0, added = 0;
    std::string line;

    while (std::getline(f, line)) {
        // Strip Windows CR
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Strip UTF-8 BOM on first line
        if (line.size() >= 3 &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = line.substr(3);

        // Skip empty lines and comment lines
        size_t nonSpace = line.find_first_not_of(" \t");
        if (nonSpace == std::string::npos) continue;
        char fc = line[nonSpace];
        if (fc == '/' || fc == '#' || fc == ';') continue;

        // ── Field extraction ────────────────────────────────────────────────
        // DataRefs.txt uses tab-separated fields. Some older/alternative formats
        // use spaces. We try tabs first; if we get only one field, fall back to
        // splitting on any whitespace.
        std::vector<std::string> fields;
        {
            std::istringstream ss(line);
            std::string tok;
            while (std::getline(ss, tok, '\t')) {
                // trim inline
                size_t a = tok.find_first_not_of(" \t");
                size_t b = tok.find_last_not_of(" \t");
                if (a != std::string::npos)
                    fields.push_back(tok.substr(a, b - a + 1));
            }
        }
        if (fields.size() < 2) {
            fields.clear();
            std::istringstream ss(line);
            std::string tok;
            while (ss >> tok) fields.push_back(tok);
        }
        if (fields.size() < 2) continue;

        // ── Locate the dataref path ─────────────────────────────────────────
        // Some formats have a leading single-character section code (e.g. "A").
        // The path is the first token that contains multiple '/'.
        std::string path;
        size_t pathIdx = 0;
        for (size_t i = 0; i < std::min(fields.size(), size_t(3)); ++i) {
            if (fields[i].find('/') != std::string::npos) {
                path = fields[i];
                pathIdx = i;
                break;
            }
        }
        if (path.empty()) continue;

        // Strip per-element array suffix: "sim/foo/bar[3]" → "sim/foo/bar"
        {
            size_t bracket = path.find('[');
            if (bracket != std::string::npos) path.resize(bracket);
        }

        // Must have at least two slash-separated segments to be a real dataref
        if (std::count(path.begin(), path.end(), '/') < 2) continue;

        ++linesParsed;

        // ── Writable flag ───────────────────────────────────────────────────
        // Look for a standalone "y" (or "ya", "yw") token in the 1–3 fields
        // that follow the path. Formats seen:
        //   path  float[8]  y  description
        //   path  float     y
        //   path  float     ya  (y=writable, a=some other flag)
        bool writable = false;
        size_t searchEnd = std::min(fields.size(), pathIdx + 4);
        for (size_t i = pathIdx + 1; i < searchEnd; ++i) {
            const std::string& fi = fields[i];
            // Type field (e.g. "float[8]", "int", "double") — skip
            if (fi.find('[') != std::string::npos) continue;
            if (fi == "float" || fi == "int" || fi == "double" ||
                fi == "byte"  || fi == "data") continue;

            if (!fi.empty()) {
                char flag = fi[0];
                if (flag == 'y') { writable = true;  break; }
                if (flag == 'n') { writable = false; break; }
            }
        }
        if (!writable) continue;

        // ── Namespace + blocklist filter ────────────────────────────────────
        if (!isAllowedPath(path)) continue;

        // ── Deduplication ───────────────────────────────────────────────────
        if (seen.count(path)) continue;
        seen.insert(path);

        DatarefEntry e;
        e.path   = std::move(path);
        e.zoneId = AUTO_ZONE_ID;
        e.mode   = SyncMode::ONCHANGE;
        result.push_back(std::move(e));
        ++added;
    }

    Log("AutoDatarefSync: scanned %d lines in '%s', added %d writable datarefs",
        linesParsed, usedPath.c_str(), added);
    return result;
}

} // namespace cp
