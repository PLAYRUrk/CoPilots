#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Notepad / whiteboard shared data model.
// No ImGui or XPLM dependency — safe to include in plugin.cpp for the host
// authoritative copy without pulling in UI headers.
// ---------------------------------------------------------------------------

namespace cp {
namespace notepad {

// ---------------------------------------------------------------------------
// Stable network IDs: (creatorParticipantId << 24) | counter24
// Owner is *intrinsic* to the ID — no separate ownership table needed.
// ---------------------------------------------------------------------------
using NpId = uint32_t;
constexpr NpId INVALID_NPID = 0xFFFFFFFFu;

inline NpId makeNpId(uint8_t creator, uint32_t counter)
{
    return (static_cast<uint32_t>(creator) << 24) | (counter & 0x00FFFFFFu);
}
inline uint8_t npOwner(NpId id) { return static_cast<uint8_t>(id >> 24); }

// ---------------------------------------------------------------------------
// Per-participant palette (16 distinct colours, indexed by pid & 0x0F).
// Colour is baked into Stroke::colorRGBA at authoring time so late joiners
// see correct colours even after the author has left.
// Format: 0xRRGGBBAA (ImGui uses RGBA in AddPolyline etc.)
// ---------------------------------------------------------------------------
inline uint32_t colorForParticipant(uint8_t pid)
{
    static const uint32_t kPalette[16] = {
        0xFF4477FFu,  // blue
        0xFF44FF77u,  // green
        0xFFFF4444u,  // red
        0xFFFFDD22u,  // yellow
        0xFFFF88AAu,  // pink
        0xFF22DDFFu,  // cyan
        0xFFFF8822u,  // orange
        0xFF8844FFu,  // purple
        0xFF44FFDDu,  // teal
        0xFFFF44DDu,  // magenta
        0xFF88FF44u,  // lime
        0xFF4488FFu,  // sky
        0xFFFFAA44u,  // amber
        0xFFAA44FFu,  // violet
        0xFF44FFAAu,  // mint
        0xFFFFFF44u,  // light yellow
    };
    return kPalette[pid & 0x0Fu];
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------
enum class Tool : uint8_t { Pen = 0, Eraser = 1, Line = 2, Rect = 3, Ellipse = 4 };

struct Point { float x = 0, y = 0; };

// One completed stroke (sent atomically on mouse-release).
struct Stroke {
    NpId     id        = INVALID_NPID;  // makeNpId(author, counter); author = npOwner(id)
    Tool     tool      = Tool::Pen;
    uint32_t colorRGBA = 0xFFFFFFFFu;  // baked author colour
    float    thickness = 2.f;
    // Pen/Eraser: polyline vertices (capped at MAX_POINTS).
    // Shapes (Line/Rect/Ellipse): exactly 2 points — p0 = anchor, p1 = opposite corner.
    std::vector<Point> pts;

    static constexpr uint16_t MAX_POINTS = 4096;
};

// A resizable canvas that accumulates strokes.
struct Sheet {
    NpId  id  = INVALID_NPID;          // owner = npOwner(id)
    float w   = 1200.f;
    float h   = 800.f;
    std::vector<Stroke> strokes;
    std::unordered_set<NpId> strokeIds; // fast de-dup (echo suppression)

    // Returns true if the stroke was new (not a duplicate).
    bool applyStroke(Stroke s)
    {
        if (strokeIds.count(s.id)) return false;
        strokeIds.insert(s.id);
        strokes.push_back(std::move(s));
        return true;
    }

    // Remove stroke by ID (smart eraser).  Returns true if found and removed.
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

// A named collection of sheets.  shared==false means private/local (never networked).
struct Tab {
    NpId        id           = INVALID_NPID; // owner = npOwner(id)
    std::string name;
    bool        shared       = false;
    std::vector<Sheet> sheets;
    NpId        activeSheet  = INVALID_NPID;

    Sheet* findSheet(NpId sheetId)
    {
        auto it = std::find_if(sheets.begin(), sheets.end(),
                               [sheetId](const Sheet& s){ return s.id == sheetId; });
        return (it != sheets.end()) ? &*it : nullptr;
    }
    const Sheet* findSheet(NpId sheetId) const
    {
        auto it = std::find_if(sheets.begin(), sheets.end(),
                               [sheetId](const Sheet& s){ return s.id == sheetId; });
        return (it != sheets.end()) ? &*it : nullptr;
    }
};

// Root container (one per plugin instance; host holds an extra authoritative copy).
struct Notepad {
    std::vector<Tab> tabs;

    Tab* findTab(NpId tabId)
    {
        auto it = std::find_if(tabs.begin(), tabs.end(),
                               [tabId](const Tab& t){ return t.id == tabId; });
        return (it != tabs.end()) ? &*it : nullptr;
    }
    Sheet* findSheet(NpId tabId, NpId sheetId)
    {
        Tab* t = findTab(tabId);
        return t ? t->findSheet(sheetId) : nullptr;
    }

    // Ensure a shared Tab exists (create if absent). Returns pointer (never null).
    Tab* ensureSharedTab(NpId tabId, const std::string& name)
    {
        Tab* t = findTab(tabId);
        if (!t) {
            tabs.push_back({tabId, name, true, {}, INVALID_NPID});
            t = &tabs.back();
        }
        t->shared = true;
        return t;
    }

    void clear() { tabs.clear(); }
};

// ---------------------------------------------------------------------------
// Smart-eraser helpers (no ImGui / XPLM dependency)
// ---------------------------------------------------------------------------

// Minimum distance from point P to the finite line segment AB (2-D).
inline float pointSegDist(Point p, Point a, Point b)
{
    float dx = b.x - a.x, dy = b.y - a.y;
    float lenSq = dx*dx + dy*dy;
    if (lenSq < 1e-6f) {
        float ex = p.x - a.x, ey = p.y - a.y;
        return std::sqrt(ex*ex + ey*ey);
    }
    float t = ((p.x - a.x)*dx + (p.y - a.y)*dy) / lenSq;
    if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
    float qx = a.x + t*dx - p.x, qy = a.y + t*dy - p.y;
    return std::sqrt(qx*qx + qy*qy);
}

// Returns true if probe is within (radius + stroke.thickness/2) of any part of stroke.
inline bool strokeHit(const Stroke& stroke, Point probe, float radius)
{
    if (stroke.pts.empty()) return false;
    float thresh = radius + stroke.thickness * 0.5f;

    switch (stroke.tool) {
    case Tool::Pen:
    case Tool::Eraser:
    case Tool::Line: {
        size_t end = (stroke.tool == Tool::Line) ? 2 : stroke.pts.size();
        if (end > stroke.pts.size()) end = stroke.pts.size();
        if (end == 1) {
            float dx = probe.x - stroke.pts[0].x, dy = probe.y - stroke.pts[0].y;
            return std::sqrt(dx*dx + dy*dy) <= thresh;
        }
        for (size_t i = 1; i < end; ++i)
            if (pointSegDist(probe, stroke.pts[i-1], stroke.pts[i]) <= thresh)
                return true;
        return false;
    }
    case Tool::Rect:
        if (stroke.pts.size() < 2) return false;
        {
            Point tl = stroke.pts[0], br = stroke.pts[1];
            Point tr{br.x, tl.y}, bl{tl.x, br.y};
            return pointSegDist(probe, tl, tr) <= thresh
                || pointSegDist(probe, tr, br) <= thresh
                || pointSegDist(probe, br, bl) <= thresh
                || pointSegDist(probe, bl, tl) <= thresh;
        }
    case Tool::Ellipse:
        if (stroke.pts.size() < 2) return false;
        {
            float cx = (stroke.pts[0].x + stroke.pts[1].x) * 0.5f;
            float cy = (stroke.pts[0].y + stroke.pts[1].y) * 0.5f;
            float rx = std::fabs(stroke.pts[1].x - stroke.pts[0].x) * 0.5f;
            float ry = std::fabs(stroke.pts[1].y - stroke.pts[0].y) * 0.5f;
            constexpr int SEGS = 48;
            constexpr float PI2 = 6.28318530718f;
            Point prev{cx + rx, cy};
            for (int i = 1; i <= SEGS; ++i) {
                float ang = PI2 * static_cast<float>(i) / SEGS;
                Point cur{cx + rx * std::cos(ang), cy + ry * std::sin(ang)};
                if (pointSegDist(probe, prev, cur) <= thresh) return true;
                prev = cur;
            }
            return false;
        }
    default:
        return false;
    }
}

} // namespace notepad
} // namespace cp
