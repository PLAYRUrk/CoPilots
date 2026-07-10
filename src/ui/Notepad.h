#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>

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

} // namespace notepad
} // namespace cp
