#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cp {
namespace net {

// Downloads aircraft configs from the GitHub library (configs/ folder of the
// project repository) and installs them into the current aircraft's folder.
//
// Flow (all on a worker thread, never blocking the sim):
//   1. GET configs/manifest.json from raw.githubusercontent.com
//   2. Match the aircraft folder / .acf name against the manifest 'match' keys
//   3. GET configs/<folder>/copilots.json, validate it parses as JSON
//   4. Back up any existing copilots.json to copilots.json.bak, write the new one
//
// The main thread polls state() each flight loop and shows message() in the UI.
// The downloaded config is picked up on the next Host/Join (config.load reads
// the aircraft folder) — an active session is never mutated mid-flight.
class ConfigDownloader {
public:
    enum class State { IDLE, RUNNING, SUCCESS, FAILED };

    struct Entry {
        std::string folder;    // configs/<folder>/ on GitHub
        std::string aircraft;  // display name
    };

    ~ConfigDownloader();

    // Kick off the async download for the given aircraft folder, auto-matching
    // the aircraft against the library manifest.
    // acfName is the aircraft's .acf file name (used for matching, may be empty).
    // Returns false if a download is already running.
    bool start(const std::string& aircraftDir, const std::string& acfName);

    // Download a specific library entry (user picked it manually — e.g. flying
    // Zibo with only a LevelUp config in the library).
    bool startFolder(const std::string& aircraftDir, const std::string& folder,
                     const std::string& aircraftName);

    State state() const { return state_.load(); }

    // Thread-safe status/result text for the UI.
    std::string message();

    // Library entries from the last fetched manifest (empty until a download
    // attempt has run).  Used by the UI for the manual picker.
    std::vector<Entry> entries();

    // Reset SUCCESS/FAILED back to IDLE after the UI consumed the result.
    void acknowledge();

private:
    // forcedFolder empty → auto-match against the manifest.
    bool launch(const std::string& aircraftDir, const std::string& acfName,
                const std::string& forcedFolder, const std::string& forcedName);
    void run(std::string aircraftDir, std::string acfName,
             std::string forcedFolder, std::string forcedName);
    void setMessage(const std::string& m);

    std::thread        thread_;
    std::atomic<State> state_{State::IDLE};
    std::mutex         mu_;
    std::string        msg_;
    std::vector<Entry> entries_;
};

}
}
