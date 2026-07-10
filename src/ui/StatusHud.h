#pragma once
#include "XPImguiWindow.h"
#include "../session/Session.h"
#include <functional>

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

    // Callbacks wired by the plugin to toggle other windows.
    std::function<void()> onToggleConn;
    std::function<void()> onToggleNotepad;

protected:
    void renderContent() override;

private:
    const Session* sess_         = nullptr;
    uint32_t       ping_ms_      = 0;
    float          packetLoss_   = 0.f;
    ImVec2         measuredSize_ = {0.f, 0.f};

    // Re-pins the XPLM window box so the right/bottom edges stay fixed at the
    // screen corner while the left/top grow as content changes.
    void reanchorBottomRight();
};

}
}
