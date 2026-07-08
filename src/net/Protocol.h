#pragma once
// Protocol.h — binary wire protocol for CoPilots.
//
// TCP framing:  [uint8_t type][uint32_t payload_len][payload...]
// UDP framing:  [uint8_t type][payload...]  (fixed-size per type, no length field)

#include <cstdint>
#include <vector>
#include <string>

namespace cp {
namespace proto {

constexpr uint8_t PROTOCOL_VERSION = 1;
constexpr size_t  TCP_HEADER_SIZE  = 5; // type(1) + length(4)

// ── TCP message types ──────────────────────────────────────────────────────
enum class MsgType : uint8_t {
    // Handshake
    HELLO             = 0x01,  // client→server: {version, nick}
    WELCOME           = 0x02,  // server→client: {assigned_id, full state}
    REJECT            = 0x03,  // server→client: {reason string}

    // Participant management
    PARTICIPANT_JOIN   = 0x10,  // broadcast: new participant
    PARTICIPANT_LEAVE  = 0x11,  // broadcast: participant disconnected
    PARTICIPANT_UPDATE = 0x12,  // broadcast: nick/role/zones changed

    // Admin operations (server only sends to all; clients send to server)
    ROLE_ASSIGN        = 0x20,  // admin→server: {target_id, role_id}
    ZONE_ASSIGN        = 0x21,  // admin→server: {target_id, zone_ids[]}
    KICK               = 0x22,  // admin→server: {target_id}
    PHYSICS_MASTER_SET = 0x23,  // admin→server: {target_id}

    // Authority map — full zone→owner map, sent after any assignment change
    AUTHORITY_MAP      = 0x30,  // server→all: {zone_id→participant_id map}

    // Dataref / command sync
    DATAREF_SET        = 0x40,  // any→server→all: {dr_index, type, value}
    COMMAND_FIRE       = 0x41,  // any→server→all: {cmd_index} (matches plugin.cpp usage)

    // Infrastructure
    HEARTBEAT          = 0xF0,  // bidirectional, no payload
    CHAT               = 0xF1,  // optional: {participant_id, message}
};

// ── UDP message types ──────────────────────────────────────────────────────
enum class UdpType : uint8_t {
    PHYSICS_STATE  = 0x01,  // physics master→all: position/attitude/velocities
    CONTROL_INPUT  = 0x02,  // control zone owner→physics master: axes
    PING           = 0x03,  // client→server: {seq, timestamp_ms}
    PONG           = 0x04,  // server→client: {seq, timestamp_ms}
};

// ── Value types for DATAREF_SET ───────────────────────────────────────────
enum class ValType : uint8_t {
    INT       = 0,
    FLOAT     = 1,
    DOUBLE    = 2,
    INT_ARR   = 3,
    FLOAT_ARR = 4,
    BYTES     = 5,
};

// ── Structures ────────────────────────────────────────────────────────────

// Physics state — UDP packet (~128 bytes)
#pragma pack(push, 1)
struct PhysicsState {
    uint8_t  type = static_cast<uint8_t>(UdpType::PHYSICS_STATE);
    uint32_t seq;
    double   lat, lon, alt;    // degrees, degrees, metres MSL
    float    pitch, roll, hdg; // degrees
    float    vx, vy, vz;       // m/s ECEF-local
    float    p, q, r;          // rad/s roll,pitch,yaw rates
    float    flap_ratio;       // 0..1
    float    gear_ratio;       // 0..1
    float    throttle[8];      // 0..1 per engine
    float    aileron;          // -1..1
    float    elevator;         // -1..1
    float    rudder;           // -1..1
    float    speedbrake;       // 0..1
};
static_assert(sizeof(PhysicsState) < 512, "PhysicsState too large for single UDP");

struct ControlInput {
    uint8_t  type = static_cast<uint8_t>(UdpType::CONTROL_INPUT);
    uint8_t  participant_id;
    float    roll_axis;     // -1..1
    float    pitch_axis;    // -1..1
    float    yaw_axis;      // -1..1
    float    throttle[8];   // 0..1
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

// ── Builder helpers ────────────────────────────────────────────────────────

class MsgBuilder {
public:
    explicit MsgBuilder(MsgType t) {
        buf_.push_back(static_cast<uint8_t>(t));
        buf_.resize(TCP_HEADER_SIZE); // reserve space for length
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

    // Finalise: write payload length at offset 1
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

// ── Reader helper ─────────────────────────────────────────────────────────
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

} // namespace proto
} // namespace cp
