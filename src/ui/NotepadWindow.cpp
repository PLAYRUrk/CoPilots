#include "NotepadWindow.h"
#include "Theme.h"
#include "../net/Protocol.h"

#include <imgui.h>
#include <XPLM/XPLMDisplay.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

namespace cp {
namespace ui {

// ── Init / shutdown ──────────────────────────────────────────────────────────

bool NotepadWindow::init()
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    // Place centre-ish on screen, 800×600 initial size.
    int cx = (scrL + scrR) / 2;
    int cy = (scrB + scrT) / 2;
    int l = cx - 400, r = cx + 400;
    int t = cy + 300, b = cy - 300;
    return xpwInit(l, t, r, b);
}

// ── Helpers ──────────────────────────────────────────────────────────────────

cp::notepad::NpId NotepadWindow::mintId()
{
    uint8_t myId = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
    return cp::notepad::makeNpId(myId, npCounter_++);
}

// ── Network send helpers ──────────────────────────────────────────────────────

void NotepadWindow::netTabShare(const cp::notepad::Tab& tab)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_TAB_SHARE)
                 .u32(tab.id)
                 .str(tab.name)
                 .build();
    sendFn(std::move(frame));
}

void NotepadWindow::netSheetNew(cp::notepad::NpId tabId,
                                const cp::notepad::Sheet& sheet)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_SHEET_NEW)
                 .u32(tabId)
                 .u32(sheet.id)
                 .f32(sheet.w)
                 .f32(sheet.h)
                 .build();
    sendFn(std::move(frame));
}

void NotepadWindow::netStrokeAdd(cp::notepad::NpId tabId,
                                 cp::notepad::NpId sheetId,
                                 const cp::notepad::Stroke& stroke)
{
    if (!sendFn || stroke.pts.empty()) return;
    auto b = cp::proto::MsgBuilder(cp::proto::MsgType::NP_STROKE_ADD)
             .u32(tabId)
             .u32(sheetId)
             .u32(stroke.id)
             .u8(static_cast<uint8_t>(stroke.tool))
             .u32(stroke.colorRGBA)
             .f32(stroke.thickness)
             .u16(static_cast<uint16_t>(stroke.pts.size()));
    for (const auto& p : stroke.pts)
        b.f32(p.x).f32(p.y);
    sendFn(b.build());
}

void NotepadWindow::netSheetDel(cp::notepad::NpId tabId,
                                cp::notepad::NpId sheetId)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_SHEET_DEL)
                 .u32(tabId)
                 .u32(sheetId)
                 .build();
    sendFn(std::move(frame));
}

void NotepadWindow::netSheetParam(cp::notepad::NpId tabId,
                                  const cp::notepad::Sheet& sheet)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_SHEET_PARAM)
                 .u32(tabId)
                 .u32(sheet.id)
                 .f32(sheet.w)
                 .f32(sheet.h)
                 .build();
    sendFn(std::move(frame));
}

// ── Inbound event handlers (called from flight loop, sim thread) ─────────────

void NotepadWindow::onTabShare(cp::notepad::NpId tabId, const std::string& name)
{
    notepad_.ensureSharedTab(tabId, name);
}

void NotepadWindow::onSheetNew(cp::notepad::NpId tabId,
                               cp::notepad::NpId sheetId,
                               float w, float h)
{
    cp::notepad::Tab* tab = notepad_.findTab(tabId);
    if (!tab) return;
    if (tab->findSheet(sheetId)) return; // already exists (echo from relay)
    cp::notepad::Sheet s;
    s.id = sheetId; s.w = w; s.h = h;
    tab->sheets.push_back(std::move(s));
    if (tab->activeSheet == cp::notepad::INVALID_NPID)
        tab->activeSheet = sheetId;
}

void NotepadWindow::onStrokeAdd(cp::notepad::NpId tabId,
                                cp::notepad::NpId sheetId,
                                const cp::notepad::Stroke& stroke)
{
    cp::notepad::Sheet* sheet = notepad_.findSheet(tabId, sheetId);
    if (!sheet) return;
    sheet->applyStroke(stroke); // idempotent via strokeIds set
}

void NotepadWindow::onSheetDel(cp::notepad::NpId tabId,
                               cp::notepad::NpId sheetId)
{
    cp::notepad::Tab* tab = notepad_.findTab(tabId);
    if (!tab) return;
    tab->sheets.erase(
        std::remove_if(tab->sheets.begin(), tab->sheets.end(),
                       [sheetId](const cp::notepad::Sheet& s){ return s.id == sheetId; }),
        tab->sheets.end());
    if (tab->activeSheet == sheetId)
        tab->activeSheet = tab->sheets.empty()
                         ? cp::notepad::INVALID_NPID
                         : tab->sheets.front().id;
}

void NotepadWindow::onSheetParam(cp::notepad::NpId tabId,
                                 cp::notepad::NpId sheetId,
                                 float w, float h)
{
    cp::notepad::Sheet* sheet = notepad_.findSheet(tabId, sheetId);
    if (!sheet) return;
    sheet->w = w; sheet->h = h;
}

void NotepadWindow::onTabDel(cp::notepad::NpId tabId)
{
    if (drawingTab_ == tabId) {
        drawing_      = false;
        drawingTab_   = cp::notepad::INVALID_NPID;
        drawingSheet_ = cp::notepad::INVALID_NPID;
        scratchStroke_ = {};
        erasedThisStroke_.clear();
    }
    notepad_.tabs.erase(
        std::remove_if(notepad_.tabs.begin(), notepad_.tabs.end(),
                       [tabId](const cp::notepad::Tab& t){ return t.id == tabId; }),
        notepad_.tabs.end());
}

void NotepadWindow::netTabDel(cp::notepad::NpId tabId)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_TAB_DEL)
                 .u32(tabId)
                 .build();
    sendFn(std::move(frame));
}

void NotepadWindow::netStrokeDel(cp::notepad::NpId tabId,
                                  cp::notepad::NpId sheetId,
                                  cp::notepad::NpId strokeId)
{
    if (!sendFn) return;
    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::NP_STROKE_DEL)
                 .u32(tabId)
                 .u32(sheetId)
                 .u32(strokeId)
                 .build();
    sendFn(std::move(frame));
}

void NotepadWindow::onStrokeDel(cp::notepad::NpId tabId,
                                 cp::notepad::NpId sheetId,
                                 cp::notepad::NpId strokeId)
{
    cp::notepad::Sheet* sheet = notepad_.findSheet(tabId, sheetId);
    if (sheet) sheet->removeStroke(strokeId);
}

void NotepadWindow::onSnapSheet(cp::notepad::NpId tabId,
                                const std::string& tabName,
                                cp::notepad::NpId sheetId,
                                float w, float h,
                                bool isFirstChunk,
                                const std::vector<cp::notepad::Stroke>& strokes)
{
    cp::notepad::Tab* tab = notepad_.ensureSharedTab(tabId, tabName);
    cp::notepad::Sheet* sheet = tab->findSheet(sheetId);
    if (!sheet || isFirstChunk) {
        // Create or reset the sheet
        if (!sheet) {
            tab->sheets.push_back({sheetId, w, h, {}, {}});
            sheet = &tab->sheets.back();
            if (tab->activeSheet == cp::notepad::INVALID_NPID)
                tab->activeSheet = sheetId;
        } else {
            sheet->strokes.clear();
            sheet->strokeIds.clear();
            sheet->w = w; sheet->h = h;
        }
    }
    for (const auto& s : strokes)
        sheet->applyStroke(s);
}

void NotepadWindow::resetShared()
{
    // Remove all shared tabs; keep private tabs.
    notepad_.tabs.erase(
        std::remove_if(notepad_.tabs.begin(), notepad_.tabs.end(),
                       [](const cp::notepad::Tab& t){ return t.shared; }),
        notepad_.tabs.end());
    drawing_ = false;
    scratchStroke_ = {};
    erasedThisStroke_.clear();
    drawingTab_   = cp::notepad::INVALID_NPID;
    drawingSheet_ = cp::notepad::INVALID_NPID;
}

// ── Canvas rendering ──────────────────────────────────────────────────────────

bool NotepadWindow::renderCanvas(cp::notepad::Tab& tab, cp::notepad::Sheet& sheet)
{
    using namespace cp::notepad;

    bool isOwner = sess_ && (npOwner(sheet.id) == static_cast<uint8_t>(sess_->myId()));
    bool shared  = tab.shared;

    // ── Sheet controls (owner only or disabled) ─────────────────────────────
    if (!isOwner) ImGui::BeginDisabled();

    float newW = sheet.w, newH = sheet.h;
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::DragFloat("W##sw", &newW, 10.f, 200.f, 8000.f, "%.0f")) {
        sheet.w = (newW < 200.f) ? 200.f : newW;
        if (shared) netSheetParam(tab.id, sheet);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.f);
    if (ImGui::DragFloat("H##sh", &newH, 10.f, 200.f, 6000.f, "%.0f")) {
        sheet.h = (newH < 200.f) ? 200.f : newH;
        if (shared) netSheetParam(tab.id, sheet);
    }
    ImGui::SameLine();

    // Delete sheet button
    bool deleteRequested = false;
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.10f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.05f, 0.05f, 1.f));
    if (ImGui::SmallButton("Del Sheet"))
        deleteRequested = true;
    ImGui::PopStyleColor(3);

    if (!isOwner) ImGui::EndDisabled();

    if (deleteRequested) {
        if (shared) netSheetDel(tab.id, sheet.id);
        // For private sheets: signal the caller to delete and stop rendering this sheet.
        // For shared sheets: the host relay echo will trigger onSheetDel idempotently.
        if (!shared) return true;
        // shared: fall through and continue rendering until echo arrives
    }

    ImGui::Separator();

    // ── Tool toolbar ────────────────────────────────────────────────────────
    auto toolButton = [&](const char* label, Tool tool) {
        bool active = (currentTool_ == tool);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccentR, kAccentG, kAccentB, 1.f));
        }
        if (ImGui::SmallButton(label)) currentTool_ = tool;
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    toolButton("Pen",    Tool::Pen);
    toolButton("Eraser", Tool::Eraser);
    toolButton("Line",   Tool::Line);
    toolButton("Rect",   Tool::Rect);
    toolButton("Ellipse",Tool::Ellipse);

    ImGui::SetNextItemWidth(100.f);
    ImGui::SliderFloat("##thick", &currentThickness_, 1.f, 20.f, "%.0f px");
    ImGui::SameLine();

    // Author colour swatch
    uint8_t myPid = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
    uint32_t col32 = colorForParticipant(myPid);
    ImVec4 colVec(
        ((col32 >> 0)  & 0xFF) / 255.f,
        ((col32 >> 8)  & 0xFF) / 255.f,
        ((col32 >> 16) & 0xFF) / 255.f,
        1.f);
    ImGui::ColorButton("##mycolor", colVec,
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                       ImVec2(16, 16));
    ImGui::SameLine();
    ImGui::TextDisabled("Your colour");

    ImGui::Separator();

    // ── Scrollable canvas ───────────────────────────────────────────────────
    ImVec2 canvasSize(sheet.w, sheet.h);
    ImGui::BeginChild("##canvas_scroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Clip to canvas rect
    dl->PushClipRect(origin, ImVec2(origin.x + sheet.w, origin.y + sheet.h), true);

    // Draw background
    dl->AddRectFilled(origin,
                      ImVec2(origin.x + sheet.w, origin.y + sheet.h),
                      0xFF1A1C1Fu); // near-black background

    // Render all committed strokes
    for (const auto& stroke : sheet.strokes) {
        if (stroke.pts.empty()) continue;
        uint32_t c = stroke.colorRGBA;
        // ImGui expects ABGR internally but AddPolyline takes ImU32 via IM_COL32
        // colorRGBA is stored as 0xRRGGBBAA; convert to ImGui ABGR:
        ImU32 imCol = IM_COL32(
            (c >> 0)  & 0xFF,  // R
            (c >> 8)  & 0xFF,  // G
            (c >> 16) & 0xFF,  // B
            (c >> 24) & 0xFF   // A
        );

        switch (stroke.tool) {
        case Tool::Pen:
        case Tool::Eraser: {
            std::vector<ImVec2> verts;
            verts.reserve(stroke.pts.size());
            for (const auto& p : stroke.pts)
                verts.push_back(ImVec2(origin.x + p.x, origin.y + p.y));
            if (verts.size() >= 2)
                dl->AddPolyline(verts.data(), static_cast<int>(verts.size()),
                                imCol, 0, stroke.thickness);
            else if (verts.size() == 1)
                dl->AddCircleFilled(verts[0], stroke.thickness * 0.5f, imCol);
            break;
        }
        case Tool::Line:
            if (stroke.pts.size() >= 2)
                dl->AddLine(ImVec2(origin.x + stroke.pts[0].x, origin.y + stroke.pts[0].y),
                            ImVec2(origin.x + stroke.pts[1].x, origin.y + stroke.pts[1].y),
                            imCol, stroke.thickness);
            break;
        case Tool::Rect:
            if (stroke.pts.size() >= 2)
                dl->AddRect(ImVec2(origin.x + stroke.pts[0].x, origin.y + stroke.pts[0].y),
                            ImVec2(origin.x + stroke.pts[1].x, origin.y + stroke.pts[1].y),
                            imCol, 0.f, 0, stroke.thickness);
            break;
        case Tool::Ellipse:
            if (stroke.pts.size() >= 2) {
                float cx = origin.x + (stroke.pts[0].x + stroke.pts[1].x) * 0.5f;
                float cy = origin.y + (stroke.pts[0].y + stroke.pts[1].y) * 0.5f;
                float rx = fabsf(stroke.pts[1].x - stroke.pts[0].x) * 0.5f;
                float ry = fabsf(stroke.pts[1].y - stroke.pts[0].y) * 0.5f;
                dl->AddEllipse(ImVec2(cx, cy), rx, ry, imCol, 0.f, 48, stroke.thickness);
            }
            break;
        }
    }

    // Draw in-progress scratch stroke (local preview)
    if (drawing_ && drawingTab_ == tab.id && drawingSheet_ == sheet.id
        && !scratchStroke_.pts.empty())
    {
        uint8_t myId2 = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
        uint32_t sc = colorForParticipant(myId2);
        ImU32 imSc = IM_COL32((sc>>0)&0xFF, (sc>>8)&0xFF, (sc>>16)&0xFF, (sc>>24)&0xFF);

        switch (scratchStroke_.tool) {
        case Tool::Pen:
        case Tool::Eraser: {
            std::vector<ImVec2> verts;
            for (const auto& p : scratchStroke_.pts)
                verts.push_back(ImVec2(origin.x + p.x, origin.y + p.y));
            if (verts.size() >= 2)
                dl->AddPolyline(verts.data(), static_cast<int>(verts.size()),
                                imSc, 0, scratchStroke_.thickness);
            break;
        }
        case Tool::Line:
            if (scratchStroke_.pts.size() >= 2)
                dl->AddLine(ImVec2(origin.x + scratchStroke_.pts[0].x,
                                   origin.y + scratchStroke_.pts[0].y),
                            ImVec2(origin.x + scratchStroke_.pts[1].x,
                                   origin.y + scratchStroke_.pts[1].y),
                            imSc, scratchStroke_.thickness);
            break;
        case Tool::Rect:
            if (scratchStroke_.pts.size() >= 2)
                dl->AddRect(ImVec2(origin.x + scratchStroke_.pts[0].x,
                                   origin.y + scratchStroke_.pts[0].y),
                            ImVec2(origin.x + scratchStroke_.pts[1].x,
                                   origin.y + scratchStroke_.pts[1].y),
                            imSc, 0.f, 0, scratchStroke_.thickness);
            break;
        case Tool::Ellipse:
            if (scratchStroke_.pts.size() >= 2) {
                float cx = origin.x + (scratchStroke_.pts[0].x + scratchStroke_.pts[1].x)*0.5f;
                float cy = origin.y + (scratchStroke_.pts[0].y + scratchStroke_.pts[1].y)*0.5f;
                float rx = fabsf(scratchStroke_.pts[1].x - scratchStroke_.pts[0].x)*0.5f;
                float ry = fabsf(scratchStroke_.pts[1].y - scratchStroke_.pts[0].y)*0.5f;
                dl->AddEllipse(ImVec2(cx, cy), rx, ry, imSc, 0.f, 48, scratchStroke_.thickness);
            }
            break;
        }
    }

    dl->PopClipRect();

    // Invisible button over the canvas captures mouse input
    ImGui::InvisibleButton("##canvas", canvasSize);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();

    // Safety reset: if the mouse was released while this canvas wasn't rendered
    // (e.g. the user switched sheets mid-gesture), cancel the in-flight gesture.
    // Guard against IsMouseReleased so we don't stomp the commit block below on the
    // normal release frame (IsMouseDown is already false on that same frame).
    if (drawing_ && drawingTab_ == tab.id && !ImGui::IsMouseDown(0) && !ImGui::IsMouseReleased(0)) {
        drawing_ = false;
        scratchStroke_ = {};
        erasedThisStroke_.clear();
    }

    if (hovered || active) {
        ImVec2 mp = ImGui::GetMousePos();
        float lx = mp.x - origin.x;
        float ly = mp.y - origin.y;

        // Clamp to canvas bounds
        float cx = lx < 0.f ? 0.f : (lx > sheet.w ? sheet.w : lx);
        float cy = ly < 0.f ? 0.f : (ly > sheet.h ? sheet.h : ly);

        if (ImGui::IsMouseClicked(0) && hovered) {
            // Start a new drawing/erase gesture
            drawing_ = true;
            drawingTab_   = tab.id;
            drawingSheet_ = sheet.id;
            scratchStroke_ = {};

            if (currentTool_ == Tool::Eraser) {
                erasedThisStroke_.clear();
            } else {
                scratchStroke_.tool      = currentTool_;
                scratchStroke_.thickness = currentThickness_;

                cp::notepad::Point p0{cx, cy};
                scratchStroke_.pts.push_back(p0);

                if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect
                 || currentTool_ == Tool::Ellipse)
                    scratchStroke_.pts.push_back(p0); // placeholder p1
            }
        }

        if (drawing_ && active && drawingTab_ == tab.id && drawingSheet_ == sheet.id) {
            if (currentTool_ == Tool::Eraser) {
                // Smart eraser: hit-test every stroke against the current probe position
                // and delete those that are touched.  Collect IDs first to avoid iterator
                // invalidation while calling removeStroke().
                float eRadius = currentThickness_ > 4.f ? currentThickness_ : 4.f;
                cp::notepad::Point probe{lx, ly};  // use raw (unclipped) for hit-testing
                std::vector<cp::notepad::NpId> toErase;
                for (const auto& s : sheet.strokes) {
                    if (!erasedThisStroke_.count(s.id)
                        && cp::notepad::strokeHit(s, probe, eRadius))
                        toErase.push_back(s.id);
                }
                for (auto eid : toErase) {
                    erasedThisStroke_.insert(eid);
                    sheet.removeStroke(eid);
                    if (shared) netStrokeDel(tab.id, sheet.id, eid);
                }
            } else if (currentTool_ == Tool::Pen) {
                // Accumulate polyline with distance filter (≥ 1.5 px) to reduce point spam
                if (scratchStroke_.pts.size() < cp::notepad::Stroke::MAX_POINTS
                    && !scratchStroke_.pts.empty())
                {
                    const auto& last = scratchStroke_.pts.back();
                    float ddx = cx - last.x, ddy = cy - last.y;
                    if (ddx*ddx + ddy*ddy >= 1.5f * 1.5f)
                        scratchStroke_.pts.push_back({cx, cy});
                }
            } else {
                // Shape (Line/Rect/Ellipse): update the second anchor point in-place
                if (scratchStroke_.pts.size() >= 2)
                    scratchStroke_.pts[1] = {cx, cy};
            }
        }

        if (drawing_ && ImGui::IsMouseReleased(0) && drawingTab_ == tab.id) {
            drawing_ = false;
            if (currentTool_ == Tool::Eraser) {
                erasedThisStroke_.clear();
            } else if (!scratchStroke_.pts.empty()) {
                // Finalise and commit the stroke
                uint8_t myId3 = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
                scratchStroke_.id        = mintId();
                scratchStroke_.colorRGBA = colorForParticipant(myId3);
                sheet.applyStroke(scratchStroke_);
                if (shared)
                    netStrokeAdd(tab.id, sheet.id, scratchStroke_);
            }
            scratchStroke_ = {};
        }

        // Eraser cursor preview: draw a circle at the mouse position when eraser is active
        if (currentTool_ == Tool::Eraser) {
            float eRadius = currentThickness_ > 4.f ? currentThickness_ : 4.f;
            ImGui::GetForegroundDrawList()->AddCircle(
                mp, eRadius, IM_COL32(200, 200, 200, 160), 24, 1.5f);
        }
    } else if (drawing_ && drawingTab_ == tab.id && drawingSheet_ == sheet.id) {
        // Mouse left the button area while a gesture was in progress
        if (ImGui::IsMouseReleased(0)) {
            drawing_ = false;
            scratchStroke_ = {};
            erasedThisStroke_.clear();
        }
    }

    ImGui::EndChild();
    return false;
}

// ── Main render ──────────────────────────────────────────────────────────────

void NotepadWindow::renderContent()
{
    using namespace cp::notepad;

    // Lazy-init target size from XPLM box (first draw only).
    if (targetW_ < 1.f) {
        targetW_ = (float)xpwWidth();
        targetH_ = (float)xpwHeight();
    }

    // Force ImGui window to exactly our tracked size so it always matches the XPLM box.
    // NoResize keeps ImGui's built-in resize grip hidden (we have our own handle below).
    ImGui::SetNextWindowSize(ImVec2(targetW_, targetH_), ImGuiCond_Always);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("Notepad", nullptr, wf)) { xpwEndWindow(); return; }

    // ── Custom resize grip (bottom-right corner) ─────────────────────────────
    {
        const float gripSize = 20.f;
        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImVec2 gripMin = ImVec2(winPos.x + winSize.x - gripSize, winPos.y + winSize.y - gripSize);
        ImVec2 gripMax = ImVec2(winPos.x + winSize.x,            winPos.y + winSize.y);

        ImVec2 mouse = ImGui::GetMousePos();
        bool inGrip = !resizing_ &&
                      (mouse.x >= gripMin.x && mouse.x <= gripMax.x &&
                       mouse.y >= gripMin.y && mouse.y <= gripMax.y);

        if (inGrip || resizing_)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);

        if (inGrip && ImGui::IsMouseClicked(0)) {
            resizing_          = true;
            resizeStartMouseX_ = mouse.x;
            resizeStartMouseY_ = mouse.y;
            resizeStartW_      = winSize.x;
            resizeStartH_      = winSize.y;
        }
        if (resizing_) {
            if (ImGui::IsMouseDown(0)) {
                float newW = resizeStartW_ + (mouse.x - resizeStartMouseX_);
                float newH = resizeStartH_ + (mouse.y - resizeStartMouseY_);
                newW = (newW < 420.f) ? 420.f : newW;
                newH = (newH < 300.f) ? 300.f : newH;
                targetW_ = newW;
                targetH_ = newH;
                xpwSetGeometry(xpwLeft(), xpwTop(),
                               xpwLeft() + (int)newW,
                               xpwTop()  - (int)newH);
            } else {
                resizing_ = false;
            }
        }

        // Draw grip lines over everything else (foreground list)
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImU32 gripCol = (inGrip || resizing_)
                      ? IM_COL32(140, 180, 255, 220)
                      : IM_COL32(100, 130, 180, 140);
        for (int i = 1; i <= 3; ++i) {
            float off = gripSize * 0.28f * (float)i;
            fg->AddLine(ImVec2(gripMax.x - off, gripMax.y),
                        ImVec2(gripMax.x,        gripMax.y - off),
                        gripCol, 1.5f);
        }
    }

    uint8_t myPid = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;

    // ── Tab bar ──────────────────────────────────────────────────────────────
    if (ImGui::BeginTabBar("##np_tabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {

        // Button to create a new private tab
        if (ImGui::TabItemButton("+##newtab", ImGuiTabItemFlags_Trailing)) {
            Tab t;
            t.id     = mintId();
            t.name   = std::string("Tab ") + std::to_string(notepad_.tabs.size() + 1);
            t.shared = false;
            notepad_.tabs.push_back(std::move(t));
        }

        NpId toDeleteTab = INVALID_NPID;

        for (auto& tab : notepad_.tabs) {
            char tabLabel[80];
            snprintf(tabLabel, sizeof(tabLabel), "%s%s##%u",
                     tab.name.c_str(),
                     tab.shared ? " [S]" : "",
                     tab.id);

            if (ImGui::BeginTabItem(tabLabel)) {
                bool isTabOwner = (npOwner(tab.id) == myPid);

                // ── Tab header ───────────────────────────────────────────────
                // Share button (tab owner only; once shared cannot unshare)
                if (!tab.shared) {
                    if (!isTabOwner) ImGui::BeginDisabled();
                    if (ImGui::SmallButton("Share")) {
                        tab.shared = true;
                        netTabShare(tab);
                        for (const auto& sheet : tab.sheets) {
                            netSheetNew(tab.id, sheet);
                            for (const auto& stroke : sheet.strokes)
                                netStrokeAdd(tab.id, sheet.id, stroke);
                        }
                    }
                    if (!isTabOwner) ImGui::EndDisabled();
                    ImGui::SameLine();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.3f, 1.f));
                    ImGui::TextUnformatted("[Shared]");
                    ImGui::PopStyleColor();
                    ImGui::SameLine();
                }

                // Rename tab (owner only).
                // Reload the buffer whenever the active tab changes so we don't show the
                // previous tab's name in the field.
                if (isTabOwner) {
                    if (renameTabId_ != tab.id) {
                        strncpy(tabNameBuf_, tab.name.c_str(), sizeof(tabNameBuf_) - 1);
                        tabNameBuf_[sizeof(tabNameBuf_) - 1] = '\0';
                        renameTabId_ = tab.id;
                    }
                    ImGui::SetNextItemWidth(110.f);
                    if (ImGui::InputText("##tabname", tabNameBuf_, sizeof(tabNameBuf_),
                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                        tab.name = tabNameBuf_;
                        // Keep renameTabId_ == tab.id; buffer already matches.
                    }
                    ImGui::SameLine();
                }

                // Add sheet button
                if (ImGui::SmallButton("+Sheet")) {
                    Sheet s;
                    s.id = mintId();
                    s.w = 1200.f; s.h = 800.f;
                    NpId sid = s.id;
                    tab.sheets.push_back(std::move(s));
                    tab.activeSheet = sid;
                    if (tab.shared) netSheetNew(tab.id, *tab.findSheet(sid));
                }
                ImGui::SameLine();

                // Delete tab button (owner only)
                if (!isTabOwner) ImGui::BeginDisabled();
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.10f, 0.10f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.15f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.05f, 0.05f, 1.f));
                if (ImGui::SmallButton("Del Tab"))
                    toDeleteTab = tab.id;
                ImGui::PopStyleColor(3);
                if (!isTabOwner) ImGui::EndDisabled();

                ImGui::Separator();

                // ── Sheet tab bar ────────────────────────────────────────────
                if (tab.sheets.empty()) {
                    ImGui::TextDisabled("No sheets. Click +Sheet to create one.");
                } else {
                    if (ImGui::BeginTabBar("##np_sheets")) {
                        NpId toDeleteSheet = INVALID_NPID;
                        int sheetIdx = 0;
                        for (auto& sheet : tab.sheets) {
                            ++sheetIdx;
                            char sheetLabel[48];
                            snprintf(sheetLabel, sizeof(sheetLabel),
                                     "Sheet %d##%u", sheetIdx, sheet.id);

                            if (ImGui::BeginTabItem(sheetLabel)) {
                                tab.activeSheet = sheet.id;
                                if (renderCanvas(tab, sheet))
                                    toDeleteSheet = sheet.id;
                                ImGui::EndTabItem();
                            }
                        }
                        ImGui::EndTabBar();
                        if (toDeleteSheet != INVALID_NPID)
                            onSheetDel(tab.id, toDeleteSheet);
                    }
                }

                ImGui::EndTabItem();
            }
        }

        ImGui::EndTabBar();

        // Process tab deletion after the tab bar loop to avoid iterator invalidation.
        if (toDeleteTab != INVALID_NPID) {
            // Find the tab to know if it's shared before erasing.
            Tab* dying = notepad_.findTab(toDeleteTab);
            if (dying && dying->shared) netTabDel(toDeleteTab);
            onTabDel(toDeleteTab);
        }
    }

    xpwEndWindow();
}

} // namespace ui
} // namespace cp
