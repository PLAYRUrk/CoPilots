#pragma once
#include "ChartCal.h"
#include "../net/HttpFetch.h"

#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace cp {
namespace chart {

// Where the shared calibration database lives.  Same repository and the same
// raw.githubusercontent.com route the aircraft config library already uses
// (see net/ConfigDownloader.cpp), one file per airport:
//   charts/cal/<ICAO>.json   — the calibrations
//   charts/cal/config.json   — {"submit_url": "..."} for the upload endpoint
constexpr const char* kCalRawBase =
    "https://raw.githubusercontent.com/PLAYRUrk/CoPilots/main/charts/cal/";
constexpr const char* kCalIssueUrl =
    "https://github.com/PLAYRUrk/CoPilots/issues/new";

// Built-in upload endpoint.  Empty by default: the address of the submission
// worker is published in charts/cal/config.json so it can change without a
// rebuild, and until one exists the Share button falls back to a prefilled
// GitHub issue.
constexpr const char* kCalSubmitUrlDefault = "";

// ---------------------------------------------------------------------------
// ChartCalCloud — the shared calibration database.
//
// Download is lazy and frugal: opening a tab on EGLL reads the local copy of
// EGLL.json and stops there.  GitHub is only asked when there is nothing local
// for that airport, or when the user presses Refresh — chart calibrations
// barely change, and a fetch on every tab open would be traffic for nothing.
//
// Upload happens by itself: a calibration made here is sent as soon as an
// endpoint is known (Mode::Auto — quiet, no browser, retried later if it
// fails).  The Share button is the manual path (Mode::Manual): it reports what
// happened and falls back to a prefilled GitHub issue when there is no
// endpoint to post to.  The endpoint is a small serverless worker that holds
// the repository credential; the plugin ships no write token.
// ---------------------------------------------------------------------------
class ChartCalCloud {
public:
    void init(net::AsyncHttp* http, ChartCalStore* store);

    // Prefs override for the upload endpoint (debugging / a private mirror).
    void setSubmitUrlOverride(const std::string& url) { urlOverride_ = url; }

    // Make one airport's calibrations available.  Reads the local copy; only
    // goes to GitHub when there is none, or when force is set (Refresh).
    void requestAirport(const std::string& icao, bool force = false);
    // Allow a fresh fetch for every airport again.
    void refresh();

    enum class Mode {
        Auto,     // by itself: quiet, never opens a browser, retried later
        Manual    // the user asked: reports the outcome, issue fallback allowed
    };
    void submit(std::vector<std::pair<std::string, CalEntry>> entries,
                Mode mode = Mode::Manual);

    // True once we know there is nowhere to upload to — the automatic path
    // stops trying, the Share button falls back to the issue form.
    bool endpointMissing() const;

    // Poll in-flight requests; call once per frame.
    void tick();

    bool uploading() const { return submitHandle_ != net::AsyncHttp::INVALID; }
    const std::string& status() const { return status_; }
    bool statusIsError() const { return statusError_; }
    void setStatus(const std::string& text, bool isError)
    { status_ = text; statusError_ = isError; }

    // Set by the owner so the issue fallback can open a browser window.
    std::function<void(const std::string& url)> openBrowser;

private:
    struct Fetch {
        net::AsyncHttp::Handle handle = net::AsyncHttp::INVALID;
        std::string            icao;
    };

    void ensureConfig();
    void sendPending();
    bool configKnown() const { return cfgDone_; }
    void openIssue(const std::vector<std::pair<std::string, CalEntry>>& entries);
    std::string effectiveSubmitUrl() const;

    net::AsyncHttp* http_  = nullptr;
    ChartCalStore*  store_ = nullptr;

    std::vector<Fetch>    fetches_;
    std::set<std::string> requested_;

    net::AsyncHttp::Handle cfgHandle_ = net::AsyncHttp::INVALID;
    bool                   cfgDone_   = false;
    std::string            cfgSubmitUrl_;
    std::string            urlOverride_;

    net::AsyncHttp::Handle submitHandle_ = net::AsyncHttp::INVALID;
    Mode                   mode_         = Mode::Manual;
    // Kept until the answer arrives: marked shared when accepted, handed to the
    // issue fallback when the endpoint refuses.
    std::vector<std::pair<std::string, CalEntry>> inFlight_;
    std::vector<std::pair<std::string, CalEntry>> pending_;  // waiting for config
    Mode                                          pendingMode_ = Mode::Manual;

    std::string status_;
    bool        statusError_ = false;
};

}
}
