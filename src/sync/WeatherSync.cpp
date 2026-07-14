#include "WeatherSync.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <cstring>

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

void WeatherSync::init(Session* session, net::NetThread* net)
{
    session_ = session;
    net_     = net;
    reset();
}

void WeatherSync::reset()
{
    elapsed_ = 0.f;
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
        // Pin real weather off every frame so X-Plane's real-weather updater
        // doesn't override values synced from the weather master.
        // Mirrors how PhysicsSync pins override_joystick=0 on the physics master.
        XPLMDataRef dr = XPLMFindDataRef("sim/weather/use_real_weather_bool");
        if (dr && XPLMCanWriteDataRef(dr)) {
            XPLMSetDatai(dr, 0);
            suppressingRealWeather_ = true;
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
    float wind_speed[3], wind_dir[3], wind_alt[3], turbulence[3];
    float cloud_base[3], cloud_tops[3];
    int   cloud_type[3];

    rdrfa("sim/weather/wind_speed_kt",        wind_speed,  3);
    rdrfa("sim/weather/wind_direction_degt",   wind_dir,    3);
    rdrfa("sim/weather/wind_altitude_msl_m",   wind_alt,    3);
    rdrfa("sim/weather/turbulence",            turbulence,  3);
    rdrvia("sim/weather/cloud_type",           cloud_type,  3);
    rdrfa("sim/weather/cloud_base_msl_m",      cloud_base,  3);
    rdrfa("sim/weather/cloud_tops_msl_m",      cloud_tops,  3);

    float visibility  = rdrf("sim/weather/visibility_reported_sm");
    float rain        = rdrf("sim/weather/rain_percent");
    float temperature = rdrf("sim/weather/temperature_sealevel_c");
    float dewpoint    = rdrf("sim/weather/dewpoint_sealevel_c");
    float barometer   = rdrf("sim/weather/barometer_sealevel_inhg");
    float zulu_sec    = rdrf("sim/time/zulu_time_sec");
    int   date_days   = rdri("sim/time/local_date_days");

    auto b = proto::MsgBuilder(proto::MsgType::WEATHER_STATE);
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

    wdrfa("sim/weather/wind_speed_kt",       wind_speed, 3);
    wdrfa("sim/weather/wind_direction_degt",  wind_dir,   3);
    wdrfa("sim/weather/wind_altitude_msl_m",  wind_alt,   3);
    wdrfa("sim/weather/turbulence",           turbulence, 3);
    wdrvia("sim/weather/cloud_type",          cloud_type, 3);
    wdrfa("sim/weather/cloud_base_msl_m",     cloud_base, 3);
    wdrfa("sim/weather/cloud_tops_msl_m",     cloud_tops, 3);
    wdrf("sim/weather/visibility_reported_sm", visibility);
    wdrf("sim/weather/rain_percent",           rain);
    wdrf("sim/weather/temperature_sealevel_c", temperature);
    wdrf("sim/weather/dewpoint_sealevel_c",    dewpoint);
    wdrf("sim/weather/barometer_sealevel_inhg",barometer);
    wdrf("sim/time/zulu_time_sec",             zulu_sec);
    wdri("sim/time/local_date_days",           date_days);
}

}
