#pragma once
#include "Theme.h"
#include <imgui.h>
#include <XPLM/XPLMDisplay.h>
#include <chrono>

namespace cp {
namespace ui {

class XPImguiWindow {
public:
    virtual ~XPImguiWindow();

    bool xpwInit(int l, int t, int r, int b,
                 XPLMWindowDecoration dec = xplm_WindowDecorationSelfDecorated,
                 XPLMWindowLayer     layer = xplm_WindowLayerFloatingWindows);
    void xpwShutdown();
    void xpwSetVisible(bool v);
    bool xpwVisible() const { return xpVisible_; }

protected:
    virtual void renderContent() = 0;

    int xpwWidth()  const { return xpR_ - xpL_; }
    int xpwHeight() const { return xpT_ - xpB_; }

    // Convenience wrappers: call these instead of ImGui::Begin/End in renderContent().
    // xpwEndWindow() captures the window size so onDraw() can auto-resize the XPLM window.
    bool xpwBeginWindow(const char* title, ImGuiWindowFlags extraFlags = 0);
    void xpwEndWindow();

    ImVec2 lastWindowSize_ = {0.f, 0.f};

private:
    XPLMWindowID  xpWin_   = nullptr;
    ImGuiContext* imCtx_   = nullptr;
    bool          glReady_ = false;
    bool          xpVisible_ = true;

    int xpL_ = 0, xpT_ = 0, xpR_ = 0, xpB_ = 0;

    bool isDragging_  = false;
    int  dragAnchorX_ = 0, dragAnchorY_ = 0;
    int  dragStartL_  = 0, dragStartT_  = 0;

    float titleBarH_  = 20.f;
    int   screenH_    = 0;
    bool  hasKbFocus_ = false;  // tracks whether we currently own XPLM keyboard focus
    std::chrono::steady_clock::time_point lastTime_;

    void onDraw();
    int  onMouse(int x, int y, XPLMMouseStatus s);
    int  onScroll(int x, int y, int wheel, int clicks);
    void onKey(char key, XPLMKeyFlags flags, char vk, int losingFocus);

    static void             DrawCB(XPLMWindowID, void*);
    static int              MouseCB(XPLMWindowID, int, int, XPLMMouseStatus, void*);
    static int              RClickCB(XPLMWindowID, int, int, XPLMMouseStatus, void*);
    static int              ScrollCB(XPLMWindowID, int, int, int, int, void*);
    static void             KeyCB(XPLMWindowID, char, XPLMKeyFlags, char, void*, int);
    static XPLMCursorStatus CursorCB(XPLMWindowID, int, int, void*);
};

} // namespace ui
} // namespace cp
