#pragma once
#include "ChartTypes.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace cp {
namespace chart {

// One hand-made calibration.  The georeference itself is a plain ChartGeoref —
// the same parameterisation ChartFox publishes — which makes a hand-made
// calibration and a supplied georeference interchangeable in every consumer.
// The rest is bookkeeping the shared database needs.
struct CalEntry {
    ChartGeoref g;
    std::string icao;                // airport this chart belongs to
    bool        isOverride = false;  // made on top of a ChartFox georeference
    int64_t     mtime      = 0;      // unix seconds — newer wins when merging
    bool        shared     = false;  // already uploaded to the community database
};

// Where the georeference in force came from (for the status line under a chart).
enum class CalOrigin { None, Own, Community, ChartFox };

// ---------------------------------------------------------------------------
// Hand-made chart calibrations, ours and the community's.
//
// Roughly three quarters of the charts ChartFox serves carry no georeference at
// all, so the crew can pin one down themselves from two reference points (a
// navaid, the aircraft's own position, or typed coordinates).  Those numbers
// are relative to the page, and every client renders the same page of the same
// chart, so they are portable: over the session (CF_CAL_SET) and through the
// shared database on GitHub (ChartCalCloud).
//
// Two maps, both keyed "<chartId>:<page>" with page 1-based (as ChartFox
// numbers them):
//   own_        — made here or received from the crew; Output/CoPilots/chart_calibrations.json
//   community_  — downloaded from the project repository, cached per airport
//
// Lookup order is deliberate (see findOverriding / findFallback): our own
// calibration always wins, a community entry that was made to correct a wrong
// ChartFox georeference beats ChartFox, and an ordinary community entry only
// fills in where there is no georeference at all.
// ---------------------------------------------------------------------------
class ChartCalStore {
public:
    // ownFile is the JSON with our calibrations (a missing file is fine);
    // cacheDir holds one <ICAO>.json per airport of downloaded community data.
    void init(const std::string& ownFile, const std::string& cacheDir);

    // Ours, else a community entry that was made over a ChartFox georeference.
    const CalEntry* findOverriding(const std::string& chartId, int page1) const;
    // A community entry that only applies where nothing else does.
    const CalEntry* findFallback(const std::string& chartId, int page1) const;
    // Ours alone — what the Calibrate / Clear / Share buttons act on.
    const CalEntry* findOwn(const std::string& chartId, int page1) const;

    void set(const std::string& chartId, int page1, const ChartGeoref& g,
             const std::string& icao, bool isOverride, int64_t mtime = 0);
    void erase(const std::string& chartId, int page1);

    // An older file (or a crewmate on an older build) carries no airport code;
    // fill it in once the chart is known to belong to one.  Returns true when
    // something changed.
    bool backfillIcao(const std::string& chartId, const std::string& icao);

    // Replace everything we hold for one airport with a freshly fetched set.
    void setCommunity(const std::string& icao, std::map<std::string, CalEntry> entries,
                      bool writeCache);
    // Load <cacheDir>/<ICAO>.json into community_ (offline path).
    bool loadCommunityCache(const std::string& icao);

    const std::map<std::string, CalEntry>& own() const { return own_; }
    // Our calibrations that have an airport and have not been uploaded yet.
    std::vector<std::pair<std::string, CalEntry>> unshared() const;
    void markShared(const std::vector<std::string>& keys);

    static std::string keyFor(const std::string& chartId, int page1);
    // Splits "<chartId>:<page>"; returns false when the key is malformed.
    static bool splitKey(const std::string& key, std::string& chartId, int& page1);
    // The one on-disk/on-wire format: a JSON object of key → entry.  Used for
    // our file, the community cache and the files in the repository.
    static bool parseEntries(const std::string& text,
                             std::map<std::string, CalEntry>& out, std::string& err);
    static std::string dumpEntries(const std::map<std::string, CalEntry>& entries);

private:
    void save() const;
    std::string cachePathFor(const std::string& icao) const;

    std::map<std::string, CalEntry> own_;
    std::map<std::string, CalEntry> community_;
    std::string path_;
    std::string cacheDir_;
};

}
}
