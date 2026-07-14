#pragma once

#include <cstdint>
#include <vector>
#include <string>

namespace cp {
namespace proto {

constexpr uint8_t PROTOCOL_VERSION = 3;
constexpr size_t  TCP_HEADER_SIZE  = 5;

enum class MsgType : uint8_t {
    HELLO             = 0x01,
    WELCOME           = 0x02,
    REJECT            = 0x03,

    PARTICIPANT_JOIN   = 0x10,
    PARTICIPANT_LEAVE  = 0x11,
    PARTICIPANT_UPDATE = 0x12,

    ROLE_ASSIGN        = 0x20,
    ZONE_ASSIGN        = 0x21,
    KICK               = 0x22,
    PHYSICS_MASTER_SET = 0x23,
    WEATHER_MASTER_SET = 0x24,
    CONTROL_REQUEST    = 0x25,  // client → host: request to become physics master
    CONTROL_DENY       = 0x26,  // host → client: control request denied

    AUTHORITY_MAP      = 0x30,

    DATAREF_SET        = 0x40,
    COMMAND_FIRE       = 0x41,
    WEATHER_STATE      = 0x42,
    RESYNC_REQUEST     = 0x43,  // client → host: SASL side-effects detected, request full resync

    // Notepad / whiteboard messages (0x44–0x4B)
    NP_TAB_SHARE       = 0x44,  // any → host → all:  announce that a private tab is now shared
    NP_SHEET_NEW       = 0x45,  // any → host → all:  create a new sheet in a shared tab
    NP_STROKE_ADD      = 0x46,  // any → host → all:  append one completed stroke to a sheet
    NP_SHEET_DEL       = 0x47,  // owner → host → all: delete a sheet (owner only)
    NP_SHEET_PARAM     = 0x48,  // owner → host → all: resize / rename a sheet (owner only)
    NP_SNAP_REQ        = 0x49,  // client → host:      request full shared-notepad snapshot
    NP_SNAP_SHEET      = 0x4A,  // host → client:      one sheet worth of strokes (may be chunked)
    NP_SNAP_END        = 0x4B,  // host → client:      snapshot complete
    NP_TAB_DEL         = 0x4C,  // owner → host → all: delete a shared tab (owner only)
    NP_STROKE_DEL      = 0x4D,  // any → host → all:  delete one stroke by ID (smart eraser)

    FMS_LIST           = 0x50,  // any → host → all: inventory of Output/FMS plans {name,size,hash}
    FMS_REQUEST        = 0x51,  // any → host → all: request files missing locally (by name)
    FMS_FILE           = 0x52,  // any → host → all: one flight-plan file (name + raw bytes)

    HEARTBEAT          = 0xF0,
    CHAT               = 0xF1,
};

enum class UdpType : uint8_t {
    PHYSICS_STATE  = 0x01,
    CONTROL_INPUT  = 0x02,
    PING           = 0x03,
    PONG           = 0x04,
    ANNOUNCE       = 0x05,  // client → server: learn my UDP endpoint
};

enum class ValType : uint8_t {
    INT       = 0,
    FLOAT     = 1,
    DOUBLE    = 2,
    INT_ARR   = 3,
    FLOAT_ARR = 4,
    BYTES     = 5,
};

#pragma pack(push, 1)
struct PhysicsState {
    uint8_t  type      = static_cast<uint8_t>(UdpType::PHYSICS_STATE);
    uint32_t seq;
    uint8_t  sender_id = 0xFF;   // participant ID of the current physics master
    double   t_send    = 0.0;    // master's sim/time/total_running_time_sec at send time
    double   lat, lon, alt;
    float    pitch, roll, hdg;
    float    vx, vy, vz;
    float    p, q, r;
    float    flap_ratio;
    float    gear_ratio;
    float    throttle[8];
    float    reverser_ratio[8];  // thrust reverser deployment (0=stowed, 1=full)
    float    aileron;
    float    elevator;
    float    rudder;
    float    speedbrake;
    float    left_brake;         // left toe brake ratio
    float    right_brake;        // right toe brake ratio
    float    prop_ratio[8];      // prop pitch ratio (turboprops/pistons)
    float    engine_N2[8];      // real ENGN_N2_ RPM % — written to bypass SASL engine overrides
    float    engine_N1[8];      // real ENGN_N1_ RPM % — same rationale
    uint8_t  engine_running[8]; // sim/flightmodel/engine/ENGN_running (0/1 per engine)

    // Derived aerodynamic state — sent by the physics master so that clients' instruments
    // (AUASP, AoA indicators, accelerometers) display values consistent with the master
    // rather than values derived from the clients' own locally-computed kinematics.
    // Written to clients via wflt() in applyState() after the flight model runs.
    float    g_nrml = 0.f;   // sim/flightmodel2/misc/gforce_normal — normal load factor (G)
    float    g_axil = 0.f;   // sim/flightmodel2/misc/gforce_axil  — axial load factor
    float    g_side = 0.f;   // sim/flightmodel2/misc/gforce_side  — side load factor
    float    alpha  = 0.f;   // sim/flightmodel/position/alpha     — angle of attack, deg
    float    beta   = 0.f;   // sim/flightmodel/position/beta      — sideslip angle, deg

    // Fuel per tank (kg) — sim/flightmodel/weight/m_fuel.  Clients burn fuel in
    // their own local simulation at a slightly different rate than the master;
    // over a flight the divergence changes aircraft weight and therefore engine
    // parameters and trim.  Streaming the master's tank quantities keeps every
    // participant's weight (and everything derived from it) converged.
    float    fuel_kg[9] = {};
};
static_assert(sizeof(PhysicsState) < 512, "PhysicsState too large for single UDP");

struct ControlInput {
    uint8_t  type = static_cast<uint8_t>(UdpType::CONTROL_INPUT);
    uint8_t  participant_id;
    float    roll_axis;
    float    pitch_axis;
    float    yaw_axis;
    float    throttle[8];
};

struct UdpPing {
    uint8_t  type = static_cast<uint8_t>(UdpType::PING);
    uint16_t seq;
    uint64_t timestamp_ms;
};

struct UdpPong {
    uint8_t  type = static_cast<uint8_t>(UdpType::PONG);
    uint16_t seq;
    uint64_t timestamp_ms;
};
#pragma pack(pop)

class MsgBuilder {
public:
    explicit MsgBuilder(MsgType t) {
        buf_.push_back(static_cast<uint8_t>(t));
        buf_.resize(TCP_HEADER_SIZE);
    }

    MsgBuilder& u8(uint8_t v)  { buf_.push_back(v); return *this; }
    MsgBuilder& u16(uint16_t v) {
        buf_.push_back(v & 0xFF); buf_.push_back((v>>8)&0xFF); return *this;
    }
    MsgBuilder& u32(uint32_t v) {
        for (int i=0;i<4;i++) { buf_.push_back(v & 0xFF); v>>=8; }
        return *this;
    }
    MsgBuilder& f32(float v) {
        uint32_t tmp; memcpy(&tmp,&v,4); return u32(tmp);
    }
    MsgBuilder& f64(double v) {
        uint64_t tmp; memcpy(&tmp,&v,8);
        for (int i=0;i<8;i++) { buf_.push_back(tmp&0xFF); tmp>>=8; }
        return *this;
    }
    MsgBuilder& str(const std::string& s) {
        u16(static_cast<uint16_t>(s.size()));
        buf_.insert(buf_.end(), s.begin(), s.end());
        return *this;
    }
    MsgBuilder& bytes(const uint8_t* data, uint32_t len) {
        u32(len);
        buf_.insert(buf_.end(), data, data+len);
        return *this;
    }

    std::vector<uint8_t> build() {
        uint32_t plen = static_cast<uint32_t>(buf_.size()) - TCP_HEADER_SIZE;
        buf_[1] = plen & 0xFF;
        buf_[2] = (plen >> 8) & 0xFF;
        buf_[3] = (plen >> 16) & 0xFF;
        buf_[4] = (plen >> 24) & 0xFF;
        return buf_;
    }

private:
    std::vector<uint8_t> buf_;
};

class MsgReader {
public:
    MsgReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), pos_(0) {}

    bool ok() const { return pos_ <= len_; }
    bool empty() const { return pos_ >= len_; }

    uint8_t  u8()  { return (pos_<len_) ? data_[pos_++] : 0; }
    uint16_t u16() { uint16_t v=u8(); v|=((uint16_t)u8())<<8; return v; }
    uint32_t u32() { uint32_t v=0; for(int i=0;i<4;i++) v|=((uint32_t)u8())<<(8*i); return v; }
    float    f32() { uint32_t v=u32(); float r; memcpy(&r,&v,4); return r; }
    double   f64() { uint64_t v=0; for(int i=0;i<8;i++) v|=((uint64_t)u8())<<(8*i); double r; memcpy(&r,&v,8); return r; }
    std::string str() {
        uint16_t len = u16();
        if (pos_+len > len_) { pos_=len_+1; return {}; }
        std::string s(reinterpret_cast<const char*>(data_+pos_), len);
        pos_ += len;
        return s;
    }
    std::vector<uint8_t> bytes() {
        uint32_t len = u32();
        if (pos_+len > len_) { pos_=len_+1; return {}; }
        std::vector<uint8_t> v(data_+pos_, data_+pos_+len);
        pos_ += len;
        return v;
    }

private:
    const uint8_t* data_;
    size_t len_, pos_;
};

}
}
