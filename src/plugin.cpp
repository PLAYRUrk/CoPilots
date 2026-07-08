#include "Log.h"
#include "config/Config.h"
#include "sync/DatarefRegistry.h"
#include "sync/SyncEngine.h"
#include "sync/PhysicsSync.h"
#include "sync/WeatherSync.h"
#include "session/Session.h"
#include "net/NetThread.h"
#include "net/Protocol.h"
#include "net/Transport.h"
#include "ui/ConnectionWindow.h"
#include "ui/StatusHud.h"

#include <XPLM/XPLMPlugin.h>
#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMProcessing.h>
#include <XPLM/XPLMUtilities.h>
#include <XPLM/XPLMMenus.h>
#include <XPLM/XPLMPlanes.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

using namespace cp;

struct CoPilotsPlugin {

    cp::Config              config;
    cp::DatarefRegistry     registry;
    cp::Session             session;
    cp::net::NetThread      netThread;
    cp::SyncEngine          syncEngine;
    cp::PhysicsSync         physicsSync;
    cp::WeatherSync         weatherSync;

    cp::ui::ConnectionWindow connWin;
    cp::ui::StatusHud        statusHud;

    // Maps TCP connection ID (from network thread) → participant ID (from session)
    std::map<uint8_t, cp::ParticipantId> connIdMap_;
    // Maps TCP connection ID → remote IP (received in 0xFD message)
    std::map<uint8_t, std::string> connRemoteIp_;

    XPLMMenuID menuId    = nullptr;
    int        menuItem  = -1;

    XPLMFlightLoopID flLoop = nullptr;

    bool active = false;

    void onEnable()
    {
        Log("Plugin enabled");
        cp::net::InitNetwork();

        session.onChanged = [this]() { };

        connWin.onHost = [this](const cp::ui::ConnectionConfig& cfg) {
            onHost(cfg);
        };
        connWin.onJoin = [this](const cp::ui::ConnectionConfig& cfg) {
            onJoin(cfg);
        };
        connWin.onDisconnect = [this]() { onDisconnect(); };

        connWin.onStopHosting = [this]() { onDisconnect(); };
        connWin.onRoleAssign = [this](cp::ParticipantId pid, const std::string& roleId) {
            session.setRole(pid, roleId, config.get().roles);
            broadcastAuthorityMap();
        };
        connWin.onZoneAssign = [this](cp::ParticipantId pid,
                                      const std::vector<std::string>& zones) {
            session.assignZones(pid, zones);
            broadcastAuthorityMap();
        };
        connWin.onKick = [this](cp::ParticipantId pid) {
            auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::KICK)
                         .u8(pid).build();
            cp::net::OutboundMsg out;
            out.target = 0xFF;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
            session.removeParticipant(pid);
        };
        connWin.onPhysicsMasterSet = [this](cp::ParticipantId pid) {
            session.setPhysicsMaster(pid);
            auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::PHYSICS_MASTER_SET)
                         .u8(pid).build();
            cp::net::OutboundMsg out;
            out.target = 0xFF;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
        };
        connWin.onWeatherMasterSet = [this](cp::ParticipantId pid) {
            session.setWeatherMaster(pid);
            auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::WEATHER_MASTER_SET)
                         .u8(pid).build();
            cp::net::OutboundMsg out;
            out.target = 0xFF;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
        };

        connWin.init();
        statusHud.init();
        statusHud.setSession(&session);
        connWin.setData(&session, &config.get());
        weatherSync.init(&session, &netThread);

        XPLMCreateFlightLoop_t params{};
        params.structSize = sizeof(params);
        params.phase      = xplm_FlightLoop_Phase_AfterFlightModel;
        params.callbackFunc = [](float , float ,
                                 int , void* ref) -> float {
            static_cast<CoPilotsPlugin*>(ref)->onFlightLoop();
            return -1.f;
        };
        params.refcon = this;
        flLoop = XPLMCreateFlightLoop(&params);
        XPLMScheduleFlightLoop(flLoop, -1.f, 1);

        active = true;
        Log("CoPilots ready — open Plugins > CoPilots to connect");
    }

    void onDisablePlugin()
    {
        if (!active) return;
        active = false;

        onDisconnect();

        XPLMDestroyFlightLoop(flLoop);
        flLoop = nullptr;

        connWin.shutdown();
        statusHud.shutdown();
        cp::net::ShutdownNetwork();
        Log("Plugin disabled");
    }

    void onFlightLoop()
    {
        if (!active) return;

        cp::net::InboundMsg msg;
        while (netThread.inTcp.pop(msg)) {
            handleTcpMessage(msg);
        }

        cp::net::UdpDatagram dg;
        while (netThread.inUdp.pop(dg)) {
            if (dg.data.empty()) continue;
            uint8_t t = dg.data[0];
            if (t == static_cast<uint8_t>(cp::proto::UdpType::PHYSICS_STATE)) {
                physicsSync.onUdpDatagram(dg.data.data(), dg.data.size());
                if (session.isHost()) {
                    // Relay physics state from non-host physics master to all clients
                    cp::net::UdpDatagram relay;
                    relay.data = dg.data;
                    // empty `to` triggers broadcast in serverLoop
                    netThread.outUdp.push(std::move(relay));
                }
            } else if (t == static_cast<uint8_t>(cp::proto::UdpType::ANNOUNCE)
                       && dg.data.size() >= 2 && session.isHost()) {
                // Client announcing its UDP endpoint so we can send physics to it
                uint8_t participantId = dg.data[1];
                for (auto& [cid, pid] : connIdMap_) {
                    if (pid == participantId) {
                        netThread.clientUdpEpUpdates.push({cid, dg.from});
                        Log("CoPilots: learned UDP ep for participant %u via ANNOUNCE", participantId);
                        break;
                    }
                }
            }
        }

        if (registry.datarefs().size() > 0 && netThread.connected) {
            syncEngine.tick(
                [this](uint16_t idx, const cp::DrValue& val) {
                    sendDatarefSet(idx, val);
                },
                [this](uint16_t idx) {
                    sendCommandFire(idx);
                }
            );
        }

        if (netThread.connected)
            physicsSync.tick();

        if (netThread.connected)
            weatherSync.tick(1.f / 60.f);

        if (netThread.hasError) {
            connWin.setState(cp::ui::ConnState::CONNECT_ERROR, netThread.lastError);
            netThread.hasError = false;
            session.clear();
        }
    }

    void handleTcpMessage(const cp::net::InboundMsg& msg)
    {
        using MT = cp::proto::MsgType;
        auto type = static_cast<MT>(msg.type);
        cp::proto::MsgReader r(msg.payload.data(), msg.payload.size());

        switch (type) {
        case static_cast<MT>(0xFD): {
            // Network thread: new TCP client connected — payload is remote IP string
            std::string ip(msg.payload.begin(), msg.payload.end());
            connRemoteIp_[msg.sender] = ip;
            break;
        }

        case static_cast<MT>(0xFE): {
            // Network thread: TCP client disconnected
            auto it = connIdMap_.find(msg.sender);
            if (it != connIdMap_.end()) {
                cp::ParticipantId pid = it->second;
                session.removeParticipant(pid);
                auto frame = cp::proto::MsgBuilder(MT::PARTICIPANT_LEAVE).u8(pid).build();
                cp::net::OutboundMsg leaveOut;
                leaveOut.target = 0xFF;
                leaveOut.frame  = std::move(frame);
                netThread.outTcp.push(std::move(leaveOut));
                connIdMap_.erase(it);
            }
            connRemoteIp_.erase(msg.sender);
            break;
        }

        case MT::HELLO: {
            uint8_t ver = r.u8();
            std::string nick = r.str();
            (void)ver;
            cp::ParticipantId pid = session.addParticipant(nick);

            auto authMap = session.buildAuthorityMap();
            auto wb = cp::proto::MsgBuilder(MT::WELCOME).u8(pid);
            wb.u16(static_cast<uint16_t>(session.participants().size()));
            for (const auto& p : session.participants()) {
                wb.u8(p.id).str(p.nick).str(p.roleId);
                wb.u8(static_cast<uint8_t>(p.zoneIds.size()));
                for (const auto& z : p.zoneIds) wb.str(z);
            }
            wb.u8(session.physicsMasterId());
            cp::net::OutboundMsg welcomeOut;
            welcomeOut.target = msg.sender;
            welcomeOut.frame  = wb.build();
            netThread.outTcp.push(std::move(welcomeOut));

            auto jb = cp::proto::MsgBuilder(MT::PARTICIPANT_JOIN)
                      .u8(pid).str(nick);
            cp::net::OutboundMsg joinOut;
            joinOut.target = 0xFF;
            joinOut.frame  = jb.build();
            netThread.outTcp.push(std::move(joinOut));

            broadcastAuthorityMap();

            // Track connection ID → participant ID for authority translation
            connIdMap_[msg.sender] = pid;
            break;
        }

        case MT::WELCOME: {
            cp::ParticipantId myId = r.u8();
            session.setMyId(myId);
            session.setIsHost(false);

            uint16_t count = r.u16();
            for (uint16_t i = 0; i < count; ++i) {
                cp::ParticipantId pid = r.u8();
                std::string nick    = r.str();
                std::string roleId  = r.str();
                uint8_t nzones      = r.u8();
                std::vector<std::string> zones;
                for (uint8_t z = 0; z < nzones; ++z) zones.push_back(r.str());
                session.addParticipant(nick);
                session.assignZones(pid, zones);
                session.setRole(pid, roleId, config.get().roles);
            }
            cp::ParticipantId pm = r.u8();
            session.setPhysicsMaster(pm);

            connWin.setState(cp::ui::ConnState::CONNECTED);
            Log("Joined session as participant %u", myId);

            // Announce our UDP endpoint to the server so it can send us physics state
            uint8_t ann[2] = {static_cast<uint8_t>(cp::proto::UdpType::ANNOUNCE), myId};
            cp::net::UdpDatagram udpAnn;
            udpAnn.data.assign(ann, ann + 2);
            netThread.outUdp.push(std::move(udpAnn));
            break;
        }

        case MT::PARTICIPANT_JOIN: {
            cp::ParticipantId pid = r.u8();
            std::string nick = r.str();
            (void)pid;
            session.addParticipant(nick);
            break;
        }

        case MT::PARTICIPANT_LEAVE: {
            cp::ParticipantId pid = r.u8();
            session.removeParticipant(pid);
            break;
        }

        case MT::AUTHORITY_MAP: {
            uint16_t n = r.u16();
            cp::AuthorityMap map;
            for (uint16_t i = 0; i < n; ++i) {
                std::string zone = r.str();
                cp::ParticipantId pid = r.u8();
                map[zone] = pid;
            }
            session.updateFromAuthorityMap(map);
            break;
        }

        case MT::DATAREF_SET: {
            uint16_t idx = r.u16();
            auto vtype = static_cast<cp::DrType>(r.u8());
            cp::DrValue val;
            val.type = vtype;
            switch (vtype) {
                case cp::DrType::INT:       val.i = static_cast<int>(r.u32()); break;
                case cp::DrType::FLOAT:     val.f = r.f32(); break;
                case cp::DrType::DOUBLE:    val.d = r.f64(); break;
                case cp::DrType::INT_ARR: {
                    uint16_t n = r.u16();
                    val.ia.resize(n);
                    for (auto& x : val.ia) x = static_cast<int>(r.u32());
                    break;
                }
                case cp::DrType::FLOAT_ARR: {
                    uint16_t n = r.u16();
                    val.fa.resize(n);
                    for (auto& x : val.fa) x = r.f32();
                    break;
                }
                default: break;
            }
            // Translate TCP connection ID → participant ID for authority check
            uint8_t effectiveSender = msg.sender;
            {
                auto it = connIdMap_.find(msg.sender);
                if (it != connIdMap_.end()) effectiveSender = it->second;
            }
            syncEngine.applyIncoming(idx, val, effectiveSender);

            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1] = plen & 0xFF;
                relay.frame[2] = (plen>>8) & 0xFF;
                relay.frame[3] = (plen>>16) & 0xFF;
                relay.frame[4] = (plen>>24) & 0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::COMMAND_FIRE: {
            uint16_t idx = r.u16();
            {
                uint8_t effectiveSender = msg.sender;
                auto it = connIdMap_.find(msg.sender);
                if (it != connIdMap_.end()) effectiveSender = it->second;
                syncEngine.applyIncomingCommand(idx, effectiveSender);
            }
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1] = plen & 0xFF;
                relay.frame[2] = (plen>>8) & 0xFF;
                relay.frame[3] = (plen>>16) & 0xFF;
                relay.frame[4] = (plen>>24) & 0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::ROLE_ASSIGN: {
            cp::ParticipantId pid = r.u8();
            std::string roleId = r.str();
            if (session.isHost()) {
                session.setRole(pid, roleId, config.get().roles);
                broadcastAuthorityMap();
            }
            break;
        }

        case MT::ZONE_ASSIGN: {
            cp::ParticipantId pid = r.u8();
            uint8_t n = r.u8();
            std::vector<std::string> zones;
            for (uint8_t i = 0; i < n; ++i) zones.push_back(r.str());
            if (session.isHost()) {
                session.assignZones(pid, zones);
                broadcastAuthorityMap();
            }
            break;
        }

        case MT::PHYSICS_MASTER_SET: {
            cp::ParticipantId pid = r.u8();
            session.setPhysicsMaster(pid);
            break;
        }

        case MT::WEATHER_MASTER_SET: {
            cp::ParticipantId pid = r.u8();
            session.setWeatherMaster(pid);
            if (session.isHost()) {
                auto frame = cp::proto::MsgBuilder(MT::WEATHER_MASTER_SET).u8(pid).build();
                cp::net::OutboundMsg out;
                out.target = 0xFF;
                out.frame  = std::move(frame);
                netThread.outTcp.push(std::move(out));
            }
            break;
        }

        case MT::WEATHER_STATE: {
            weatherSync.onTcpMessage(msg.payload.data(), msg.payload.size());
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1] = plen & 0xFF;
                relay.frame[2] = (plen >> 8) & 0xFF;
                relay.frame[3] = (plen >> 16) & 0xFF;
                relay.frame[4] = (plen >> 24) & 0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin() + 5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::KICK: {
            cp::ParticipantId pid = r.u8();
            if (pid == session.myId()) {
                onDisconnect();
                connWin.setState(cp::ui::ConnState::CONNECT_ERROR, "You were kicked from the session.");
            }
            break;
        }

        case MT::HEARTBEAT:
            break;

        default:
            LogWarning("Unhandled TCP message type 0x%02X", msg.type);
            break;
        }
    }

    void broadcastAuthorityMap()
    {
        auto map = session.buildAuthorityMap();
        auto b = cp::proto::MsgBuilder(cp::proto::MsgType::AUTHORITY_MAP);
        b.u16(static_cast<uint16_t>(map.size()));
        for (const auto& [zone, pid] : map)
            b.str(zone).u8(pid);
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        netThread.outTcp.push(std::move(out));
    }

    void sendDatarefSet(uint16_t idx, const cp::DrValue& val)
    {
        using MT = cp::proto::MsgType;
        auto b = cp::proto::MsgBuilder(MT::DATAREF_SET).u16(idx).u8(static_cast<uint8_t>(val.type));
        switch (val.type) {
            case cp::DrType::INT:       b.u32(static_cast<uint32_t>(val.i)); break;
            case cp::DrType::FLOAT:     b.f32(val.f); break;
            case cp::DrType::DOUBLE:    b.f64(val.d); break;
            case cp::DrType::INT_ARR:
                b.u16(static_cast<uint16_t>(val.ia.size()));
                for (int x : val.ia) b.u32(static_cast<uint32_t>(x));
                break;
            case cp::DrType::FLOAT_ARR:
                b.u16(static_cast<uint16_t>(val.fa.size()));
                for (float x : val.fa) b.f32(x);
                break;
            default: break;
        }
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        netThread.outTcp.push(std::move(out));
    }

    void sendCommandFire(uint16_t idx)
    {
        auto b = cp::proto::MsgBuilder(cp::proto::MsgType::COMMAND_FIRE).u16(idx);
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        netThread.outTcp.push(std::move(out));
    }

    void onHost(const cp::ui::ConnectionConfig& cfg)
    {
        onDisconnect();
        connWin.setState(cp::ui::ConnState::CONNECTING);

        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);
        connWin.setData(&session, &config.get());

        registry.build(config.get().datarefs, config.get().commands);
        syncEngine.init(&registry, &session);
        physicsSync.init(&session, &netThread);
        weatherSync.init(&session, &netThread);

        session.setIsHost(true);
        cp::ParticipantId myId = session.addParticipant(cfg.nick);
        session.setMyId(myId);

        netThread.startServer(cfg.port, cfg.port);

        std::string localIp = "127.0.0.1";
        {
            char hostname[256] = {};
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family   = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                if (getaddrinfo(hostname, nullptr, &hints, &res) == 0 && res) {
                    char ipbuf[INET_ADDRSTRLEN] = "127.0.0.1";
                    inet_ntop(AF_INET,
                              &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                              ipbuf, sizeof(ipbuf));
                    localIp = ipbuf;
                    freeaddrinfo(res);
                }
            }
        }
        connWin.setIsHost(true);
        connWin.setLocalEndpoint(localIp, cfg.port);
        connWin.setState(cp::ui::ConnState::CONNECTED);
        Log("Hosting on port %u  (local IP: %s)", cfg.port, localIp.c_str());
    }

    void onJoin(const cp::ui::ConnectionConfig& cfg)
    {
        onDisconnect();
        connWin.setState(cp::ui::ConnState::CONNECTING);

        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);

        registry.build(config.get().datarefs, config.get().commands);
        syncEngine.init(&registry, &session);
        physicsSync.init(&session, &netThread);
        weatherSync.init(&session, &netThread);

        netThread.startClient(cfg.host, cfg.port, cfg.port);

        auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::HELLO)
                     .u8(cp::proto::PROTOCOL_VERSION)
                     .str(cfg.nick).build();
        cp::net::OutboundMsg out;
        out.target = 0;
        out.frame  = std::move(frame);
        netThread.outTcp.push(std::move(out));
        Log("Joining %s:%u as '%s'", cfg.host.c_str(), cfg.port, cfg.nick.c_str());
    }

    void onDisconnect()
    {
        netThread.stop();
        session.clear();
        syncEngine.reset();
        physicsSync.reset();
        weatherSync.reset();
        registry.clear();
        config.reset();
        connIdMap_.clear();
        connRemoteIp_.clear();
        connWin.setState(cp::ui::ConnState::IDLE);
    }

    void onAircraftLoaded()
    {
        if (netThread.connected) {
            char path[512]; char file[256];
            XPLMGetNthAircraftModel(0, file, path);
            std::string dir = path;
            size_t sep = dir.find_last_of("/\\");
            if (sep != std::string::npos) dir = dir.substr(0, sep);
            config.load(dir);
            registry.build(config.get().datarefs, config.get().commands);
            syncEngine.reset();
        }
    }

    void onMenuClick(int item)
    {
        if (item == 0)
            connWin.setVisible(!connWin.visible());
        else if (item == 1)
            statusHud.setVisible(!statusHud.visible());
    }
};

static CoPilotsPlugin* g_plugin = nullptr;

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc)
{
    strncpy(outName, "CoPilots",         64);
    strncpy(outSig,  "com.copilots.xp11", 64);
    strncpy(outDesc, "Multi-crew shared cockpit for X-Plane 11 — unlimited crew, zone-based authority", 256);

    g_plugin = new CoPilotsPlugin();

    XPLMMenuID pluginsMenu = XPLMFindPluginsMenu();
    g_plugin->menuItem = XPLMAppendMenuItem(pluginsMenu, "CoPilots", nullptr, 1);
    g_plugin->menuId = XPLMCreateMenu("CoPilots", pluginsMenu, g_plugin->menuItem,
        [](void* ref, void* param) {
            static_cast<CoPilotsPlugin*>(ref)->onMenuClick(
                static_cast<int>(reinterpret_cast<intptr_t>(param)));
        }, g_plugin);
    XPLMAppendMenuItem(g_plugin->menuId, "Connect / Host", reinterpret_cast<void*>(0), 1);
    XPLMAppendMenuItem(g_plugin->menuId, "Toggle HUD",     reinterpret_cast<void*>(1), 1);

    cp::Log("XPluginStart — CoPilots loaded");
    return 1;
}

PLUGIN_API void XPluginStop()
{
    if (g_plugin) {
        if (g_plugin->active) g_plugin->onDisablePlugin();
        XPLMDestroyMenu(g_plugin->menuId);
        delete g_plugin;
        g_plugin = nullptr;
    }
    cp::Log("XPluginStop");
}

PLUGIN_API int XPluginEnable()
{
    if (g_plugin) g_plugin->onEnable();
    return 1;
}

PLUGIN_API void XPluginDisable()
{
    if (g_plugin) g_plugin->onDisablePlugin();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID , long msg, void* )
{
    if (!g_plugin) return;
    if (msg == 203) g_plugin->onAircraftLoaded();
}
