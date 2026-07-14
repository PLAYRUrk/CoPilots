#pragma once

#include "../net/Protocol.h"
#include "../net/NetThread.h"
#include "../session/Session.h"
#include <cstdint>

namespace cp {

class WeatherSync {
public:
    void init(Session* session, net::NetThread* net, const std::string& xpSystemPath);
    void tick(float dt);
    void onTcpMessage(const uint8_t* payload, size_t len);
    // WEATHER_METAR chunk (METAR.rwx file transfer for Active Sky-style setups).
    void onMetarChunk(proto::MsgReader& r);
    void reset();

private:
    Session*        session_   = nullptr;
    net::NetThread* net_       = nullptr;
    float           elapsed_                = 0.f;
    bool            suppressingRealWeather_ = false;
    // True when running under X-Plane 12: the legacy sim/weather/* datarefs are
    // read-only there — weather lives in the sim/weather/region/* arrays instead.
    bool            xp12_                   = false;

    // Wire format identifiers (first payload byte of WEATHER_STATE).
    static constexpr uint8_t FMT_LEGACY = 1;  // XP11 sim/weather/*
    static constexpr uint8_t FMT_REGION = 2;  // XP12 sim/weather/region/*

    static constexpr float kSendInterval = 5.f;
    static constexpr int   kWindLayers12 = 13;
    static constexpr int   kCloudLayers  = 3;

    // ── METAR.rwx file sync (Active Sky and friends) ────────────────────────
    // When the weather master flies with real weather (use_real_weather_bool=1,
    // e.g. Active Sky writing METAR.rwx), the legacy cloud/wind datarefs do NOT
    // describe the actual weather — snapshotting them sends "clear skies".
    // Instead the master ships METAR.rwx itself; clients save it and enable
    // real-weather-from-file, so every sim computes weather from the same source.
    static constexpr float    kMetarCheckS  = 20.f;
    static constexpr uint32_t kMetarMaxSize = 8u * 1024u * 1024u;
    static constexpr uint32_t kMetarChunk   = 48u * 1024u;

    std::string metarPath_;          // <X-Plane>/METAR.rwx
    float       metarTimer_    = 0.f;
    uint32_t    lastSentHash_  = 0;
    // Client-side reassembly
    uint32_t             rxHash_  = 0;
    uint16_t             rxNext_  = 0;
    std::vector<uint8_t> rxBuf_;
    // Client: we received a METAR file this session → run in real-weather mode
    // instead of pinning it off.
    bool  metarMode_    = false;
    int   reenableWx_   = 0;     // frames until we flip use_real_weather back to 1

    bool masterUsesRealWeather() const;
    void checkAndSendMetar();

    void sendState();
    void sendStateLegacy();
    void sendStateRegion();
    void applyState(proto::MsgReader& r);
    void applyStateLegacy(proto::MsgReader& r);
    void applyStateRegion(proto::MsgReader& r);
};

}
