#include "ChartCalCloud.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <cctype>
#include <cstdio>

using json = nlohmann::json;

namespace cp {
namespace chart {

namespace {

std::string urlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// The upload body, and the same text the issue fallback carries.
std::string bodyFor(const std::vector<std::pair<std::string, CalEntry>>& entries)
{
    json arr = json::array();
    for (const auto& [key, c] : entries) {
        std::string chartId;
        int page1 = 0;
        if (!ChartCalStore::splitKey(key, chartId, page1)) continue;
        arr.push_back({ {"icao", c.icao}, {"chartId", chartId}, {"page", page1},
                        {"k", c.g.k}, {"tx", c.g.tx}, {"ty", c.g.ty},
                        {"angle", c.g.transformAngle},
                        {"override", c.isOverride}, {"mtime", c.mtime} });
    }
    json j = { {"v", 1}, {"entries", std::move(arr)} };
    return j.dump();
}

}   // namespace

void ChartCalCloud::init(net::AsyncHttp* http, ChartCalStore* store)
{
    http_  = http;
    store_ = store;
}

std::string ChartCalCloud::effectiveSubmitUrl() const
{
    if (!urlOverride_.empty())  return urlOverride_;
    if (!cfgSubmitUrl_.empty()) return cfgSubmitUrl_;
    return kCalSubmitUrlDefault;
}

void ChartCalCloud::ensureConfig()
{
    if (!http_ || cfgDone_ || cfgHandle_ != net::AsyncHttp::INVALID) return;
    cfgHandle_ = http_->get(std::string(kCalRawBase) + "config.json", {});
}

void ChartCalCloud::requestAirport(const std::string& icao, bool force)
{
    if (!http_ || !store_ || icao.empty()) return;
    const bool first = requested_.insert(icao).second;
    if (!first && !force) return;

    // What is already on disk is enough to fly with, and it is all there is
    // with no network.  Having it means we do not go asking for it again.
    const bool haveLocal = store_->loadCommunityCache(icao);

    ensureConfig();
    if (haveLocal && !force) return;

    Fetch f;
    f.icao   = icao;
    f.handle = http_->get(std::string(kCalRawBase) + icao + ".json", {});
    fetches_.push_back(f);
}

void ChartCalCloud::refresh()
{
    requested_.clear();
    status_.clear();
    statusError_ = false;
}

bool ChartCalCloud::endpointMissing() const
{
    return cfgDone_ && effectiveSubmitUrl().empty();
}

void ChartCalCloud::submit(std::vector<std::pair<std::string, CalEntry>> entries,
                           Mode mode)
{
    const bool quiet = (mode == Mode::Auto);
    if (entries.empty()) {
        if (!quiet) { status_ = "Nothing new to share."; statusError_ = false; }
        return;
    }
    if (uploading() || !pending_.empty()) {
        if (!quiet) {
            status_      = "Still uploading the previous batch...";
            statusError_ = false;
        }
        return;
    }
    // The endpoint accepts a bounded batch; the rest goes on the next round.
    if (entries.size() > 20) entries.resize(20);

    pending_     = std::move(entries);
    pendingMode_ = mode;
    ensureConfig();
    if (!effectiveSubmitUrl().empty() || cfgDone_) sendPending();
    else if (!quiet) {
        status_      = "Looking up the submission endpoint...";
        statusError_ = false;
    }
}

void ChartCalCloud::sendPending()
{
    if (pending_.empty()) return;
    const std::string url = effectiveSubmitUrl();
    if (url.empty()) {
        // Nowhere to post.  Asked by hand, hand the crew the issue form so the
        // calibration still reaches the database, via a person; by itself, stay
        // silent - nothing is marked shared, so it is offered again later.
        if (pendingMode_ == Mode::Manual) openIssue(pending_);
        pending_.clear();
        return;
    }
    mode_         = pendingMode_;
    inFlight_     = std::move(pending_);
    pending_.clear();
    submitHandle_ = http_->postJson(url, {}, bodyFor(inFlight_));
    if (mode_ == Mode::Manual) {
        status_      = "Sharing " + std::to_string(inFlight_.size()) + " calibration(s)...";
        statusError_ = false;
    }
}

void ChartCalCloud::openIssue(const std::vector<std::pair<std::string, CalEntry>>& entries)
{
    std::string title = "Chart calibration";
    if (!entries.empty() && !entries.front().second.icao.empty())
        title += ": " + entries.front().second.icao;

    std::string body = "Calibration data from the CoPilots plugin.\n\n```json\n"
                     + bodyFor(entries) + "\n```\n";

    std::string url = std::string(kCalIssueUrl) + "?title=" + urlEncode(title)
                    + "&body=" + urlEncode(body);
    // Browsers and GitHub both give up on very long URLs; a batch that big is
    // better attached by hand than silently truncated.
    if (url.size() > 7000) {
        status_ = "Too many calibrations for the issue form — share them one "
                  "airport at a time, or attach Output/CoPilots/chart_calibrations.json "
                  "to an issue.";
        statusError_ = true;
        return;
    }
    if (openBrowser) openBrowser(url);
    status_      = "Opened a prefilled issue in your browser — press Submit there.";
    statusError_ = false;
}

void ChartCalCloud::tick()
{
    if (!http_ || !store_) return;

    if (cfgHandle_ != net::AsyncHttp::INVALID) {
        net::HttpResult res;
        if (http_->poll(cfgHandle_, res)) {
            cfgHandle_ = net::AsyncHttp::INVALID;
            cfgDone_   = true;
            if (res.ok) {
                try {
                    json j = json::parse(res.body);
                    cfgSubmitUrl_ = j.value("submit_url", std::string());
                } catch (const std::exception& e) {
                    Log("ChartCalCloud: bad config.json (%s)", e.what());
                }
            }
            sendPending();   // a Share pressed while this was in flight
        }
    }

    for (auto it = fetches_.begin(); it != fetches_.end(); ) {
        net::HttpResult res;
        if (!http_->poll(it->handle, res)) { ++it; continue; }

        if (res.ok) {
            std::map<std::string, CalEntry> entries;
            std::string err;
            if (ChartCalStore::parseEntries(res.body, entries, err)) {
                const size_t n = entries.size();
                store_->setCommunity(it->icao, std::move(entries), true);
                Log("ChartCalCloud: %s — %zu community calibration(s)",
                    it->icao.c_str(), n);
            } else {
                Log("ChartCalCloud: %s — bad calibration file (%s)",
                    it->icao.c_str(), err.c_str());
            }
        } else if (res.status == 404) {
            // No file for this airport yet.  Perfectly normal — and the cached
            // copy, if any, is stale, so drop it.
            store_->setCommunity(it->icao, {}, true);
        } else {
            // Offline or GitHub having a bad day: keep whatever the cache gave.
            Log("ChartCalCloud: %s — %s", it->icao.c_str(),
                res.error.empty() ? "fetch failed" : res.error.c_str());
        }
        it = fetches_.erase(it);
    }

    if (submitHandle_ != net::AsyncHttp::INVALID) {
        net::HttpResult res;
        if (http_->poll(submitHandle_, res)) {
            submitHandle_ = net::AsyncHttp::INVALID;
            if (res.ok) {
                std::vector<std::string> keys;
                for (const auto& [key, c] : inFlight_) keys.push_back(key);
                store_->markShared(keys);
                Log("ChartCalCloud: shared %zu calibration(s)", keys.size());
                if (mode_ == Mode::Manual) {
                    status_      = "Shared - thanks! Others will see it on their next fetch.";
                    statusError_ = false;
                }
            } else if (mode_ == Mode::Auto) {
                // Nothing was marked shared, so the next round tries again.
                Log("ChartCalCloud: automatic upload failed (HTTP %d) %s", res.status,
                    res.error.empty() ? "" : res.error.c_str());
            } else if (res.status == 429) {
                status_      = "Too many submissions in a row - try again in a moment.";
                statusError_ = true;
            } else {
                // The endpoint is down or refused us; the browser route still
                // works, so the calibration is not lost.
                char buf[160];
                snprintf(buf, sizeof(buf), "Upload failed (HTTP %d) - opening an issue instead.",
                         res.status);
                Log("ChartCalCloud: upload failed (HTTP %d) %s", res.status,
                    res.error.empty() ? "" : res.error.c_str());
                std::string msg = buf;
                openIssue(inFlight_);
                if (!statusError_) status_ = msg + " " + status_;
            }
            inFlight_.clear();
        }
    }
}

}
}
