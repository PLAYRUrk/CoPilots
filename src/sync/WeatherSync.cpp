#include "WeatherSync.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <cstring>
#include <fstream>

namespace cp {

static float rdrf(const char* path)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    return dr ? XPLMGetDataf(dr) : 0.f;
}

static int rdri(const char* path)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    return dr ? XPLMGetDatai(dr) : 0;
}

static void rdrfa(const char* path, float* out, int count)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr) XPLMGetDatavf(dr, out, 0, count);
    else    memset(out, 0, count * sizeof(float));
}

static void rdrvia(const char* path, int* out, int count)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr) XPLMGetDatavi(dr, out, 0, count);
    else    memset(out, 0, count * sizeof(int));
}

static void wdrf(const char* path, float v)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDataf(dr, v);
}

static void wdri(const char* path, int v)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDatai(dr, v);
}

static void wdrfa(const char* path, const float* v, int count)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDatavf(dr, const_cast<float*>(v), 0, count);
}

static void wdrvia(const char* path, const int* v, int count)
{
    XPLMDataRef dr = XPLMFindDataRef(path);
    if (dr && XPLMCanWriteDataRef(dr)) XPLMSetDatavi(dr, const_cast<int*>(v), 0, count);
}

// XP11 legacy weather layers are NOT arrays: each layer is a separate scalar
// dataref whose registered name literally contains the index, e.g.
// "sim/weather/wind_speed_kt[0]".  Resolving the bare name either fails
// (clouds, turbulence, wind altitude) or resolves to a DIFFERENT read-only
// dataref (the "effective wind at the aircraft" scalars) — which is why the
// original array-based implementation silently synced nothing.
static float rdrfIdx(const char* base, int i)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s[%d]", base, i);
    return rdrf(buf);
}

static int rdriIdx(const char* base, int i)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s[%d]", base, i);
    return rdri(buf);
}

static void wdrfIdx(const char* base, int i, float v)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s[%d]", base, i);
    wdrf(buf, v);
}

static void wdriIdx(const char* base, int i, int v)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s[%d]", base, i);
    wdri(buf, v);
}

void WeatherSync::init(Session* session, net::NetThread* net, const std::string& xpSystemPath)
{
    session_   = session;
    net_       = net;
    metarPath_ = xpSystemPath + "METAR.rwx";
    // X-Plane 12: the legacy sim/weather/* datarefs still exist but are read-only;
    // real weather state lives in the writable sim/weather/region/* arrays.
    xp12_ = XPLMFindDataRef("sim/weather/region/wind_speed_msc") != nullptr;
    Log("WeatherSync: using %s weather datarefs", xp12_ ? "XP12 region" : "XP11 legacy");
    reset();
}

bool WeatherSync::masterUsesRealWeather() const
{
    if (xp12_) return false;  // XP12 path snapshots the region arrays directly
    XPLMDataRef dr = XPLMFindDataRef("sim/weather/use_real_weather_bool");
    return dr && XPLMGetDatai(dr) == 1;
}

static uint32_t fnv1aBuf(const std::vector<uint8_t>& data)
{
    uint32_t h = 2166136261u;
    for (uint8_t b : data) { h ^= b; h *= 16777619u; }
    return h;
}

void WeatherSync::checkAndSendMetar()
{
    std::ifstream f(metarPath_, std::ios::binary);
    if (!f.is_open()) return;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (data.empty() || data.size() > kMetarMaxSize) return;

    uint32_t h = fnv1aBuf(data);
    if (h == lastSentHash_) return;
    lastSentHash_ = h;

    uint32_t total  = static_cast<uint32_t>(data.size());
    uint16_t chunks = static_cast<uint16_t>((total + kMetarChunk - 1) / kMetarChunk);
    for (uint16_t i = 0; i < chunks; ++i) {
        uint32_t off = i * kMetarChunk;
        uint32_t len = (off + kMetarChunk <= total) ? kMetarChunk : (total - off);
        auto b = proto::MsgBuilder(proto::MsgType::WEATHER_METAR)
                     .u32(h).u32(total).u16(i).u16(chunks);
        b.bytes(data.data() + off, len);
        net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        net_->outTcp.push(std::move(out));
    }
    Log("WeatherSync: sent METAR.rwx (%u bytes, %u chunks, hash=0x%08X)",
        total, chunks, h);
}

void WeatherSync::onMetarChunk(proto::MsgReader& r)
{
    if (session_ && session_->isWeatherMaster()) return;

    uint32_t hash  = r.u32();
    uint32_t total = r.u32();
    uint16_t idx   = r.u16();
    uint16_t count = r.u16();
    std::vector<uint8_t> chunk = r.bytes();
    if (!r.ok() || total == 0 || total > kMetarMaxSize || count == 0) return;

    if (idx == 0) {
        rxHash_ = hash;
        rxNext_ = 0;
        rxBuf_.clear();
        rxBuf_.reserve(total);
    }
    if (hash != rxHash_ || idx != rxNext_) {
        // Out-of-order / mixed transfer — drop and wait for the next resend cycle.
        rxHash_ = 0;
        rxBuf_.clear();
        return;
    }
    rxBuf_.insert(rxBuf_.end(), chunk.begin(), chunk.end());
    ++rxNext_;

    if (rxNext_ != count) return;
    if (rxBuf_.size() != total) { rxBuf_.clear(); rxHash_ = 0; return; }

    std::ofstream out(metarPath_, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LogWarning("WeatherSync: cannot write %s", metarPath_.c_str());
        return;
    }
    out.write(reinterpret_cast<const char*>(rxBuf_.data()),
              static_cast<std::streamsize>(rxBuf_.size()));
    out.close();

    Log("WeatherSync: received METAR.rwx (%zu bytes) — switching to real-weather-from-file",
        rxBuf_.size());
    rxBuf_.clear();

    // Force X-Plane to re-read the file: real weather off now, back on in a few frames.
    metarMode_  = true;
    reenableWx_ = 5;
    wdri("sim/weather/use_real_weather_bool", 0);
}

void WeatherSync::reset()
{
    elapsed_      = 0.f;
    metarTimer_   = 0.f;
    lastSentHash_ = 0;
    rxHash_       = 0;
    rxNext_       = 0;
    rxBuf_.clear();
    metarMode_    = false;
    reenableWx_   = 0;
    if (suppressingRealWeather_) {
        XPLMDataRef dr = XPLMFindDataRef("sim/weather/use_real_weather_bool");
        if (dr && XPLMCanWriteDataRef(dr))
            XPLMSetDatai(dr, 1);
        suppressingRealWeather_ = false;
    }
}

void WeatherSync::tick(float dt)
{
    if (!session_ || !net_) return;

    if (!session_->isWeatherMaster()) {
        if (metarMode_) {
            // We mirror the master's METAR.rwx: run real-weather-from-file mode.
            // The brief off/on toggle after a file update forces X-Plane to re-read it.
            if (reenableWx_ > 0 && --reenableWx_ == 0)
                wdri("sim/weather/use_real_weather_bool", 1);
            return;
        }
        // XP11: pin real weather off every frame so X-Plane's real-weather updater
        // doesn't override values synced from the weather master.
        // XP12: not needed — writing the region arrays switches the sim to
        // manually-configured weather, which disables the live-weather updater.
        if (!xp12_) {
            XPLMDataRef dr = XPLMFindDataRef("sim/weather/use_real_weather_bool");
            if (dr && XPLMCanWriteDataRef(dr)) {
                XPLMSetDatai(dr, 0);
                suppressingRealWeather_ = true;
            }
        }
        return;
    }

    // Master with real weather from file (Active Sky & co): the legacy cloud/wind
    // datarefs do NOT describe the actual weather in this mode — snapshotting them
    // would broadcast "clear skies".  Ship METAR.rwx itself instead so every sim
    // computes identical weather from the identical source.
    if (masterUsesRealWeather()) {
        metarTimer_ += dt;
        if (metarTimer_ >= kMetarCheckS) {
            metarTimer_ = 0.f;
            checkAndSendMetar();
        }
        return;
    }

    elapsed_ += dt;
    if (elapsed_ < kSendInterval) return;
    elapsed_ = 0.f;

    sendState();
}

void WeatherSync::sendState()
{
    if (xp12_) sendStateRegion();
    else       sendStateLegacy();
}

// ── XP12: sim/weather/region/* ──────────────────────────────────────────────

void WeatherSync::sendStateRegion()
{
    float wAlt[kWindLayers12], wDir[kWindLayers12], wSpd[kWindLayers12], wTurb[kWindLayers12];
    float cType[kCloudLayers], cCover[kCloudLayers], cBase[kCloudLayers], cTops[kCloudLayers];

    rdrfa("sim/weather/region/wind_altitude_msl_m",  wAlt,  kWindLayers12);
    rdrfa("sim/weather/region/wind_direction_degt",  wDir,  kWindLayers12);
    rdrfa("sim/weather/region/wind_speed_msc",       wSpd,  kWindLayers12);
    rdrfa("sim/weather/region/turbulence",           wTurb, kWindLayers12);
    rdrfa("sim/weather/region/cloud_type",           cType, kCloudLayers);
    rdrfa("sim/weather/region/cloud_coverage_percent", cCover, kCloudLayers);
    rdrfa("sim/weather/region/cloud_base_msl_m",     cBase, kCloudLayers);
    rdrfa("sim/weather/region/cloud_tops_msl_m",     cTops, kCloudLayers);

    float visibility = rdrf("sim/weather/region/visibility_reported_sm");
    float rain       = rdrf("sim/weather/region/rain_percent");
    float sl_temp    = rdrf("sim/weather/region/sealevel_temperature_c");
    float sl_press   = rdrf("sim/weather/region/sealevel_pressure_pas");
    float zulu_sec   = rdrf("sim/time/zulu_time_sec");
    int   date_days  = rdri("sim/time/local_date_days");

    auto b = proto::MsgBuilder(proto::MsgType::WEATHER_STATE);
    b.u8(FMT_REGION);
    for (int i = 0; i < kWindLayers12; ++i) b.f32(wAlt[i]);
    for (int i = 0; i < kWindLayers12; ++i) b.f32(wDir[i]);
    for (int i = 0; i < kWindLayers12; ++i) b.f32(wSpd[i]);
    for (int i = 0; i < kWindLayers12; ++i) b.f32(wTurb[i]);
    for (int i = 0; i < kCloudLayers; ++i)  b.f32(cType[i]);
    for (int i = 0; i < kCloudLayers; ++i)  b.f32(cCover[i]);
    for (int i = 0; i < kCloudLayers; ++i)  b.f32(cBase[i]);
    for (int i = 0; i < kCloudLayers; ++i)  b.f32(cTops[i]);
    b.f32(visibility).f32(rain).f32(sl_temp).f32(sl_press);
    b.f32(zulu_sec).u32(static_cast<uint32_t>(date_days));

    net::OutboundMsg out;
    out.target = 0xFF;
    out.frame  = b.build();
    net_->outTcp.push(std::move(out));
}

void WeatherSync::applyStateRegion(proto::MsgReader& r)
{
    float wAlt[kWindLayers12], wDir[kWindLayers12], wSpd[kWindLayers12], wTurb[kWindLayers12];
    float cType[kCloudLayers], cCover[kCloudLayers], cBase[kCloudLayers], cTops[kCloudLayers];

    for (int i = 0; i < kWindLayers12; ++i) wAlt[i]  = r.f32();
    for (int i = 0; i < kWindLayers12; ++i) wDir[i]  = r.f32();
    for (int i = 0; i < kWindLayers12; ++i) wSpd[i]  = r.f32();
    for (int i = 0; i < kWindLayers12; ++i) wTurb[i] = r.f32();
    for (int i = 0; i < kCloudLayers; ++i)  cType[i]  = r.f32();
    for (int i = 0; i < kCloudLayers; ++i)  cCover[i] = r.f32();
    for (int i = 0; i < kCloudLayers; ++i)  cBase[i]  = r.f32();
    for (int i = 0; i < kCloudLayers; ++i)  cTops[i]  = r.f32();

    float visibility = r.f32();
    float rain       = r.f32();
    float sl_temp    = r.f32();
    float sl_press   = r.f32();
    float zulu_sec   = r.f32();
    int   date_days  = static_cast<int>(r.u32());

    if (!r.ok()) return;

    if (!xp12_) {
        // Receiver runs XP11 while the master runs XP12 — mixed sims can't fly the
        // same modern aircraft anyway; log once and skip rather than corrupt state.
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogWarning("WeatherSync: received XP12 weather but running XP11 — ignored");
        }
        return;
    }

    wdrfa("sim/weather/region/wind_altitude_msl_m",  wAlt,  kWindLayers12);
    wdrfa("sim/weather/region/wind_direction_degt",  wDir,  kWindLayers12);
    wdrfa("sim/weather/region/wind_speed_msc",       wSpd,  kWindLayers12);
    wdrfa("sim/weather/region/turbulence",           wTurb, kWindLayers12);
    wdrfa("sim/weather/region/cloud_type",           cType, kCloudLayers);
    wdrfa("sim/weather/region/cloud_coverage_percent", cCover, kCloudLayers);
    wdrfa("sim/weather/region/cloud_base_msl_m",     cBase, kCloudLayers);
    wdrfa("sim/weather/region/cloud_tops_msl_m",     cTops, kCloudLayers);
    wdrf("sim/weather/region/visibility_reported_sm", visibility);
    wdrf("sim/weather/region/rain_percent",           rain);
    wdrf("sim/weather/region/sealevel_temperature_c", sl_temp);
    wdrf("sim/weather/region/sealevel_pressure_pas",  sl_press);
    // Commit the block atomically — without this XP12 keeps interpolating toward
    // the new values over ~a minute instead of applying them.
    wdri("sim/weather/region/update_immediately", 1);

    wdrf("sim/time/zulu_time_sec",   zulu_sec);
    wdri("sim/time/local_date_days", date_days);
}

// ── XP11: legacy sim/weather/* ──────────────────────────────────────────────

void WeatherSync::sendStateLegacy()
{
    float wind_speed[3], wind_dir[3], wind_alt[3], turbulence[3];
    float cloud_base[3], cloud_tops[3];
    int   cloud_type[3];

    for (int i = 0; i < 3; ++i) {
        wind_speed[i] = rdrfIdx("sim/weather/wind_speed_kt",       i);
        wind_dir[i]   = rdrfIdx("sim/weather/wind_direction_degt",  i);
        wind_alt[i]   = rdrfIdx("sim/weather/wind_altitude_msl_m",  i);
        turbulence[i] = rdrfIdx("sim/weather/turbulence",           i);
        cloud_type[i] = rdriIdx("sim/weather/cloud_type",           i);
        cloud_base[i] = rdrfIdx("sim/weather/cloud_base_msl_m",     i);
        cloud_tops[i] = rdrfIdx("sim/weather/cloud_tops_msl_m",     i);
    }

    // NOTE: the wire field carries METERS ("visibility_reported_sm" does not
    // exist in XP11 — the real dataref is visibility_reported_m).  The dew point
    // dataref name really is "dewpoi_sealevel_c" (an X-Plane typo kept for
    // compatibility); "dewpoint_sealevel_c" resolves to nothing.
    float visibility  = rdrf("sim/weather/visibility_reported_m");
    float rain        = rdrf("sim/weather/rain_percent");
    float temperature = rdrf("sim/weather/temperature_sealevel_c");
    float dewpoint    = rdrf("sim/weather/dewpoi_sealevel_c");
    float barometer   = rdrf("sim/weather/barometer_sealevel_inhg");
    float zulu_sec    = rdrf("sim/time/zulu_time_sec");
    int   date_days   = rdri("sim/time/local_date_days");

    auto b = proto::MsgBuilder(proto::MsgType::WEATHER_STATE);
    b.u8(FMT_LEGACY);
    for (int i = 0; i < 3; ++i) b.f32(wind_speed[i]);
    for (int i = 0; i < 3; ++i) b.f32(wind_dir[i]);
    for (int i = 0; i < 3; ++i) b.f32(wind_alt[i]);
    for (int i = 0; i < 3; ++i) b.f32(turbulence[i]);
    for (int i = 0; i < 3; ++i) b.u32(static_cast<uint32_t>(cloud_type[i]));
    for (int i = 0; i < 3; ++i) b.f32(cloud_base[i]);
    for (int i = 0; i < 3; ++i) b.f32(cloud_tops[i]);
    b.f32(visibility).f32(rain).f32(temperature).f32(dewpoint).f32(barometer);
    b.f32(zulu_sec).u32(static_cast<uint32_t>(date_days));

    net::OutboundMsg out;
    out.target = 0xFF;
    out.frame  = b.build();
    net_->outTcp.push(std::move(out));
}

void WeatherSync::onTcpMessage(const uint8_t* payload, size_t len)
{
    if (session_ && session_->isWeatherMaster()) return;

    proto::MsgReader r(payload, len);
    applyState(r);
}

void WeatherSync::applyState(proto::MsgReader& r)
{
    uint8_t fmt = r.u8();
    if (fmt == FMT_REGION) { applyStateRegion(r); return; }
    applyStateLegacy(r);
}

void WeatherSync::applyStateLegacy(proto::MsgReader& r)
{
    float wind_speed[3], wind_dir[3], wind_alt[3], turbulence[3];
    float cloud_base[3], cloud_tops[3];
    int   cloud_type[3];

    for (int i = 0; i < 3; ++i) wind_speed[i]  = r.f32();
    for (int i = 0; i < 3; ++i) wind_dir[i]    = r.f32();
    for (int i = 0; i < 3; ++i) wind_alt[i]    = r.f32();
    for (int i = 0; i < 3; ++i) turbulence[i]  = r.f32();
    for (int i = 0; i < 3; ++i) cloud_type[i]  = static_cast<int>(r.u32());
    for (int i = 0; i < 3; ++i) cloud_base[i]  = r.f32();
    for (int i = 0; i < 3; ++i) cloud_tops[i]  = r.f32();

    float visibility  = r.f32();
    float rain        = r.f32();
    float temperature = r.f32();
    float dewpoint    = r.f32();
    float barometer   = r.f32();
    float zulu_sec    = r.f32();
    int   date_days   = static_cast<int>(r.u32());

    if (!r.ok()) return;

    for (int i = 0; i < 3; ++i) {
        wdrfIdx("sim/weather/wind_speed_kt",       i, wind_speed[i]);
        wdrfIdx("sim/weather/wind_direction_degt",  i, wind_dir[i]);
        wdrfIdx("sim/weather/wind_altitude_msl_m",  i, wind_alt[i]);
        wdrfIdx("sim/weather/turbulence",           i, turbulence[i]);
        wdriIdx("sim/weather/cloud_type",           i, cloud_type[i]);
        wdrfIdx("sim/weather/cloud_base_msl_m",     i, cloud_base[i]);
        wdrfIdx("sim/weather/cloud_tops_msl_m",     i, cloud_tops[i]);
    }
    wdrf("sim/weather/visibility_reported_m",  visibility);  // meters, see sender
    wdrf("sim/weather/rain_percent",           rain);
    wdrf("sim/weather/temperature_sealevel_c", temperature);
    wdrf("sim/weather/dewpoi_sealevel_c",      dewpoint);    // XP typo name
    wdrf("sim/weather/barometer_sealevel_inhg",barometer);
    wdrf("sim/time/zulu_time_sec",             zulu_sec);
    wdri("sim/time/local_date_days",           date_days);
}

}
