#pragma once

#include "../net/Protocol.h"
#include "../net/NetThread.h"
#include "../session/Session.h"

#include <XPLM/XPLMDataAccess.h>
#include <chrono>
#include <map>

namespace cp {

// Shared cockpit laser pointer.  While the user holds the CoPilots/pointer_hold
// command, a ray is cast from the camera through the mouse cursor; the depth
// buffer gives the hit point on the cockpit geometry, which is converted to the
// aircraft-body frame and streamed over UDP.  Every participant renders every
// active pointer as a colored billboard dot (color = notepad palette by pid).
//
// All entry points run on the main thread (command handler, flight loop, draw
// callback); the only cross-thread interaction is the thread-safe outUdp queue.
class PointerSync {
public:
    void init(Session* session, net::NetThread* net);
    void reset();

    // User opt-in (Prefs::pointerEnabled, off by default).  Governs the local
    // emitter only: a disabled participant still sees everyone else's pointers.
    void setEnabled(bool e);
    bool enabled() const { return enabled_; }

    // From the CoPilots/pointer_hold command handler (Begin/End).
    void setHolding(bool held);

    // From the flight loop: an incoming POINTER_STATE datagram.
    void onUdpDatagram(const uint8_t* data, size_t len);

    // From the XPLM draw callback (3-D phase, matrices valid): computes the
    // local hit point while holding, sends it, and renders all active pointers.
    void onDraw();

private:
    struct Remote {
        bool   active = false;
        float  x = 0, y = 0, z = 0;                       // aircraft-body frame
        std::chrono::steady_clock::time_point lastRecv;
    };

    Session*        session_ = nullptr;
    net::NetThread* net_     = nullptr;

    bool  available_    = false;  // matrix datarefs found at init
    bool  enabled_      = false;  // user opt-in (Prefs)
    bool  holding_      = false;
    bool  haveLocalHit_ = false;
    float localHit_[3]  = {};     // own hit, aircraft-body frame
    std::chrono::steady_clock::time_point lastSend_{};

    std::map<uint8_t, Remote> remotes_;

    XPLMDataRef drWorldMtx_ = nullptr;   // sim/graphics/view/world_matrix
    XPLMDataRef drProjMtx_  = nullptr;   // sim/graphics/view/projection_matrix_3d
    XPLMDataRef drViewport_ = nullptr;   // sim/graphics/view/viewport [l,b,r,t]
    XPLMDataRef drLocalX_   = nullptr;
    XPLMDataRef drLocalY_   = nullptr;
    XPLMDataRef drLocalZ_   = nullptr;
    XPLMDataRef drPsi_      = nullptr;
    XPLMDataRef drTheta_    = nullptr;
    XPLMDataRef drPhi_      = nullptr;

    // Hide a remote pointer when no packet refreshed it for this long.
    static constexpr double STALE_S       = 0.5;
    static constexpr double SEND_PERIOD_S = 1.0 / 30.0;

    bool computeLocalHit(const float mv[16], const float pj[16], const int vp[4]);
    void sendState(bool active);
    void renderAll(const float mv[16]);
};

}
