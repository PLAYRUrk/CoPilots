#include "ConnectionWindow.h"
#include "Theme.h"
#include <imgui.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <XPLM/XPLMDisplay.h>

namespace cp {
namespace ui {

// ── Init ──────────────────────────────────────────────────────────────────────

bool ConnectionWindow::init()
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrR; (void)scrB;
    // Initial size — will auto-resize to content every frame
    return xpwInit(scrL + 40, scrT - 40, scrL + 440, scrT - 200);
}

// ── State ─────────────────────────────────────────────────────────────────────

void ConnectionWindow::setState(ConnState s, const std::string& msg)
{
    state_     = s;
    statusMsg_ = msg;
    if (s != ConnState::CONNECTED) {
        isHost_ = false; localPort_ = 0; localIp_.clear();
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static bool parseAddr(const char* buf, std::string& outHost, uint16_t& outPort)
{
    const char* colon = strrchr(buf, ':');
    if (!colon || colon == buf) return false;
    outHost = std::string(buf, colon);
    int p = std::atoi(colon + 1);
    if (p <= 0 || p > 65535) return false;
    outPort = static_cast<uint16_t>(p);
    return true;
}

// ── renderContent ─────────────────────────────────────────────────────────────

void ConnectionWindow::renderContent()
{
    xpwBeginWindow("CoPilots");

    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "CoPilots");
    ImGui::SameLine(); ImGui::TextDisabled("v0.1");
    ImGui::Separator();
    ImGui::Spacing();

    switch (state_) {
    case ConnState::CONNECTED:
        if (isHost_) renderHostedView();
        else         renderClientView();
        break;
    default:
        renderConnectForm();
        break;
    }

    xpwEndWindow();
}

void ConnectionWindow::renderConnectForm()
{
    if (ImGui::BeginTabBar("##mode")) {
        if (ImGui::BeginTabItem("Host")) {
            connCfg_.asHost = true;
            ImGui::Spacing();
            ImGui::Text("Nickname");
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::InputText("##nick", nickBuf_, sizeof(nickBuf_)))
                connCfg_.nick = nickBuf_;

            ImGui::Spacing();
            ImGui::Text("Port");
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::InputText("##port", portBuf_, sizeof(portBuf_),
                                 ImGuiInputTextFlags_CharsDecimal))
                connCfg_.port = static_cast<uint16_t>(std::atoi(portBuf_));

            ImGui::Spacing();
            if (state_ == ConnState::CONNECT_ERROR)
                ImGui::TextColored(ImVec4(0.95f,0.3f,0.3f,1.f), "Error: %s", statusMsg_.c_str());
            else if (state_ == ConnState::CONNECTING)
                ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "Connecting...");
            ImGui::Spacing();

            bool busy = (state_ == ConnState::CONNECTING);
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Start Hosting", ImVec2(-1.f, 30.f)))
                if (onHost) onHost(connCfg_);
            if (busy) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Join")) {
            connCfg_.asHost = false;
            ImGui::Spacing();
            ImGui::Text("Nickname");
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::InputText("##nick2", nickBuf_, sizeof(nickBuf_)))
                connCfg_.nick = nickBuf_;

            ImGui::Spacing();
            ImGui::Text("Host address  (IP:Port)");
            ImGui::SetNextItemWidth(240.f);
            if (ImGui::InputText("##addr", addrBuf_, sizeof(addrBuf_))) {
                std::string h; uint16_t p = 0;
                if (parseAddr(addrBuf_, h, p)) { connCfg_.host = h; connCfg_.port = p; }
            }

            ImGui::Spacing();
            if (state_ == ConnState::CONNECT_ERROR)
                ImGui::TextColored(ImVec4(0.95f,0.3f,0.3f,1.f), "Error: %s", statusMsg_.c_str());
            else if (state_ == ConnState::CONNECTING)
                ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "Connecting...");
            ImGui::Spacing();

            bool busy = (state_ == ConnState::CONNECTING);
            if (busy) ImGui::BeginDisabled();
            if (ImGui::Button("Join", ImVec2(-1.f, 30.f))) {
                std::string h; uint16_t p = 0;
                if (parseAddr(addrBuf_, h, p)) { connCfg_.host = h; connCfg_.port = p; }
                if (onJoin) onJoin(connCfg_);
            }
            if (busy) ImGui::EndDisabled();

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void ConnectionWindow::renderClientView()
{
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f), "Connected");
    if (!statusMsg_.empty()) ImGui::TextDisabled("%s", statusMsg_.c_str());
    ImGui::Spacing();

    if (ImGui::Button("Disconnect", ImVec2(-1.f, 30.f)))
        if (onDisconnect) onDisconnect();
}

void ConnectionWindow::renderHostedView()
{
    // Local address info + copy button
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f), "Hosting");
    if (!localIp_.empty() && localPort_ != 0) {
        ImGui::Spacing();
        char ep[280];
        snprintf(ep, sizeof(ep), "%s:%u", localIp_.c_str(), localPort_);
        ImGui::Text("Your address:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "%s", ep);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ep);
        ImGui::TextDisabled("Share this with your crew.");
    }
    if (!statusMsg_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", statusMsg_.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Lobby / participants table
    renderLobbyTable();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Stop Hosting — prominent red button at the bottom
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f,0.10f,0.10f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f,0.20f,0.20f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f,0.05f,0.05f,1.f));
    if (ImGui::Button("Stop Hosting", ImVec2(-1.f, 34.f))) {
        if (onStopHosting) onStopHosting();
        else if (onDisconnect) onDisconnect();
    }
    ImGui::PopStyleColor(3);
}

void ConnectionWindow::renderLobbyTable()
{
    if (!sess_ || !aircraftCfg_) {
        ImGui::TextDisabled("Session data not available.");
        return;
    }

    int count = static_cast<int>(sess_->participants().size());
    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f),
                       "Crew: %d", count);

    if (aircraftCfg_->fromSmartCopilot)
        ImGui::TextColored(ImVec4(1.f,0.7f,0.1f,1.f),
                           "smartcopilot.cfg -- zone/role features unavailable.");
    ImGui::Spacing();

    if (ImGui::BeginTable("##pilots", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(560.f, 220.f)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("ID",      ImGuiTableColumnFlags_WidthFixed,    28.f);
        ImGui::TableSetupColumn("Nick",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Zones",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed,   120.f);
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
                for (const auto& r : aircraftCfg_->roles) {
                    bool sel = (p.roleId == r.id);
                    if (ImGui::Selectable(r.name.c_str(), sel))
                        if (onRoleAssign) onRoleAssign(p.id, r.id);
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(3);
            if (aircraftCfg_->fromSmartCopilot) {
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
                    for (const auto& z : aircraftCfg_->zones) {
                        bool owns = std::find(newZones.begin(), newZones.end(), z.id) != newZones.end();
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
    ImGui::TextDisabled("[M] = Physics Master  |  Edit to customise zones");
}

} // namespace ui
} // namespace cp
