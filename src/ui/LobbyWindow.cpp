#include "LobbyWindow.h"
#include "Theme.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <XPLM/XPLMDisplay.h>

namespace cp {
namespace ui {

bool LobbyWindow::init()
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrR; (void)scrB;
    int l = scrL + 450;
    int t = scrT - 40;
    int r = l + 540;
    int b = t - 460;
    return xpwInit(l, t, r, b);
}

void LobbyWindow::renderContent()
{
    if (!sess_ || !cfg_ || !sess_->isHost()) {
        ImGui::Begin("CoPilots -- Lobby (Admin)", nullptr,
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::TextDisabled("Not hosting.");
        ImGui::End();
        return;
    }

    ImGui::Begin("CoPilots -- Lobby (Admin)", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f,0.10f,0.10f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f,0.20f,0.20f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f,0.05f,0.05f,1.f));
    if (ImGui::Button("Stop Hosting", ImVec2(-1.f, 30.f)))
        if (onStopHosting) onStopHosting();
    ImGui::PopStyleColor(3);
    ImGui::Spacing();

    int count = static_cast<int>(sess_->participants().size());
    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f),
                       "Connected Pilots: %d", count);
    ImGui::Separator();

    if (cfg_->fromSmartCopilot)
        ImGui::TextColored(ImVec4(1.f,0.7f,0.1f,1.f),
                           "Loaded from smartcopilot.cfg -- zone/role features unavailable.");

    ImGui::Spacing();

    if (ImGui::BeginTable("##pilots", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(0, 220.f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID",      ImGuiTableColumnFlags_WidthFixed,   30.f);
        ImGui::TableSetupColumn("Nick",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Zones",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,  120.f);
        ImGui::TableHeadersRow();

        for (const auto& p : sess_->participants()) {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(p.id));

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", p.id);

            ImGui::TableSetColumnIndex(1);
            if (p.isPhysicsMaster)
                ImGui::TextColored(ImVec4(kAccentR,kAccentG,kAccentB,1.f),
                                   "[M] %s", p.nick.c_str());
            else
                ImGui::Text("%s", p.nick.c_str());

            ImGui::TableSetColumnIndex(2);
            std::string roleLabel = p.roleId.empty() ? "-- none --" : p.roleId;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##role", roleLabel.c_str())) {
                if (ImGui::Selectable("-- none --", p.roleId.empty()))
                    if (onRoleAssign) onRoleAssign(p.id, "");
                for (const auto& r : cfg_->roles) {
                    bool sel = (p.roleId == r.id);
                    if (ImGui::Selectable(r.name.c_str(), sel))
                        if (onRoleAssign) onRoleAssign(p.id, r.id);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            if (cfg_->fromSmartCopilot) {
                ImGui::TextDisabled("n/a");
            } else {
                if (p.zoneIds.empty()) {
                    ImGui::TextDisabled("none");
                } else {
                    bool first = true;
                    for (const auto& z : p.zoneIds) {
                        if (!first) ImGui::SameLine();
                        ImGui::TextColored(ImVec4(0.5f,0.8f,0.5f,1.f), "[%s]", z.c_str());
                        first = false;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Edit##z")) ImGui::OpenPopup("EditZones");
                if (ImGui::BeginPopup("EditZones")) {
                    ImGui::Text("Zones for %s:", p.nick.c_str());
                    ImGui::Separator();
                    std::vector<std::string> newZones = p.zoneIds;
                    bool changed = false;
                    for (const auto& z : cfg_->zones) {
                        bool owns = std::find(newZones.begin(), newZones.end(), z.id)
                                    != newZones.end();
                        bool prev = owns;
                        ImGui::Checkbox(z.name.c_str(), &owns);
                        if (owns != prev) {
                            changed = true;
                            if (owns) newZones.push_back(z.id);
                            else newZones.erase(
                                std::remove(newZones.begin(), newZones.end(), z.id),
                                newZones.end());
                        }
                    }
                    if (changed && onZoneAssign) onZoneAssign(p.id, newZones);
                    ImGui::EndPopup();
                }
            }

            ImGui::TableSetColumnIndex(4);
            if (!p.isPhysicsMaster) {
                if (ImGui::SmallButton("Physics##pm"))
                    if (onPhysicsMasterSet) onPhysicsMasterSet(p.id);
                ImGui::SameLine();
            }
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f,0.1f,0.1f,0.8f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f,0.2f,0.2f,1.0f));
            if (ImGui::SmallButton("Kick"))
                if (onKick) onKick(p.id);
            ImGui::PopStyleColor(2);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("[M] = Physics Master  |  Click Edit to customise zones");

    ImGui::End();
}

}
}
