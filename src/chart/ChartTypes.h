#pragma once
#include "../ui/Notepad.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// ChartFox charts shared data model.
// No ImGui or XPLM dependency — plugin.cpp holds the host authoritative copy.
//
// Reuses the notepad primitives (NpId minting, Stroke, strokeHit) so chart
// annotations share the exact wire layout and eraser geometry of the notepad.
// Stroke coordinates are CANONICAL CHART PIXELS: the pixel grid of the chart
// page rendered at the deterministic canonical scale (see ChartRenderer) —
// identical on every client, independent of local zoom/pan.
// ---------------------------------------------------------------------------

namespace cp {
namespace chart {

using cp::notepad::NpId;
using cp::notepad::Stroke;
using cp::notepad::INVALID_NPID;

// One chart in the airport list (from /v2/airports/{icao}/charts/grouped).
struct ChartInfo {
    std::string id;        // ChartFox UUID
    std::string name;
    std::string code;      // short identifier, may be empty
    int         type = 0;  // ChartType enum (3=Ground, 4=SID, 5=STAR, 6=Approach, ...)
    std::string typeKey;
    bool        hasGeorefs = false;
    std::string runways;   // meta Runways joined, e.g. "26L 08R" (may be empty)
};

// One georeference of a chart page (from /v2/charts/{id} "georefs").
struct ChartGeoref {
    double tx = 0, ty = 0;        // translation
    double k  = 1;                // scale
    double transformAngle = 0;    // rotation, degrees
    int    page = 1;              // 1-based PDF page
    int    pdfPageRotation = 0;   // degrees
};

// Chart detail (from /v2/charts/{id}) — everything the viewer needs.
struct ChartDetail {
    ChartInfo   info;
    std::string fileUrl;          // best displayable file (files[] or url)
    std::string copyrightShort;   // source.copyright_statement_short (attribution)
    std::string sourceName;       // source.display_name
    std::string sourceUrl;        // source_url — the supplier's own copy
    int         sourceUrlType = -1;   // 0 = PDF, 1 = IMG, 2 = HTML
    std::string viewUrl;          // view_url — the chart on chartfox.org
    bool        requiresPreauth = false;
    std::vector<ChartGeoref> georefs;

    const ChartGeoref* georefForPage(int page1) const
    {
        for (const auto& g : georefs)
            if (g.page == page1) return &g;
        return nullptr;
    }
};

// Annotations on one (chartId, page) — strokes in canonical chart pixels.
struct ChartAnnots {
    std::vector<Stroke>      strokes;
    std::unordered_set<NpId> strokeIds;

    bool applyStroke(Stroke s)
    {
        if (strokeIds.count(s.id)) return false;
        strokeIds.insert(s.id);
        strokes.push_back(std::move(s));
        return true;
    }
    bool removeStroke(NpId id)
    {
        auto it = std::find_if(strokes.begin(), strokes.end(),
                               [id](const Stroke& s){ return s.id == id; });
        if (it == strokes.end()) return false;
        strokes.erase(it);
        strokeIds.erase(id);
        return true;
    }
};

// Key: (ChartFox chart UUID, 0-based page index).
using AnnotKey = std::pair<std::string, uint8_t>;

// One tab = one airport.  When shared, icao / activeChartId / annots are
// synced; zoom, pan and the current page stay local to each client.
struct ChartTab {
    NpId        id = INVALID_NPID;   // owner = notepad::npOwner(id)
    std::string icao;
    std::string name;                // tab label (defaults to the ICAO)
    bool        shared = false;
    std::string activeChartId;       // "" = none selected
    std::map<AnnotKey, ChartAnnots> annots;

    ChartAnnots& annotsFor(const std::string& chartId, uint8_t page)
    { return annots[{chartId, page}]; }
    ChartAnnots* findAnnots(const std::string& chartId, uint8_t page)
    {
        auto it = annots.find({chartId, page});
        return (it != annots.end()) ? &it->second : nullptr;
    }
};

// Root container (one per plugin instance; host holds an authoritative copy).
struct SharedCharts {
    std::vector<ChartTab> tabs;

    ChartTab* findTab(NpId tabId)
    {
        auto it = std::find_if(tabs.begin(), tabs.end(),
                               [tabId](const ChartTab& t){ return t.id == tabId; });
        return (it != tabs.end()) ? &*it : nullptr;
    }

    ChartTab* ensureSharedTab(NpId tabId, const std::string& icao, const std::string& name)
    {
        ChartTab* t = findTab(tabId);
        if (!t) {
            ChartTab nt;
            nt.id = tabId; nt.icao = icao; nt.name = name;
            tabs.push_back(std::move(nt));
            t = &tabs.back();
        }
        t->shared = true;
        return t;
    }

    void clear() { tabs.clear(); }
};

}
}
