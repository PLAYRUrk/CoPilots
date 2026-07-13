#pragma once
#include "XPImguiWindow.h"
#include "../session/Session.h"
#include "../config/Config.h"
#include <functional>
#include <string>
#include <cstdint>
#include <vector>

namespace cp {
namespace ui {

struct ConnectionConfig {
    std::string nick     = "Pilot";
    std::string host     = "127.0.0.1";
    uint16_t    port     = 56900;
    bool        asHost   = true;
    std::string password = "";
    // Local interface IP to bind the host sockets to. Empty = all interfaces.
    // Selecting the physical adapter's IP routes traffic around an active VPN
    // (strong-host model), so router port-forwarding keeps working.
    std::string bindIp   = "";
    bool        requireJoinApproval    = true;
    bool        requireControlApproval = true;
};

enum class ConnState { IDLE, CONNECTING, CONNECTED, CONNECT_ERROR };

class ConnectionWindow : public XPImguiWindow {
public:
    std::function<void(const ConnectionConfig&)> onHost;
    std::function<void(const ConnectionConfig&)> onJoin;
    std::function<void()>                        onDisconnect;

    std::function<void()>                                               onStopHosting;
    std::function<void(ParticipantId, const std::string&)>              onRoleAssign;
    std::function<void(ParticipantId, const std::vector<std::string>&)> onZoneAssign;
    std::function<void(ParticipantId)>                                  onKick;
    std::function<void(ParticipantId)>                                  onPhysicsMasterSet;
    std::function<void(ParticipantId)>                                  onWeatherMasterSet;

    // Pending join approval (host only)
    std::function<void(uint8_t connId)>                    onAcceptJoin;
    std::function<void(uint8_t connId)>                    onRejectJoin;
    // Aircraft config library (GitHub) download
    std::function<void()>                                  onDownloadConfig;
    // Manual pick: download a specific library entry (folder id)
    std::function<void(const std::string& folder, const std::string& aircraft)> onDownloadConfigEntry;
    void setDownloadStatus(const std::string& s) { downloadStatus_ = s; }
    void setLibraryList(const std::vector<std::pair<std::string,std::string>>& l)
    { libList_ = l; if (libSel_ >= (int)libList_.size()) libSel_ = 0; }
    // Control transfer
    std::function<void()>                                  onRequestControl;
    std::function<void(ParticipantId)>                     onGrantControl;
    std::function<void(ParticipantId)>                     onDenyControl;

    bool init();
    void shutdown() { xpwShutdown(); }

    void setState(ConnState s, const std::string& msg = {});
    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

    void setLocalEndpoint(const std::vector<std::string>& ips, uint16_t port)
    { localIps_ = ips; localPort_ = port; }
    void setIsHost(bool h) { isHost_ = h; }

    void setData(const Session* s, const AircraftConfig* c) { sess_ = s; aircraftCfg_ = c; }

    // Pending join queue (host only)
    struct PendingJoin { uint8_t connId; std::string nick; };
    void addPendingJoin(uint8_t connId, const std::string& nick);
    void removePendingJoin(uint8_t connId);

    // Control transfer request (host only)
    void setPendingControlRequest(ParticipantId pid, const std::string& nick);
    void clearPendingControlRequest();
    // Show a transient notification on client side
    void showControlDenied(const std::string& reason)
    { controlDeniedMsg_ = reason; controlDeniedTimer_ = 5.f; }

protected:
    void renderContent() override;

private:
    ConnState   state_     = ConnState::IDLE;
    std::string statusMsg_;
    bool                     isHost_    = false;
    std::vector<std::string> localIps_;
    uint16_t                 localPort_ = 0;
    ConnectionConfig connCfg_;

    const Session*        sess_       = nullptr;
    const AircraftConfig* aircraftCfg_ = nullptr;

    char nickBuf_[64]   = "Pilot";
    char portBuf_[8]    = "56900";
    char addrBuf_[272]  = "127.0.0.1:56900";
    char passBuf_[64]   = "";

    // Interface picker for the Host tab (bind IP). Lazily filled on first render.
    std::vector<std::string> bindIps_;
    bool                     bindIpsLoaded_ = false;
    int                      bindIpSel_     = 0;   // 0 = Auto (all interfaces)

    // Pending joins (host)
    std::vector<PendingJoin> pendingJoins_;

    // Pending control request (host)
    bool          hasPendingControl_   = false;
    ParticipantId pendingControlPid_   = 0;
    std::string   pendingControlNick_;

    // Control denied notification (client)
    std::string controlDeniedMsg_;
    float       controlDeniedTimer_ = 0.f;

    // Config library download status line + manual picker (folder, display name)
    std::string downloadStatus_;
    std::vector<std::pair<std::string,std::string>> libList_;
    int         libSel_ = 0;

    void renderConnectForm();
    void renderHostedView();
    void renderClientView();
    void renderLobbyTable();
    void renderPendingJoins();
    void renderPendingControlRequest();
};

}
}
