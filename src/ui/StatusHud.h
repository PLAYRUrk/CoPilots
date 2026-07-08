#pragma once
#include "XPImguiWindow.h"
#include "../session/Session.h"

namespace cp {
namespace ui {

class StatusHud : public XPImguiWindow {
public:
    bool init();
    void shutdown() { xpwShutdown(); }

    void setStats(uint32_t ping_ms, float packetLoss)
    {
        ping_ms_    = ping_ms;
        packetLoss_ = packetLoss;
    }
    void setSession(const Session* s) { sess_ = s; }

    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

protected:
    void renderContent() override;

private:
    const Session* sess_       = nullptr;
    uint32_t       ping_ms_    = 0;
    float          packetLoss_ = 0.f;
};

}
}
