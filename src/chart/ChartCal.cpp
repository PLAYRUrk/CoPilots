#include "ChartCal.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace cp {
namespace chart {

namespace {

int64_t nowUnix()
{
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// Airport codes reach the filesystem and the repository as file names, so keep
// them to what an ICAO/IATA code can actually be.
bool sanitizeIcao(const std::string& in, std::string& out)
{
    out.clear();
    for (char c : in) {
        if (!std::isalnum(static_cast<unsigned char>(c))) return false;
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out.size() >= 3 && out.size() <= 4;
}

}   // namespace

std::string ChartCalStore::keyFor(const std::string& chartId, int page1)
{
    return chartId + ":" + std::to_string(page1);
}

bool ChartCalStore::splitKey(const std::string& key, std::string& chartId, int& page1)
{
    const size_t colon = key.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= key.size()) return false;
    chartId = key.substr(0, colon);
    page1   = std::atoi(key.c_str() + colon + 1);
    return page1 > 0;
}

bool ChartCalStore::parseEntries(const std::string& text,
                                 std::map<std::string, CalEntry>& out, std::string& err)
{
    out.clear();
    try {
        json j = json::parse(text);
        if (!j.is_object()) { err = "not a JSON object"; return false; }
        for (auto it = j.begin(); it != j.end(); ++it) {
            const json& e = it.value();
            if (!e.is_object()) continue;
            std::string chartId;
            int page1 = 0;
            if (!splitKey(it.key(), chartId, page1)) continue;

            CalEntry c;
            c.g.tx             = e.value("tx", 0.0);
            c.g.ty             = e.value("ty", 0.0);
            c.g.k              = e.value("k", 0.0);
            c.g.transformAngle = e.value("angle", 0.0);
            c.g.page           = e.value("page", page1);
            c.icao             = e.value("icao", std::string());
            c.isOverride       = e.value("override", false);
            c.mtime            = e.value("mtime", static_cast<int64_t>(0));
            c.shared           = e.value("shared", false);
            if (c.g.k == 0.0) continue;          // unusable, drop it
            out[it.key()] = c;
        }
        return true;
    } catch (const std::exception& ex) {
        err = ex.what();
        return false;
    }
}

std::string ChartCalStore::dumpEntries(const std::map<std::string, CalEntry>& entries)
{
    json j = json::object();
    for (const auto& [key, c] : entries) {
        json e = { {"tx", c.g.tx}, {"ty", c.g.ty}, {"k", c.g.k},
                   {"angle", c.g.transformAngle}, {"page", c.g.page},
                   {"override", c.isOverride}, {"mtime", c.mtime} };
        if (!c.icao.empty()) e["icao"] = c.icao;
        if (c.shared)        e["shared"] = true;
        j[key] = std::move(e);
    }
    return j.dump(2);
}

void ChartCalStore::init(const std::string& ownFile, const std::string& cacheDir)
{
    path_     = ownFile;
    cacheDir_ = cacheDir;
    own_.clear();
    community_.clear();

    std::ifstream f(path_);
    if (!f.is_open()) return;          // nothing calibrated yet — not an error
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!parseEntries(ss.str(), own_, err))
        Log("ChartCalStore: cannot parse %s (%s)", path_.c_str(), err.c_str());
    else
        Log("ChartCalStore: loaded %zu calibration(s)", own_.size());
}

void ChartCalStore::save() const
{
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream f(path_, std::ios::trunc);
    if (!f.is_open()) {
        Log("ChartCalStore: cannot write %s", path_.c_str());
        return;
    }
    f << dumpEntries(own_);
}

std::string ChartCalStore::cachePathFor(const std::string& icao) const
{
    std::string clean;
    if (cacheDir_.empty() || !sanitizeIcao(icao, clean)) return {};
    return cacheDir_ + "/" + clean + ".json";
}

const CalEntry* ChartCalStore::findOwn(const std::string& chartId, int page1) const
{
    auto it = own_.find(keyFor(chartId, page1));
    return it == own_.end() ? nullptr : &it->second;
}

const CalEntry* ChartCalStore::findOverriding(const std::string& chartId, int page1) const
{
    const std::string key = keyFor(chartId, page1);
    auto it = own_.find(key);
    if (it != own_.end()) return &it->second;
    // Someone bothered to calibrate a chart that already had a georeference —
    // that is a correction, and it outranks the georeference it corrects.
    auto ct = community_.find(key);
    if (ct != community_.end() && ct->second.isOverride) return &ct->second;
    return nullptr;
}

const CalEntry* ChartCalStore::findFallback(const std::string& chartId, int page1) const
{
    auto ct = community_.find(keyFor(chartId, page1));
    if (ct != community_.end() && !ct->second.isOverride) return &ct->second;
    return nullptr;
}

void ChartCalStore::set(const std::string& chartId, int page1, const ChartGeoref& g,
                        const std::string& icao, bool isOverride, int64_t mtime)
{
    CalEntry c;
    c.g            = g;
    c.g.page       = page1;
    c.icao         = icao;
    c.isOverride   = isOverride;
    c.mtime        = mtime > 0 ? mtime : nowUnix();
    c.shared       = false;
    own_[keyFor(chartId, page1)] = std::move(c);
    save();
}

void ChartCalStore::erase(const std::string& chartId, int page1)
{
    if (own_.erase(keyFor(chartId, page1))) save();
}

bool ChartCalStore::backfillIcao(const std::string& chartId, const std::string& icao)
{
    std::string clean;
    if (!sanitizeIcao(icao, clean)) return false;
    bool changed = false;
    for (auto& [key, c] : own_) {
        if (!c.icao.empty()) continue;
        std::string id;
        int page1 = 0;
        if (splitKey(key, id, page1) && id == chartId) {
            c.icao  = clean;
            changed = true;
        }
    }
    if (changed) save();
    return changed;
}

void ChartCalStore::setCommunity(const std::string& icao,
                                 std::map<std::string, CalEntry> entries, bool writeCache)
{
    std::string clean;
    if (!sanitizeIcao(icao, clean)) return;

    // Drop what we held for this airport, then take the new set.  Entries are
    // stamped with the airport so a later fetch can find them again.
    for (auto it = community_.begin(); it != community_.end(); ) {
        if (it->second.icao == clean) it = community_.erase(it);
        else                          ++it;
    }
    for (auto& [key, c] : entries) {
        if (c.icao.empty()) c.icao = clean;
        community_[key] = c;
    }

    if (!writeCache) return;
    const std::string p = cachePathFor(clean);
    if (p.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(cacheDir_, ec);
    std::ofstream f(p, std::ios::trunc);
    if (f.is_open()) f << dumpEntries(entries);
}

bool ChartCalStore::loadCommunityCache(const std::string& icao)
{
    const std::string p = cachePathFor(icao);
    if (p.empty()) return false;
    std::ifstream f(p);
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::map<std::string, CalEntry> entries;
    std::string err;
    if (!parseEntries(ss.str(), entries, err)) {
        Log("ChartCalStore: cannot parse %s (%s)", p.c_str(), err.c_str());
        return false;
    }
    setCommunity(icao, std::move(entries), false);
    return true;
}

std::vector<std::pair<std::string, CalEntry>> ChartCalStore::unshared() const
{
    std::vector<std::pair<std::string, CalEntry>> out;
    for (const auto& [key, c] : own_)
        if (!c.shared && !c.icao.empty()) out.emplace_back(key, c);
    return out;
}

void ChartCalStore::markShared(const std::vector<std::string>& keys)
{
    bool changed = false;
    for (const auto& k : keys) {
        auto it = own_.find(k);
        if (it != own_.end() && !it->second.shared) {
            it->second.shared = true;
            changed = true;
        }
    }
    if (changed) save();
}

}
}
