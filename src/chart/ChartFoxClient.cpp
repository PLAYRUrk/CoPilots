#include "ChartFoxClient.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>

using json = nlohmann::json;

namespace cp {
namespace chart {

static const char* API_BASE = "https://api.chartfox.org/v2";

// Display order of ChartType enum values (Ground first — most used in-sim).
static const struct { int type; const char* fallbackKey; } kTypeOrder[] = {
    {3, "Ground"}, {6, "Approach"}, {4, "SID"}, {5, "STAR"}, {7, "Transition"},
    {1, "General"}, {2, "Textual"}, {99, "Briefing"}, {0, "Unknown"},
};

std::vector<std::string> ChartFoxClient::authHeaders() const
{
    std::vector<std::string> h;
    h.push_back("Authorization: Bearer " + token_);
    h.push_back("Accept: application/json");
    return h;
}

std::string ChartFoxClient::httpErrorToText(int status, const std::string& err,
                                            const std::string& body)
{
    if (status == 401) return "ChartFox: invalid or expired token (401)";
    if (status == 403) {
        // 403 from the API means a missing scope; 403 from a file host (after
        // the redirect to its CDN) means the URL itself was refused.
        if (body.find("<Error>") != std::string::npos
            || body.find("AccessDenied") != std::string::npos)
            return "ChartFox: the chart host refused the download (403)";
        return "ChartFox: token lacks the required scope (403)";
    }
    if (status == 404) return "ChartFox: not found (404) — check the ICAO";
    if (status == 422) {
        // Validation error shape: {"message": "...", "errors": {...}}
        try {
            json j = json::parse(body);
            return "ChartFox: " + j.value("message", std::string("validation failed"));
        } catch (...) {}
        return "ChartFox: validation failed (422)";
    }
    if (status == 429) return "ChartFox: rate limited (429) — try again shortly";
    return "ChartFox: " + err;
}

net::AsyncHttp::Handle ChartFoxClient::beginListCharts(const std::string& icao)
{
    return http_->get(std::string(API_BASE) + "/airports/" + icao + "/charts/grouped",
                      authHeaders());
}

static ChartInfo parseChartInfo(const json& c)
{
    ChartInfo ci;
    ci.id         = c.value("id", "");
    ci.name       = c.value("name", "");
    if (c.contains("code") && c["code"].is_string()) ci.code = c["code"];
    ci.type       = c.value("type", 0);
    ci.typeKey    = c.value("type_key", "");
    ci.hasGeorefs = c.contains("has_georeferences")
                 && c["has_georeferences"].is_boolean()
                 && c["has_georeferences"].get<bool>();
    if (c.contains("meta") && c["meta"].is_array()) {
        for (const auto& m : c["meta"]) {
            // ChartMetaType 0 = Runways
            if (m.value("type", -1) == 0 && m.contains("value") && m["value"].is_array()) {
                std::string rw;
                for (const auto& v : m["value"]) {
                    if (!v.is_string()) continue;
                    if (!rw.empty()) rw += " ";
                    rw += v.get<std::string>();
                }
                ci.runways = rw;
            }
        }
    }
    return ci;
}

bool ChartFoxClient::pollListCharts(net::AsyncHttp::Handle h, ChartListResult& out)
{
    net::HttpResult res;
    if (!http_->poll(h, res)) return false;

    out = {};
    out.status = res.status;
    if (!res.ok) {
        out.error = httpErrorToText(res.status, res.error, res.body);
        return true;
    }
    try {
        json j = json::parse(res.body);
        const json& data = j.at("data");
        // data: {"<typeEnum>": [charts]} — emit groups in kTypeOrder, then any
        // unknown keys the API may add later.
        std::vector<std::string> seen;
        for (const auto& t : kTypeOrder) {
            std::string key = std::to_string(t.type);
            if (!data.contains(key) || !data[key].is_array() || data[key].empty())
                continue;
            seen.push_back(key);
            std::vector<ChartInfo> charts;
            for (const auto& c : data[key]) charts.push_back(parseChartInfo(c));
            std::string label = charts.front().typeKey.empty()
                              ? t.fallbackKey : charts.front().typeKey;
            out.groups.emplace_back(label, std::move(charts));
        }
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (std::find(seen.begin(), seen.end(), it.key()) != seen.end()) continue;
            if (!it.value().is_array() || it.value().empty()) continue;
            std::vector<ChartInfo> charts;
            for (const auto& c : it.value()) charts.push_back(parseChartInfo(c));
            out.groups.emplace_back("Type " + it.key(), std::move(charts));
        }
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = std::string("ChartFox: bad chart list — ") + e.what();
    }
    return true;
}

net::AsyncHttp::Handle ChartFoxClient::beginGetChart(const std::string& chartId)
{
    return http_->get(std::string(API_BASE) + "/charts/" + chartId, authHeaders());
}

bool ChartFoxClient::pollGetChart(net::AsyncHttp::Handle h, ChartDetailResult& out)
{
    net::HttpResult res;
    if (!http_->poll(h, res)) return false;

    out = {};
    out.status = res.status;
    if (!res.ok) {
        out.error = httpErrorToText(res.status, res.error, res.body);
        return true;
    }
    try {
        json j = json::parse(res.body);
        ChartDetail d;
        d.info = parseChartInfo(j);

        // Best displayable file: prefer first-party hosted files[] (0=PDF,
        // 1=IMG — either renders; PDF preferred for multi-page + vectors),
        // fall back to the top-level hosted url.
        std::string pdfUrl, imgUrl;
        if (j.contains("files") && j["files"].is_array()) {
            for (const auto& f : j["files"]) {
                int t = f.value("type", -1);
                std::string u = f.value("url", "");
                if (u.empty()) continue;
                if (t == 0 && pdfUrl.empty()) pdfUrl = u;
                if (t == 1 && imgUrl.empty()) imgUrl = u;
            }
        }
        d.fileUrl = !pdfUrl.empty() ? pdfUrl
                  : !imgUrl.empty() ? imgUrl
                  : j.value("url", "");

        // Charts ChartFox does not host itself only carry the supplier's URL
        // (charts:view_source_url).  PDF/IMG sources render like any other
        // file; HTML ones can only be opened in a browser.
        if (j.contains("source_url") && j["source_url"].is_string())
            d.sourceUrl = j["source_url"];
        if (j.contains("source_url_type") && j["source_url_type"].is_number_integer())
            d.sourceUrlType = j["source_url_type"];
        d.viewUrl = j.value("view_url", "");
        if (d.fileUrl.empty() && !d.sourceUrl.empty()
            && (d.sourceUrlType == 0 || d.sourceUrlType == 1))
            d.fileUrl = d.sourceUrl;

        d.requiresPreauth = j.contains("requires_preauth")
                         && j["requires_preauth"].is_boolean()
                         && j["requires_preauth"].get<bool>();

        if (j.contains("source") && j["source"].is_object()) {
            const json& s = j["source"];
            d.copyrightShort = s.value("copyright_statement_short", "");
            d.sourceName     = s.value("display_name", "");
        }

        if (j.contains("georefs") && j["georefs"].is_array()) {
            for (const auto& g : j["georefs"]) {
                ChartGeoref gr;
                gr.tx              = g.value("tx", 0.0);
                gr.ty              = g.value("ty", 0.0);
                gr.k               = g.value("k", 1.0);
                gr.transformAngle  = g.value("transform_angle", 0.0);
                gr.page            = g.value("page", 1);
                gr.pdfPageRotation = g.value("pdf_page_rotation", 0);
                d.georefs.push_back(gr);
            }
        }
        out.detail = std::move(d);
        out.ok = true;
    } catch (const std::exception& e) {
        out.error = std::string("ChartFox: bad chart detail — ") + e.what();
    }
    return true;
}

std::string ChartFoxClient::cachePathFor(const std::string& chartId) const
{
    // UUIDs are filesystem-safe; extension neutral (format sniffed from bytes).
    return cacheDir_ + "/" + chartId + ".chart";
}

void ChartFoxClient::dropCachedFile(const std::string& chartId) const
{
    std::error_code ec;
    std::filesystem::remove(cachePathFor(chartId), ec);
}

net::AsyncHttp::Handle ChartFoxClient::beginDownloadFile(const ChartDetail& detail,
                                                         bool* outCached, bool withAuth)
{
    if (outCached) *outCached = false;
    std::string path = cachePathFor(detail.info.id);
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) {
        if (outCached) *outCached = true;
        return net::AsyncHttp::INVALID;
    }
    // Hosted chart files also require the bearer token, but must not ask for
    // JSON: strict suppliers answer 406 to an Accept the file cannot satisfy.
    std::vector<std::string> h;
    if (withAuth) h.push_back("Authorization: Bearer " + token_);
    return http_->getToFile(detail.fileUrl, h, path);
}

bool ChartFoxClient::pollDownloadFile(net::AsyncHttp::Handle h, DownloadResult& out)
{
    net::HttpResult res;
    if (!http_->poll(h, res)) return false;
    out = {};
    out.ok = res.ok;
    out.status = res.status;
    out.filePath = res.filePath;
    if (!res.ok)
        out.error = httpErrorToText(res.status, res.error, res.body);
    return true;
}

}
}
