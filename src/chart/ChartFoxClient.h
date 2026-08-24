#pragma once
#include "ChartTypes.h"
#include "../net/HttpFetch.h"

#include <map>
#include <string>
#include <vector>

namespace cp {
namespace chart {

// Result of a chart-list fetch: charts grouped by type, in display order.
struct ChartListResult {
    bool        ok = false;
    int         status = 0;                  // HTTP status (401 -> token refresh)
    std::string error;                       // user-readable when !ok
    // (typeKey, charts) groups in the order they should be displayed.
    std::vector<std::pair<std::string, std::vector<ChartInfo>>> groups;
};

struct ChartDetailResult {
    bool        ok = false;
    int         status = 0;
    std::string error;
    ChartDetail detail;
};

struct DownloadResult {
    bool        ok = false;
    int         status = 0;
    std::string error;
    std::string filePath;
};

// ChartFox API v2 client on top of AsyncHttp.  All begin*() calls return an
// AsyncHttp handle; the matching poll*() parses the JSON on the main thread
// once the transfer finished (grouped lists are ≤ a few hundred KiB — a
// one-shot parse per user action, not per frame).
//
// Auth: OAuth access token (ChartFoxAuth), or a PAT as a fallback, sent as
// "Authorization: Bearer <token>".
// Chart files are cached on disk under cacheDir and never re-downloaded.
class ChartFoxClient {
public:
    void init(net::AsyncHttp* http, const std::string& cacheDir)
    { http_ = http; cacheDir_ = cacheDir; }

    void setToken(const std::string& t) { token_ = t; }
    bool hasToken() const { return !token_.empty(); }

    net::AsyncHttp::Handle beginListCharts(const std::string& icao);
    bool pollListCharts(net::AsyncHttp::Handle h, ChartListResult& out);

    net::AsyncHttp::Handle beginGetChart(const std::string& chartId);
    bool pollGetChart(net::AsyncHttp::Handle h, ChartDetailResult& out);

    // Downloads detail.fileUrl into the cache (skipped when already cached —
    // the handle is INVALID and *outCached is set).  File format is sniffed
    // from magic bytes by ChartRenderer, so the extension is neutral.
    std::string cachePathFor(const std::string& chartId) const;
    // withAuth=false repeats a download that the file host rejected: WinINet
    // follows redirects transparently, so the bearer meant for the API can end
    // up on a pre-signed CDN URL that refuses signed requests.
    net::AsyncHttp::Handle beginDownloadFile(const ChartDetail& detail, bool* outCached,
                                             bool withAuth = true);
    bool pollDownloadFile(net::AsyncHttp::Handle h, DownloadResult& out);
    // Drops the cached file so the next beginDownloadFile fetches it again
    // (a truncated or HTML error body cached once would otherwise stick).
    void dropCachedFile(const std::string& chartId) const;

private:
    std::vector<std::string> authHeaders() const;
    static std::string httpErrorToText(int status, const std::string& err,
                                       const std::string& body);

    net::AsyncHttp* http_ = nullptr;
    std::string     cacheDir_;
    std::string     token_;
};

}
}
