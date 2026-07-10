#include "XPImguiWindow.h"
#include "../Log.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <XPLM/XPLMGraphics.h>
#include <XPLM/XPLMUtilities.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <GL/gl.h>
#elif defined(__APPLE__)
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

#include <algorithm>

namespace cp {
namespace ui {

XPImguiWindow::~XPImguiWindow() { xpwShutdown(); }

bool XPImguiWindow::xpwInit(int l, int t, int r, int b,
                             XPLMWindowDecoration dec,
                             XPLMWindowLayer layer)
{
    xpL_ = l; xpT_ = t; xpR_ = r; xpB_ = b;

    imCtx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imCtx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename  = nullptr;
    Theme::apply();
    lastTime_ = std::chrono::steady_clock::now();

    XPLMCreateWindow_t p{};
    p.structSize               = sizeof(p);
    p.left   = l; p.top   = t;
    p.right  = r; p.bottom= b;
    p.visible                  = xpVisible_ ? 1 : 0;
    p.drawWindowFunc           = DrawCB;
    p.handleMouseClickFunc     = MouseCB;
    p.handleKeyFunc            = KeyCB;
    p.handleCursorFunc         = CursorCB;
    p.handleMouseWheelFunc     = ScrollCB;
    p.refcon                   = this;
    p.decorateAsFloatingWindow = dec;
    p.layer                    = layer;
    p.handleRightClickFunc     = RClickCB;
    xpWin_ = XPLMCreateWindowEx(&p);
    if (!xpWin_) return false;

    XPLMBringWindowToFront(xpWin_);
    return true;
}

void XPImguiWindow::xpwShutdown()
{
    if (hasKbFocus_) {
        XPLMTakeKeyboardFocus(0);
        hasKbFocus_ = false;
    }
    if (imCtx_) {
        ImGui::SetCurrentContext(imCtx_);
        if (glReady_) {
            ImGui_ImplOpenGL3_Shutdown();
            glReady_ = false;
        }
        ImGui::DestroyContext(imCtx_);
        imCtx_ = nullptr;
    }
    if (xpWin_) {
        XPLMDestroyWindow(xpWin_);
        xpWin_ = nullptr;
    }
}

void XPImguiWindow::xpwSetVisible(bool v)
{
    xpVisible_ = v;
    if (xpWin_) XPLMSetWindowIsVisible(xpWin_, v ? 1 : 0);
}

bool XPImguiWindow::xpwBeginWindow(const char* title, ImGuiWindowFlags extraFlags)
{
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        extraFlags;
    return ImGui::Begin(title, nullptr, flags);
}

void XPImguiWindow::xpwEndWindow()
{
    lastWindowSize_ = ImGui::GetWindowSize();
    ImGui::End();
}

void XPImguiWindow::clampToScreen(int& l, int& t, int& r, int& b) const
{
    int sl, st, sr, sb;
    XPLMGetScreenBoundsGlobal(&sl, &st, &sr, &sb);
    int w = r - l;
    int h = t - b;
    // Clamp right/left
    if (r > sr) { r = sr; l = r - w; }
    if (l < sl) { l = sl; r = l + w; }
    // Clamp top/bottom (XPLM: top > bottom, origin bottom-left)
    if (t > st) { t = st; b = t - h; }
    if (b < sb) { b = sb; t = b + h; }
}

void XPImguiWindow::xpwSetGeometry(int l, int t, int r, int b)
{
    clampToScreen(l, t, r, b);
    if (!xpWin_) return;
    xpL_ = l; xpT_ = t; xpR_ = r; xpB_ = b;
    XPLMSetWindowGeometry(xpWin_, l, t, r, b);
}

void XPImguiWindow::onDraw()
{
    if (!imCtx_ || !xpWin_) return;
    ImGui::SetCurrentContext(imCtx_);

    if (!glReady_) {
        if (!ImGui_ImplOpenGL3_Init("#version 150")) {
            Log("XPImguiWindow: ImGui_ImplOpenGL3_Init failed");
            return;
        }
        glReady_ = true;
    }

    XPLMGetWindowGeometry(xpWin_, &xpL_, &xpT_, &xpR_, &xpB_);
    int w = xpR_ - xpL_;
    int h = xpT_ - xpB_;
    if (w <= 0 || h <= 0) return;

    int screenW = 0, screenH = 0;
    XPLMGetScreenSize(&screenW, &screenH);
    screenH_ = screenH;

    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - lastTime_).count();
    lastTime_ = now;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize             = ImVec2(float(screenW), float(screenH));
    io.DisplayFramebufferScale = ImVec2(1.f, 1.f);
    io.DeltaTime               = (dt > 0.f && dt < 1.f) ? dt : (1.f / 60.f);

    int mx, my;
    XPLMGetMouseLocationGlobal(&mx, &my);
    io.AddMousePosEvent(float(mx), float(screenH - my));

    bool wantKb = io.WantCaptureKeyboard;
    if (wantKb != hasKbFocus_) {
        if (wantKb) XPLMTakeKeyboardFocus(xpWin_);
        else        XPLMTakeKeyboardFocus(0);
        hasKbFocus_ = wantKb;
    }

    GLint     vp[4];  glGetIntegerv(GL_VIEWPORT, vp);
    GLint     blendSrc, blendDst;
    glGetIntegerv(GL_BLEND_SRC, &blendSrc);
    glGetIntegerv(GL_BLEND_DST, &blendDst);
    GLboolean blend       = glIsEnabled(GL_BLEND);
    GLboolean depthTest   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean cullFace    = glIsEnabled(GL_CULL_FACE);
    GLint     scissorBox[4]; glGetIntegerv(GL_SCISSOR_BOX, scissorBox);

    glViewport(0, 0, screenW, screenH);
    XPLMSetGraphicsState(0, 1, 0, 1, 1, 0, 0);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(
        ImVec2(float(xpL_), float(screenH - xpT_)),
        ImGuiCond_Always);

    renderContent();

    titleBarH_ = ImGui::GetFrameHeight();

    if (lastWindowSize_.x > 10.f && lastWindowSize_.y > 10.f) {
        int newL = xpL_, newT = xpT_;
        int newR = xpL_ + (int)lastWindowSize_.x;
        int newB = xpT_  - (int)lastWindowSize_.y;
        clampToScreen(newL, newT, newR, newB);
        if (newL != xpL_ || newT != xpT_ || newR != xpR_ || newB != xpB_) {
            xpL_ = newL; xpT_ = newT; xpR_ = newR; xpB_ = newB;
            XPLMSetWindowGeometry(xpWin_, xpL_, xpT_, xpR_, xpB_);
        }
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBlendFunc(GLenum(blendSrc), GLenum(blendDst));
    if (blend)       glEnable(GL_BLEND);        else glDisable(GL_BLEND);
    if (depthTest)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (cullFace)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
}

int XPImguiWindow::onMouse(int x, int y, XPLMMouseStatus status)
{
    if (!imCtx_) return 0;
    ImGui::SetCurrentContext(imCtx_);
    ImGuiIO& io = ImGui::GetIO();

    float imX = float(x);
    float imY = float(screenH_ - y);
    io.AddMousePosEvent(imX, imY);

    if (status == xplm_MouseDown) {
        io.AddMouseButtonEvent(0, true);
        // Take keyboard focus immediately on any click so InputText works on first keypress
        if (!hasKbFocus_) {
            XPLMTakeKeyboardFocus(xpWin_);
            hasKbFocus_ = true;
        }

        float winTop = float(screenH_ - xpT_);
        bool inTitleBar = (imY >= winTop && imY < winTop + titleBarH_);
        if (inTitleBar) {
            isDragging_  = true;
            dragAnchorX_ = x; dragAnchorY_ = y;
            dragStartL_  = xpL_; dragStartT_ = xpT_;
        }
    } else if (status == xplm_MouseDrag) {
        if (isDragging_) {
            int dx = x - dragAnchorX_;
            int dy = y - dragAnchorY_;
            int nL = dragStartL_ + dx;
            int nT = dragStartT_ + dy;
            int nR = nL + (xpR_ - xpL_);
            int nB = nT - (xpT_ - xpB_);
            clampToScreen(nL, nT, nR, nB);
            XPLMSetWindowGeometry(xpWin_, nL, nT, nR, nB);
            xpL_ = nL; xpT_ = nT; xpR_ = nR; xpB_ = nB;
        }
    } else if (status == xplm_MouseUp) {
        io.AddMouseButtonEvent(0, false);
        isDragging_ = false;
    }

    return 1;
}

int XPImguiWindow::onScroll(int x, int y, int , int clicks)
{
    if (!imCtx_) return 0;
    ImGui::SetCurrentContext(imCtx_);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(float(x), float(screenH_ - y));
    io.AddMouseWheelEvent(0.f, float(clicks));
    return 1;
}

void XPImguiWindow::onKey(char key, XPLMKeyFlags flags, char vk, int )
{
    if (!imCtx_) return;
    ImGui::SetCurrentContext(imCtx_);
    ImGuiIO& io = ImGui::GetIO();

    bool down = (flags & xplm_DownFlag) != 0;

    io.AddKeyEvent(ImGuiMod_Shift, (flags & xplm_ShiftFlag)     != 0);
    io.AddKeyEvent(ImGuiMod_Ctrl,  (flags & xplm_ControlFlag)   != 0);
    io.AddKeyEvent(ImGuiMod_Alt,   (flags & xplm_OptionAltFlag) != 0);

    auto mapVK = [](unsigned char v) -> ImGuiKey {
        switch (v) {
        case 0x08: return ImGuiKey_Backspace;
        case 0x0D: return ImGuiKey_Enter;
        case 0x1B: return ImGuiKey_Escape;
        case 0x09: return ImGuiKey_Tab;
        case 0x25: return ImGuiKey_LeftArrow;
        case 0x26: return ImGuiKey_UpArrow;
        case 0x27: return ImGuiKey_RightArrow;
        case 0x28: return ImGuiKey_DownArrow;
        case 0x2E: return ImGuiKey_Delete;
        case 0x24: return ImGuiKey_Home;
        case 0x23: return ImGuiKey_End;
        case 0x41: return ImGuiKey_A;
        case 0x43: return ImGuiKey_C;
        case 0x56: return ImGuiKey_V;
        case 0x58: return ImGuiKey_X;
        case 0x5A: return ImGuiKey_Z;
        default:   return ImGuiKey_None;
        }
    };

    ImGuiKey imKey = mapVK(static_cast<unsigned char>(vk));
    if (imKey != ImGuiKey_None)
        io.AddKeyEvent(imKey, down);

    if (down && key >= 32 && key < 127)
        io.AddInputCharacter(static_cast<unsigned int>(static_cast<unsigned char>(key)));
}

void XPImguiWindow::DrawCB(XPLMWindowID, void* ref)
{
    static_cast<XPImguiWindow*>(ref)->onDraw();
}

int XPImguiWindow::MouseCB(XPLMWindowID, int x, int y, XPLMMouseStatus s, void* ref)
{
    return static_cast<XPImguiWindow*>(ref)->onMouse(x, y, s);
}

int XPImguiWindow::RClickCB(XPLMWindowID, int, int, XPLMMouseStatus, void*)
{
    return 1;
}

int XPImguiWindow::ScrollCB(XPLMWindowID, int x, int y, int w, int c, void* ref)
{
    return static_cast<XPImguiWindow*>(ref)->onScroll(x, y, w, c);
}

void XPImguiWindow::KeyCB(XPLMWindowID, char key, XPLMKeyFlags flags,
                           char vk, void* ref, int losingFocus)
{
    static_cast<XPImguiWindow*>(ref)->onKey(key, flags, vk, losingFocus);
}

XPLMCursorStatus XPImguiWindow::CursorCB(XPLMWindowID, int, int, void*)
{
    return xplm_CursorDefault;
}

}
}
