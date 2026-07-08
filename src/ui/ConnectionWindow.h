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
    std::string nick   = "Pilot";
    std::string host   = "127.0.0.1";
    uint16_t    port   = 56900;
    bool        asHost = true;
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

    bool init();
    void shutdown() { xpwShutdown(); }

    void setState(ConnState s, const std::string& msg = {});
    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

    void setLocalEndpoint(const std::vector<std::string>& ips, uint16_t port)
    { localIps_ = ips; localPort_ = port; }
    void setIsHost(bool h) { isHost_ = h; }

    void setData(const Session* s, const AircraftConfig* c) { sess_ = s; aircraftCfg_ = c; }

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

    char nickBuf_[64]  = "Pilot";
    char portBuf_[8]   = "56900";
    char addrBuf_[272] = "127.0.0.1:56900";

    void renderConnectForm();
    void renderHostedView();
    void renderClientView();
    void renderLobbyTable();
};

}
}
