#include "StatusHud.h"
#include "Theme.h"
#include <imgui.h>
#include <cstdio>
#include <XPLM/XPLMDisplay.h>

namespace cp {
namespace ui {

bool StatusHud::init()
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrL; (void)scrB;
    int r = scrR - 10;
    int t = scrT - 10;
    int l = r - 220;
    int b = t - 200;
    return xpwInit(l, t, r, b, xplm_WindowDecorationNone);
}

void StatusHud::renderContent()
{
    ImGuiWindowFlags flags =
          ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoInputs
        | ImGuiWindowFlags_NoNav
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoBringToFrontOnFocus
        | ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::Begin("##hud", nullptr, flags);

    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "CoPilots");
    ImGui::Separator();

    if (!sess_) { ImGui::TextDisabled("Not connected"); ImGui::End(); return; }

    const Participant* me = sess_->find(sess_->myId());
    if (me) {
        ImGui::Text("Nick:  %s", me->nick.c_str());
        const char* role = me->roleId.empty() ? "-- none --" : me->roleId.c_str();
        ImGui::Text("Role:  %s", role);

        if (!me->zoneIds.empty()) {
            ImGui::Text("Zones:");
            for (const auto& z : me->zoneIds) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f,0.9f,0.4f,1.f), "[%s]", z.c_str());
            }
        } else {
            ImGui::TextDisabled("Zones: none");
        }

        if (sess_->isPhysicsMaster())
            ImGui::TextColored(ImVec4(kAccentR,kAccentG,kAccentB,1.f), "* Physics Master");
    } else {
        ImGui::TextDisabled("Not connected");
    }

    ImGui::Separator();

    if (ping_ms_ == 0) {
        ImGui::TextDisabled("Ping: --");
    } else {
        ImVec4 col = (ping_ms_ < 50)  ? ImVec4(0.3f,0.9f,0.3f,1.f)
                   : (ping_ms_ < 150) ? ImVec4(1.f, 0.8f,0.1f,1.f)
                                      : ImVec4(0.9f,0.2f,0.2f,1.f);
        ImGui::TextColored(col, "Ping: %u ms", ping_ms_);
    }
    if (packetLoss_ > 0.01f)
        ImGui::TextColored(ImVec4(0.9f,0.4f,0.1f,1.f), "Loss: %.1f%%", packetLoss_*100.f);

    ImGui::TextDisabled("Crew: %d",
        static_cast<int>(sess_->participants().size()));

    ImGui::End();
}

}
}
