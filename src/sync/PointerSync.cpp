#include "PointerSync.h"
#include "../Log.h"
#include "../ui/Notepad.h"   // colorForParticipant

#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMGraphics.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <GL/gl.h>
#elif defined(__APPLE__)
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

#include <cmath>
#include <cstring>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace cp {

// ── 4×4 column-major helpers (GL convention) ────────────────────────────────

static void mul4x4(const float a[16], const float b[16], float out[16])
{
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0.f;
            for (int k = 0; k < 4; ++k)
                s += a[k*4 + row] * b[col*4 + k];
            out[col*4 + row] = s;
        }
}

// General 4×4 inverse via adjugate (the classic gluInvertMatrix routine).
static bool invert4x4(const float m[16], float inv[16])
{
    float t[16];
    t[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
           + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    t[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
           - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    t[8]  =  m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
           + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    t[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
           - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    t[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
           - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    t[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
           + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    t[9]  = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
           - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    t[13] =  m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
           + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    t[2]  =  m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15]
           + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    t[6]  = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15]
           - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    t[10] =  m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15]
           + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    t[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14]
           - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    t[3]  = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11]
           - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    t[7]  =  m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11]
           + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    t[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11]
           - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    t[15] =  m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10]
           + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*t[0] + m[1]*t[4] + m[2]*t[8] + m[3]*t[12];
    if (det == 0.f) return false;
    det = 1.f / det;
    for (int i = 0; i < 16; ++i) inv[i] = t[i] * det;
    return true;
}

// Aircraft-body → world rotation, 3×3 row-major.  Matches the canonical XP11
// OpenGL drawing order for aircraft-relative geometry:
//   glRotatef(-psi, 0,1,0); glRotatef(theta, 1,0,0); glRotatef(-phi, 0,0,1);
// Body axes: x = right wing, y = up, z = tail (nose = -z); world: X east,
// Y up, Z south.  Both the sender and every receiver build R the same way from
// their own (synced) psi/theta/phi, so any convention error cancels out.
static void acfToWorldMatrix(float psiDeg, float thetaDeg, float phiDeg, float R[9])
{
    const float d2r = static_cast<float>(M_PI / 180.0);
    float sy = sinf(-psiDeg  * d2r), cy = cosf(-psiDeg  * d2r);
    float sp = sinf(thetaDeg * d2r), cp = cosf(thetaDeg * d2r);
    float sr = sinf(-phiDeg  * d2r), cr = cosf(-phiDeg  * d2r);

    // R = Ry(-psi) * Rx(theta) * Rz(-phi)
    R[0] =  cy*cr + sy*sp*sr;  R[1] = -cy*sr + sy*sp*cr;  R[2] =  sy*cp;
    R[3] =  cp*sr;             R[4] =  cp*cr;             R[5] = -sp;
    R[6] = -sy*cr + cy*sp*sr;  R[7] =  sy*sr + cy*sp*cr;  R[8] =  cy*cp;
}

// ── PointerSync ──────────────────────────────────────────────────────────────

void PointerSync::init(Session* session, net::NetThread* net)
{
    session_ = session;
    net_     = net;

    drWorldMtx_ = XPLMFindDataRef("sim/graphics/view/world_matrix");
    drProjMtx_  = XPLMFindDataRef("sim/graphics/view/projection_matrix_3d");
    drViewport_ = XPLMFindDataRef("sim/graphics/view/viewport");
    drLocalX_   = XPLMFindDataRef("sim/flightmodel/position/local_x");
    drLocalY_   = XPLMFindDataRef("sim/flightmodel/position/local_y");
    drLocalZ_   = XPLMFindDataRef("sim/flightmodel/position/local_z");
    drPsi_      = XPLMFindDataRef("sim/flightmodel/position/psi");
    drTheta_    = XPLMFindDataRef("sim/flightmodel/position/theta");
    drPhi_      = XPLMFindDataRef("sim/flightmodel/position/phi");

    available_ = drWorldMtx_ && drProjMtx_ && drViewport_
              && drLocalX_ && drLocalY_ && drLocalZ_
              && drPsi_ && drTheta_ && drPhi_;
    if (!available_)
        Log("PointerSync: matrix/position datarefs missing — pointer disabled "
            "(requires X-Plane 11.10+ OpenGL)");
}

void PointerSync::reset()
{
    holding_      = false;
    haveLocalHit_ = false;
    remotes_.clear();
}

void PointerSync::setEnabled(bool e)
{
    if (!e && holding_) setHolding(false);  // clean release while mid-hold
    enabled_ = e;
}

void PointerSync::setHolding(bool held)
{
    if (!available_) return;
    if (held && !enabled_) return;  // presses gated by the opt-in; releases pass
    if (holding_ && !held) {
        // Release: UDP is lossy — repeat the deactivation a few times.
        for (int i = 0; i < 3; ++i) sendState(false);
        haveLocalHit_ = false;
    }
    holding_ = held;
}

void PointerSync::onUdpDatagram(const uint8_t* data, size_t len)
{
    if (len < sizeof(proto::PointerState) || !session_) return;
    proto::PointerState ps;
    std::memcpy(&ps, data, sizeof(ps));
    if (ps.sender_id == session_->myId()) return;  // relayed echo of our own pointer

    Remote& rm = remotes_[ps.sender_id];
    rm.active   = (ps.active != 0);
    rm.x        = ps.x;
    rm.y        = ps.y;
    rm.z        = ps.z;
    rm.lastRecv = std::chrono::steady_clock::now();
}

void PointerSync::sendState(bool active)
{
    if (!net_ || !session_) return;
    proto::PointerState ps;
    ps.sender_id = session_->myId();
    ps.active    = active ? 1 : 0;
    ps.x = localHit_[0];
    ps.y = localHit_[1];
    ps.z = localHit_[2];

    net::UdpDatagram dg;
    dg.data.assign(reinterpret_cast<const uint8_t*>(&ps),
                   reinterpret_cast<const uint8_t*>(&ps) + sizeof(ps));
    // Empty `to`: client → server; on the host the server loop broadcasts.
    net_->outUdp.push(std::move(dg));
}

bool PointerSync::computeLocalHit(const float mv[16], const float pj[16], const int vp[4])
{
    // Mouse in global desktop boxels → framebuffer pixels (handles HiDPI).
    int mx = 0, my = 0;
    XPLMGetMouseLocationGlobal(&mx, &my);
    int sL, sT, sR, sB;
    XPLMGetScreenBoundsGlobal(&sL, &sT, &sR, &sB);
    const float vpW = static_cast<float>(vp[2] - vp[0]);
    const float vpH = static_cast<float>(vp[3] - vp[1]);
    if (vpW <= 0.f || vpH <= 0.f || sR == sL || sT == sB) return false;

    const float fx = static_cast<float>(mx - sL) / static_cast<float>(sR - sL);
    const float fy = static_cast<float>(my - sB) / static_cast<float>(sT - sB);
    if (fx < 0.f || fx > 1.f || fy < 0.f || fy > 1.f) return false;

    const float px = vp[0] + fx * vpW;
    const float py = vp[1] + fy * vpH;

    // Depth under the cursor.  Valid here because the callback runs after the
    // 3-D scene (incl. cockpit) has been drawn.
    float depth = 1.f;
    glReadPixels(static_cast<GLint>(px), static_cast<GLint>(py), 1, 1,
                 GL_DEPTH_COMPONENT, GL_FLOAT, &depth);

    float mvp[16], inv[16];
    mul4x4(pj, mv, mvp);
    if (!invert4x4(mvp, inv)) return false;

    const float ndcX = 2.f * fx - 1.f;
    const float ndcY = 2.f * fy - 1.f;

    auto unproject = [&](float ndcZ, float out[3]) -> bool {
        const float v[4] = {ndcX, ndcY, ndcZ, 1.f};
        float w[4];
        for (int row = 0; row < 4; ++row)
            w[row] = inv[0*4+row]*v[0] + inv[1*4+row]*v[1]
                   + inv[2*4+row]*v[2] + inv[3*4+row]*v[3];
        if (w[3] == 0.f) return false;
        out[0] = w[0] / w[3];
        out[1] = w[1] / w[3];
        out[2] = w[2] / w[3];
        return true;
    };

    float hit[3];
    if (depth < 0.9999f) {
        if (!unproject(2.f * depth - 1.f, hit)) return false;
    } else {
        // Sky / far plane: fall back to a point 20 m along the mouse ray.
        float nearP[3], farP[3];
        if (!unproject(-1.f, nearP) || !unproject(0.9f, farP)) return false;
        float d[3] = {farP[0]-nearP[0], farP[1]-nearP[1], farP[2]-nearP[2]};
        float dl = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (dl < 1e-6f) return false;
        for (int i = 0; i < 3; ++i) hit[i] = nearP[i] + d[i] / dl * 20.f;
    }

    // World → aircraft-body frame.
    const double axd = XPLMGetDatad(drLocalX_);
    const double ayd = XPLMGetDatad(drLocalY_);
    const double azd = XPLMGetDatad(drLocalZ_);
    float R[9];
    acfToWorldMatrix(XPLMGetDataf(drPsi_), XPLMGetDataf(drTheta_),
                     XPLMGetDataf(drPhi_), R);
    const float dx = static_cast<float>(hit[0] - axd);
    const float dy = static_cast<float>(hit[1] - ayd);
    const float dz = static_cast<float>(hit[2] - azd);
    // acf = Rᵀ · (hit − pos)
    const float bx = R[0]*dx + R[3]*dy + R[6]*dz;
    const float by = R[1]*dx + R[4]*dy + R[7]*dz;
    const float bz = R[2]*dx + R[5]*dy + R[8]*dz;
    if (bx*bx + by*by + bz*bz > 100.f * 100.f) return false;  // bogus depth read

    localHit_[0] = bx;
    localHit_[1] = by;
    localHit_[2] = bz;
    return true;
}

void PointerSync::renderAll(const float mv[16])
{
    // Camera basis (rows of the modelview) for billboarding.
    const float camR[3] = {mv[0], mv[4], mv[8]};
    const float camU[3] = {mv[1], mv[5], mv[9]};

    const double axd = XPLMGetDatad(drLocalX_);
    const double ayd = XPLMGetDatad(drLocalY_);
    const double azd = XPLMGetDatad(drLocalZ_);
    float R[9];
    acfToWorldMatrix(XPLMGetDataf(drPsi_), XPLMGetDataf(drTheta_),
                     XPLMGetDataf(drPhi_), R);

    // Depth test off: the dot sits on a surface, so bleed-through is minor and
    // far preferable to z-fighting with the panel it marks.
    XPLMSetGraphicsState(0 /*fog*/, 0 /*tex*/, 0 /*lighting*/, 0 /*alphaTest*/,
                         1 /*blend*/, 0 /*depthTest*/, 0 /*depthWrite*/);
    GLint prevSrc = GL_SRC_ALPHA, prevDst = GL_ONE_MINUS_SRC_ALPHA;
    glGetIntegerv(GL_BLEND_SRC, &prevSrc);   // GL 1.1 names (Windows gl.h)
    glGetIntegerv(GL_BLEND_DST, &prevDst);

    const auto now = std::chrono::steady_clock::now();

    auto drawDot = [&](uint8_t pid, const float acf[3]) {
        // Aircraft frame → this machine's world frame.
        const float wx = static_cast<float>(axd) + R[0]*acf[0] + R[1]*acf[1] + R[2]*acf[2];
        const float wy = static_cast<float>(ayd) + R[3]*acf[0] + R[4]*acf[1] + R[5]*acf[2];
        const float wz = static_cast<float>(azd) + R[6]*acf[0] + R[7]*acf[1] + R[8]*acf[2];

        const uint32_t c = notepad::colorForParticipant(pid);
        // Extract exactly like NotepadWindow renders strokes (R = LSB), so the
        // pointer color matches the participant's notepad ink on screen.
        const float cr = ((c >> 0)  & 0xFF) / 255.f;
        const float cg = ((c >> 8)  & 0xFF) / 255.f;
        const float cb = ((c >> 16) & 0xFF) / 255.f;

        constexpr int   kSegs   = 20;
        constexpr float kRadius = 0.012f;  // core disc, meters
        constexpr float kGlow   = 0.03f;   // soft halo, meters

        auto fan = [&](float radius, float alpha) {
            glColor4f(cr, cg, cb, alpha);
            glBegin(GL_TRIANGLE_FAN);
            glVertex3f(wx, wy, wz);
            for (int i = 0; i <= kSegs; ++i) {
                const float a  = 2.f * static_cast<float>(M_PI) * i / kSegs;
                const float ca = cosf(a) * radius, sa = sinf(a) * radius;
                glVertex3f(wx + camR[0]*ca + camU[0]*sa,
                           wy + camR[1]*ca + camU[1]*sa,
                           wz + camR[2]*ca + camU[2]*sa);
            }
            glEnd();
        };

        glBlendFunc(GL_SRC_ALPHA, GL_ONE);  // additive halo
        fan(kGlow, 0.25f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        fan(kRadius, 0.95f);
    };

    for (const auto& [pid, rm] : remotes_) {
        if (!rm.active) continue;
        if (std::chrono::duration<double>(now - rm.lastRecv).count() > STALE_S)
            continue;
        const float p[3] = {rm.x, rm.y, rm.z};
        drawDot(pid, p);
    }

    // Own pointer: immediate local feedback, no network round-trip.
    if (holding_ && haveLocalHit_)
        drawDot(session_->myId(), localHit_);

    glBlendFunc(static_cast<GLenum>(prevSrc), static_cast<GLenum>(prevDst));
}

void PointerSync::onDraw()
{
    if (!available_ || !session_) return;
    if (session_->myId() == INVALID_PARTICIPANT_ID) return;  // not in a session

    float mv[16], pj[16];
    int   vp[4];
    if (XPLMGetDatavf(drWorldMtx_, mv, 0, 16) != 16) return;
    if (XPLMGetDatavf(drProjMtx_,  pj, 0, 16) != 16) return;
    if (XPLMGetDatavi(drViewport_, vp, 0, 4)  != 4)  return;

    if (holding_) {
        haveLocalHit_ = computeLocalHit(mv, pj, vp);
        if (haveLocalHit_) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - lastSend_).count() >= SEND_PERIOD_S) {
                lastSend_ = now;
                sendState(true);
            }
        }
    }

    renderAll(mv);
}

}
