#pragma once

#include "../net/NetThread.h"
#include "../net/Protocol.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cp {

// Shares the flight plans in <X-Plane>/Output/FMS plans/ between all crew
// members, so everyone can load the same route regardless of whose PC it was
// created on.
//
// Protocol (TCP, host relays broadcasts like any other shared message):
//   FMS_LIST    — inventory broadcast: {name, size, hash} per file.  Sent when
//                 the session starts, whenever a rescan finds new local files,
//                 and re-sent when a new participant joins (host sends its list
//                 directly to the joiner; existing clients re-broadcast theirs)
//                 so late joiners learn about pre-existing plans.
//   FMS_REQUEST — names a participant is missing after comparing an inventory.
//                 The host serves what it has and relays only the names it
//                 cannot serve, so at most one extra participant answers for
//                 host-held files (avoids N duplicate broadcasts).
//   FMS_FILE    — one file (name + raw bytes), broadcast so every participant
//                 missing it can save it in one pass.
//
// Safety rules:
//   - only *.fms / *.flp files, max FILE_LIMIT bytes each;
//   - file names are sanitised (no path separators, no leading dot, no
//     Windows reserved device names);
//   - an existing local file is NEVER overwritten — first content wins,
//     locally created plans always take priority on their own machine.
class FmsPlanSync {
public:
    void init(net::NetThread* net, const std::string& xpSystemPath);

    // Call every flight loop while connected; announces the inventory on the
    // first tick of a session and rescans the folder every RESCAN_S seconds.
    void tick(float dt, bool connected);

    // Re-broadcast the full inventory.  target = connId for a host-side
    // targeted send (new joiner), 0xFF for a broadcast (client reacting to
    // PARTICIPANT_JOIN).  Idempotent: receivers request only what they miss.
    void announce(uint8_t target = 0xFF);

    // Wire handlers (called from the plugin's TCP dispatch).
    void onList(proto::MsgReader& r);
    // If `unserved` is non-null (host side), names that could not be served
    // locally are appended to it so the host can relay a reduced request.
    void onRequest(proto::MsgReader& r, std::vector<std::string>* unserved = nullptr);
    void onFile(proto::MsgReader& r);

    void reset();

private:
    struct FileInfo {
        std::string name;
        uint32_t    size = 0;
        uint32_t    hash = 0;
    };
    // Disk cache: skip re-reading/re-hashing files whose size+mtime match.
    struct HashCacheEntry {
        uint64_t size  = 0;
        int64_t  mtime = 0;
        uint32_t hash  = 0;
    };

    static constexpr uint32_t FILE_LIMIT  = 256 * 1024;  // per-file size cap
    static constexpr int      MAX_FILES   = 300;         // inventory cap
    static constexpr float    RESCAN_S    = 60.f;
    static constexpr float    REREQUEST_S = 30.f;  // retry window for lost requests

    std::vector<FileInfo> scanFolder();
    // Lazily populate inventory_ from disk; guarantees haveFile() is accurate
    // even when a wire message arrives before the first tick() of a session.
    void ensureScanned();
    void broadcastList(const std::vector<FileInfo>& inv, uint8_t target = 0xFF);
    bool haveFile(const std::string& name) const;
    static bool sanitizedName(const std::string& name);

    net::NetThread* net_ = nullptr;
    std::string     dir_;            // <xp>/Output/FMS plans/
    std::vector<FileInfo> inventory_;
    // name → session time of the last request; re-requested after REREQUEST_S
    // so a plan is not lost forever when its only holder disconnects mid-send.
    std::unordered_map<std::string, float> requested_;
    // name → {size, mtime, hash}; survives reset() so periodic rescans do not
    // re-read unchanged files on the flight-loop thread.
    std::unordered_map<std::string, HashCacheEntry> hashCache_;
    bool  announced_   = false;
    bool  scanned_     = false;
    float rescanTimer_ = 0.f;
    float time_        = 0.f;  // seconds since session start
};

}
