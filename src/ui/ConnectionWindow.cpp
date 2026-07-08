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

bool ConnectionWindow::init()
{
    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    (void)scrR; (void)scrB;
    return xpwInit(scrL + 40, scrT - 40, scrL + 440, scrT - 200);
}

void ConnectionWindow::setState(ConnState s, const std::string& msg)
{
    state_     = s;
    statusMsg_ = msg;
    if (s != ConnState::CONNECTED) {
        isHost_ = false; localPort_ = 0; localIps_.clear();
        pendingJoins_.clear();
        hasPendingControl_ = false;
    }
}

void ConnectionWindow::addPendingJoin(uint8_t connId, const std::string& nick)
{
    for (const auto& p : pendingJoins_) if (p.connId == connId) return;
    pendingJoins_.push_back({connId, nick});
}

void ConnectionWindow::removePendingJoin(uint8_t connId)
{
    pendingJoins_.erase(
        std::remove_if(pendingJoins_.begin(), pendingJoins_.end(),
                       [connId](const PendingJoin& p){ return p.connId == connId; }),
        pendingJoins_.end());
}

void ConnectionWindow::setPendingControlRequest(ParticipantId pid, const std::string& nick)
{
    hasPendingControl_ = true;
    pendingControlPid_ = pid;
    pendingControlNick_ = nick;
}

void ConnectionWindow::clearPendingControlRequest()
{
    hasPendingControl_ = false;
}

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

static void renderDrHashLine(const AircraftConfig* cfg)
{
    if (!cfg || cfg->drListHash == 0) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "DR list: %zu datarefs  [0x%08X]",
             cfg->datarefs.size(), cfg->drListHash);
    ImGui::TextDisabled("%s", buf);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##hash")) ImGui::SetClipboardText(buf);
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
            ImGui::Text("Lobby Password (optional)");
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::InputText("##pass_host", passBuf_, sizeof(passBuf_),
                                 ImGuiInputTextFlags_Password))
                connCfg_.password = passBuf_;

            ImGui::Spacing();
            ImGui::Checkbox("Require join approval", &connCfg_.requireJoinApproval);
            ImGui::Checkbox("Require control transfer approval", &connCfg_.requireControlApproval);

            ImGui::Spacing();
            renderDrHashLine(aircraftCfg_);
            ImGui::Spacing();
            if (state_ == ConnState::CONNECT_ERROR)
                ImGui::TextColored(ImVec4(0.95f,0.3f,0.3f,1.f), "Error: %s", statusMsg_.c_str());
            else if (state_ == ConnState::CONNECTING)
                ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "Connecting...");
            ImGui::Spacing();

            bool busy = (state_ == ConnState::CONNECTING);
            if (busy) {
                if (ImGui::Button("Cancel", ImVec2(-1.f, 30.f)))
                    if (onDisconnect) onDisconnect();
            } else {
                if (ImGui::Button("Start Hosting", ImVec2(-1.f, 30.f)))
                    if (onHost) onHost(connCfg_);
            }

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
            ImGui::Text("Password (if required)");
            ImGui::SetNextItemWidth(200.f);
            if (ImGui::InputText("##pass_join", passBuf_, sizeof(passBuf_),
                                 ImGuiInputTextFlags_Password))
                connCfg_.password = passBuf_;

            ImGui::Spacing();
            renderDrHashLine(aircraftCfg_);
            ImGui::Spacing();
            if (state_ == ConnState::CONNECT_ERROR)
                ImGui::TextColored(ImVec4(0.95f,0.3f,0.3f,1.f), "Error: %s", statusMsg_.c_str());
            else if (state_ == ConnState::CONNECTING)
                ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "Connecting...");
            ImGui::Spacing();

            bool busy = (state_ == ConnState::CONNECTING);
            if (busy) {
                if (ImGui::Button("Cancel", ImVec2(-1.f, 30.f)))
                    if (onDisconnect) onDisconnect();
            } else {
                if (ImGui::Button("Join", ImVec2(-1.f, 30.f))) {
                    std::string h; uint16_t p = 0;
                    if (parseAddr(addrBuf_, h, p)) { connCfg_.host = h; connCfg_.port = p; }
                    if (onJoin) onJoin(connCfg_);
                }
            }

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

    // Control denied notification
    if (controlDeniedTimer_ > 0.f) {
        controlDeniedTimer_ -= ImGui::GetIO().DeltaTime;
        ImGui::TextColored(ImVec4(0.95f,0.4f,0.4f,1.f),
                           "Controls denied: %s", controlDeniedMsg_.c_str());
        ImGui::Spacing();
    }

    bool isPhysMaster = sess_ && (sess_->myId() == sess_->physicsMasterId());
    if (!isPhysMaster) {
        if (ImGui::Button("Request Controls", ImVec2(-1.f, 28.f)))
            if (onRequestControl) onRequestControl();
        ImGui::Spacing();
    } else {
        ImGui::TextColored(ImVec4(kAccentR,kAccentG,kAccentB,1.f), "You have controls");
        ImGui::Spacing();
    }

    if (ImGui::Button("Disconnect", ImVec2(-1.f, 30.f)))
        if (onDisconnect) onDisconnect();
}

void ConnectionWindow::renderHostedView()
{
    ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f), "Hosting");
    if (!localIps_.empty() && localPort_ != 0) {
        ImGui::Spacing();
        ImGui::Text("Your address(es):");
        for (const auto& ip : localIps_) {
            char ep[280];
            snprintf(ep, sizeof(ep), "%s:%u", ip.c_str(), localPort_);
            ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "  %s", ep);
            ImGui::SameLine();
            ImGui::PushID(ep);
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(ep);
            ImGui::PopID();
        }
        ImGui::TextDisabled("Use your public (WAN) IP for internet sessions.");
    }
    if (!statusMsg_.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", statusMsg_.c_str());
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    renderPendingJoins();
    renderPendingControlRequest();
    renderLobbyTable();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.70f,0.10f,0.10f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.90f,0.20f,0.20f,1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f,0.05f,0.05f,1.f));
    if (ImGui::Button("Stop Hosting", ImVec2(-1.f, 34.f))) {
        if (onStopHosting) onStopHosting();
        else if (onDisconnect) onDisconnect();
    }
    ImGui::PopStyleColor(3);
}

void ConnectionWindow::renderPendingJoins()
{
    if (pendingJoins_.empty()) return;
    ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f), "Pending connections:");
    for (auto it = pendingJoins_.begin(); it != pendingJoins_.end(); ) {
        ImGui::PushID(it->connId);
        ImGui::Text("  %s", it->nick.c_str());
        ImGui::SameLine();
        bool accepted = false, rejected = false;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.6f,0.1f,1.f));
        if (ImGui::SmallButton("Accept")) accepted = true;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.1f,0.1f,1.f));
        if (ImGui::SmallButton("Reject")) rejected = true;
        ImGui::PopStyleColor();
        ImGui::PopID();
        if (accepted) {
            if (onAcceptJoin) onAcceptJoin(it->connId);
            it = pendingJoins_.erase(it);
        } else if (rejected) {
            if (onRejectJoin) onRejectJoin(it->connId);
            it = pendingJoins_.erase(it);
        } else {
            ++it;
        }
    }
    ImGui::Separator();
    ImGui::Spacing();
}

void ConnectionWindow::renderPendingControlRequest()
{
    if (!hasPendingControl_) return;
    ImGui::TextColored(ImVec4(1.f,0.8f,0.2f,1.f),
                       "  \"%s\" requests aircraft controls", pendingControlNick_.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f,0.6f,0.1f,1.f));
    if (ImGui::SmallButton("Grant")) {
        if (onGrantControl) onGrantControl(pendingControlPid_);
        hasPendingControl_ = false;
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.1f,0.1f,1.f));
    if (ImGui::SmallButton("Deny")) {
        if (onDenyControl) onDenyControl(pendingControlPid_);
        hasPendingControl_ = false;
    }
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
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

    if (aircraftCfg_->fromSmartCopilot) {
        ImGui::TextColored(ImVec4(1.f,0.7f,0.1f,1.f),
                           "smartcopilot.cfg -- zone/role features unavailable.");
        renderDrHashLine(aircraftCfg_);
    }
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
            if (p.isPhysicsMaster && p.isWeatherMaster)
                ImGui::TextColored(ImVec4(kAccentR,kAccentG,kAccentB,1.f),
                                   "[M][W] %s", p.nick.c_str());
            else if (p.isPhysicsMaster)
                ImGui::TextColored(ImVec4(kAccentR,kAccentG,kAccentB,1.f),
                                   "[M] %s", p.nick.c_str());
            else if (p.isWeatherMaster)
                ImGui::TextColored(ImVec4(0.4f,0.8f,1.f,1.f),
                                   "[W] %s", p.nick.c_str());
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
                if (ImGui::SmallButton("Phys##pm"))
                    if (onPhysicsMasterSet) onPhysicsMasterSet(p.id);
                ImGui::SameLine();
            }
            if (!p.isWeatherMaster) {
                if (ImGui::SmallButton("Wthr##wm"))
                    if (onWeatherMasterSet) onWeatherMasterSet(p.id);
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

}
}
