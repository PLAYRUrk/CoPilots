#include "StatusHud.h"
#include "Theme.h"
#include <imgui.h>
#include <cstdio>
#include <XPLM/XPLMDisplay.h>

namespace cp {
namespace ui {

bool StatusHud::init()
{
    // Initial placement: top-right corner; content will push the left/bottom edges.
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrL; (void)scrB;
    int r = scrR - 10;
    int t = scrT - 10;
    int l = r - 240;
    int b = t - 220;
    return xpwInit(l, t, r, b, xplm_WindowDecorationNone);
}

void StatusHud::renderContent()
{
    // NoInputs removed so small buttons respond to clicks.
    // NoDecoration keeps it frameless; NoMove / NoResize are enforced via XPLM re-anchor below.
    ImGuiWindowFlags flags =
          ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowBgAlpha(0.82f);
    ImGui::Begin("##hud", nullptr, flags);

    // ── Quick-open buttons ──────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(kAccentR*0.4f, kAccentG*0.3f, kAccentB*0.5f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(kAccentR,      kAccentG,      kAccentB,      1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(kAccentR*0.7f, kAccentG*0.7f, kAccentB*0.7f, 1.00f));

    if (ImGui::SmallButton(" C ")) { if (onToggleConn)    onToggleConn(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Connect / Host");
    ImGui::SameLine();
    if (ImGui::SmallButton(" N ")) { if (onToggleNotepad) onToggleNotepad(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Notepad");

    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "CoPilots");
    ImGui::Separator();

    if (!sess_) {
        ImGui::TextDisabled("Not connected");
        // Capture size and re-anchor before End()
        measuredSize_ = ImGui::GetWindowSize();
        ImGui::End();
        reanchorBottomRight();
        return;
    }

    const Participant* me = sess_->find(sess_->myId());
    if (me) {
        ImGui::Text("Nick:  %s", me->nick.c_str());
        const char* role = me->roleId.empty() ? "-- none --" : me->roleId.c_str();
        ImGui::Text("Role:  %s", role);

        if (!me->zoneIds.empty()) {
            ImGui::Text("Zones:");
            for (const auto& z : me->zoneIds) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "[%s]", z.c_str());
            }
        } else {
            ImGui::TextDisabled("Zones: none");
        }

        if (sess_->isPhysicsMaster())
            ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "* Physics Master");
    } else {
        ImGui::TextDisabled("Not connected");
    }

    ImGui::Separator();

    if (ping_ms_ == 0) {
        ImGui::TextDisabled("Ping: --");
    } else {
        ImVec4 col = (ping_ms_ < 50)  ? ImVec4(0.3f, 0.9f, 0.3f, 1.f)
                   : (ping_ms_ < 150) ? ImVec4(1.f,  0.8f, 0.1f, 1.f)
                                      : ImVec4(0.9f, 0.2f, 0.2f, 1.f);
        ImGui::TextColored(col, "Ping: %u ms", ping_ms_);
    }
    if (packetLoss_ > 0.01f)
        ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.1f, 1.f), "Loss: %.1f%%", packetLoss_ * 100.f);

    ImGui::TextDisabled("Crew: %d", static_cast<int>(sess_->participants().size()));

    measuredSize_ = ImGui::GetWindowSize();
    ImGui::End();

    reanchorBottomRight();
}

void StatusHud::reanchorBottomRight()
{
    if (measuredSize_.x <= 0.f || measuredSize_.y <= 0.f) return;

    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrL;

    // Pin the right and bottom edges; let left/top grow as content expands.
    int r = scrR - 10;
    int b = scrB + 10;
    int l = r - static_cast<int>(measuredSize_.x);
    int t = b + static_cast<int>(measuredSize_.y);

    // xpwSetGeometry calls clampToScreen internally.
    xpwSetGeometry(l, t, r, b);
}

}
}
