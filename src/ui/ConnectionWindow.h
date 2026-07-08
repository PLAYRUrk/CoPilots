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

// Avoid ERROR -- Windows.h defines it as a macro
enum class ConnState { IDLE, CONNECTING, CONNECTED, CONNECT_ERROR };

// Single main window: shows Host/Join form when disconnected,
// switches to lobby view when connected as host (or simple disconnect when client).
class ConnectionWindow : public XPImguiWindow {
public:
    // Connection callbacks
    std::function<void(const ConnectionConfig&)> onHost;
    std::function<void(const ConnectionConfig&)> onJoin;
    std::function<void()>                        onDisconnect;

    // Lobby / admin callbacks (active when isHost_ && CONNECTED)
    std::function<void()>                                               onStopHosting;
    std::function<void(ParticipantId, const std::string&)>              onRoleAssign;
    std::function<void(ParticipantId, const std::vector<std::string>&)> onZoneAssign;
    std::function<void(ParticipantId)>                                  onKick;
    std::function<void(ParticipantId)>                                  onPhysicsMasterSet;

    bool init();
    void shutdown() { xpwShutdown(); }

    void setState(ConnState s, const std::string& msg = {});
    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

    void setLocalEndpoint(const std::string& ip, uint16_t port)
    { localIp_ = ip; localPort_ = port; }
    void setIsHost(bool h) { isHost_ = h; }

    // Live session & aircraft config data (for lobby view)
    void setData(const Session* s, const AircraftConfig* c) { sess_ = s; aircraftCfg_ = c; }

protected:
    void renderContent() override;

private:
    ConnState   state_     = ConnState::IDLE;
    std::string statusMsg_;
    bool        isHost_    = false;
    std::string localIp_;
    uint16_t    localPort_ = 0;
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

} // namespace ui
} // namespace cp
