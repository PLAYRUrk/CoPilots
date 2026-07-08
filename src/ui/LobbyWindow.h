#pragma once
#include "XPImguiWindow.h"
#include "../session/Session.h"
#include "../config/Config.h"
#include <functional>

namespace cp {
namespace ui {

class LobbyWindow : public XPImguiWindow {
public:
    std::function<void()>                                                   onStopHosting;
    std::function<void(ParticipantId, const std::string& roleId)>          onRoleAssign;
    std::function<void(ParticipantId, const std::vector<std::string>& zones)> onZoneAssign;
    std::function<void(ParticipantId)>                                     onKick;
    std::function<void(ParticipantId)>                                     onPhysicsMasterSet;

    bool init();
    void shutdown() { xpwShutdown(); }

    // Update live data pointers (called after config load and session changes)
    void setData(const Session* s, const AircraftConfig* c) { sess_ = s; cfg_ = c; }

    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

protected:
    void renderContent() override;

private:
    const Session*        sess_ = nullptr;
    const AircraftConfig* cfg_  = nullptr;
};

} // namespace ui
} // namespace cp
