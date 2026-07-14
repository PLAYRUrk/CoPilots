#pragma once

#include "../net/NetThread.h"
#include "../net/Protocol.h"
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace cp {

// Shares the flight plans in <X-Plane>/Output/FMS plans/ between all crew
// members, so everyone can load the same route regardless of whose PC it was
// created on.
//
// Protocol (TCP, host relays broadcasts like any other shared message):
//   FMS_LIST    — inventory broadcast: {name, size, hash} per file.  Sent when
//                 the session starts and whenever a rescan finds new local files.
//   FMS_REQUEST — names a participant is missing after comparing an inventory.
//   FMS_FILE    — one file (name + raw bytes), broadcast so every participant
//                 missing it can save it in one pass.
//
// Safety rules:
//   - only *.fms / *.flp files, max FILE_LIMIT bytes each;
//   - file names are sanitised (no path separators, no leading dot);
//   - an existing local file is NEVER overwritten — first content wins,
//     locally created plans always take priority on their own machine.
class FmsPlanSync {
public:
    void init(net::NetThread* net, const std::string& xpSystemPath);

    // Call every flight loop while connected; announces the inventory on the
    // first tick of a session and rescans the folder every RESCAN_S seconds.
    void tick(float dt, bool connected);

    // Wire handlers (called from the plugin's TCP dispatch).
    void onList(proto::MsgReader& r);
    void onRequest(proto::MsgReader& r);
    void onFile(proto::MsgReader& r);

    void reset();

private:
    struct FileInfo {
        std::string name;
        uint32_t    size = 0;
        uint32_t    hash = 0;
    };

    static constexpr uint32_t FILE_LIMIT = 256 * 1024;  // per-file size cap
    static constexpr int      MAX_FILES  = 300;         // inventory cap
    static constexpr float    RESCAN_S   = 60.f;

    std::vector<FileInfo> scanFolder() const;
    void broadcastList(const std::vector<FileInfo>& inv);
    bool haveFile(const std::string& name) const;
    static bool sanitizedName(const std::string& name);

    net::NetThread* net_ = nullptr;
    std::string     dir_;            // <xp>/Output/FMS plans/
    std::vector<FileInfo> inventory_;
    // Names already requested from peers this session (avoid duplicate requests).
    std::unordered_set<std::string> requested_;
    bool  announced_  = false;
    float rescanTimer_ = 0.f;
};

}
