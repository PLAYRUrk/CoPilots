// plugin.cpp — CoPilots X-Plane 11 Plugin
// Entry point: XPluginStart / XPluginStop / XPluginEnable / XPluginDisable / XPluginReceiveMessage
//
// Ownership hierarchy:
//   Plugin (singleton)
//     ├── Config
//     ├── DatarefRegistry
//     ├── Session
//     ├── NetThread
//     ├── SyncEngine
//     ├── PhysicsSync
//     └── UI (ImguiBackend, ConnectionWindow, LobbyWindow, StatusHud)

#include "Log.h"
#include "config/Config.h"
#include "sync/DatarefRegistry.h"
#include "sync/SyncEngine.h"
#include "sync/PhysicsSync.h"
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
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

// ── Plugin class ──────────────────────────────────────────────────────────
// Bring cp::Log / cp::LogWarning into scope for the whole file
using namespace cp;

struct CoPilotsPlugin {

    cp::Config              config;
    cp::DatarefRegistry     registry;
    cp::Session             session;
    cp::net::NetThread      netThread;
    cp::SyncEngine          syncEngine;
    cp::PhysicsSync         physicsSync;

    // UI
    cp::ui::ConnectionWindow connWin;
    cp::ui::StatusHud        statusHud;

    // X-Plane menu
    XPLMMenuID menuId    = nullptr;
    int        menuItem  = -1;

    // Flight loop handle
    XPLMFlightLoopID flLoop = nullptr;

    bool active = false; // between Enable and Disable

    // ── Construction / init ───────────────────────────────────────────────
    void onEnable()
    {
        Log("Plugin enabled");
        cp::net::InitNetwork();

        // Wire session callbacks
        session.onChanged = [this]() { /* UI refreshed in next draw */ };

        // Wire UI → network
        connWin.onHost = [this](const cp::ui::ConnectionConfig& cfg) {
            onHost(cfg);
        };
        connWin.onJoin = [this](const cp::ui::ConnectionConfig& cfg) {
            onJoin(cfg);
        };
        connWin.onDisconnect = [this]() { onDisconnect(); };

        // Lobby / admin callbacks wired directly into the main window
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

        connWin.init();
        statusHud.init();
        statusHud.setSession(&session);
        connWin.setData(&session, &config.get());

        // Register flight loop callback (~20 Hz)
        XPLMCreateFlightLoop_t params{};
        params.structSize = sizeof(params);
        params.phase      = xplm_FlightLoop_Phase_AfterFlightModel;
        params.callbackFunc = [](float /*sincelast*/, float /*elapsed*/,
                                 int /*count*/, void* ref) -> float {
            static_cast<CoPilotsPlugin*>(ref)->onFlightLoop();
            return -1.f; // reschedule every frame
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

    // ── Flight loop ───────────────────────────────────────────────────────
    void onFlightLoop()
    {
        if (!active) return;

        // 1. Drain inbound TCP messages
        cp::net::InboundMsg msg;
        while (netThread.inTcp.pop(msg)) {
            handleTcpMessage(msg);
        }

        // 2. Drain inbound UDP datagrams
        cp::net::UdpDatagram dg;
        while (netThread.inUdp.pop(dg)) {
            if (!dg.data.empty()) {
                uint8_t t = dg.data[0];
                if (t == static_cast<uint8_t>(cp::proto::UdpType::PHYSICS_STATE))
                    physicsSync.onUdpDatagram(dg.data.data(), dg.data.size());
            }
        }

        // 3. Sync engine tick (detect changed datarefs, fire callbacks)
        if (registry.datarefs().size() > 0 && netThread.connected) {
            syncEngine.tick(
                // onDatarefChanged: build DATAREF_SET message and queue it
                [this](uint16_t idx, const cp::DrValue& val) {
                    sendDatarefSet(idx, val);
                },
                // onCommandFired
                [this](uint16_t idx) {
                    sendCommandFire(idx);
                }
            );
        }

        // 4. Physics sync tick
        if (netThread.connected)
            physicsSync.tick();

        // 5. Update connection status in UI
        if (netThread.hasError) {
            connWin.setState(cp::ui::ConnState::CONNECT_ERROR, netThread.lastError);
            netThread.hasError = false;
            session.clear();
        }
    }

    // ── TCP message handling ──────────────────────────────────────────────
    void handleTcpMessage(const cp::net::InboundMsg& msg)
    {
        using MT = cp::proto::MsgType;
        auto type = static_cast<MT>(msg.type);
        cp::proto::MsgReader r(msg.payload.data(), msg.payload.size());

        switch (type) {
        // Server-side: receive from clients
        case MT::HELLO: {
            uint8_t ver = r.u8();
            std::string nick = r.str();
            (void)ver;
            // Add participant, send WELCOME back with assigned id + authority map
            cp::ParticipantId pid = session.addParticipant(nick);

            // Build WELCOME
            auto authMap = session.buildAuthorityMap();
            auto wb = cp::proto::MsgBuilder(MT::WELCOME).u8(pid);
            wb.u16(static_cast<uint16_t>(session.participants().size()));
            for (const auto& p : session.participants()) {
                wb.u8(p.id).str(p.nick).str(p.roleId);
                wb.u8(static_cast<uint8_t>(p.zoneIds.size()));
                for (const auto& z : p.zoneIds) wb.str(z);
            }
            wb.u8(session.physicsMasterId());
            // Send WELCOME only to the new client
            cp::net::OutboundMsg welcomeOut;
            welcomeOut.target = msg.sender;
            welcomeOut.frame  = wb.build();
            netThread.outTcp.push(std::move(welcomeOut));

            // Broadcast PARTICIPANT_JOIN to others
            auto jb = cp::proto::MsgBuilder(MT::PARTICIPANT_JOIN)
                      .u8(pid).str(nick);
            cp::net::OutboundMsg joinOut;
            joinOut.target = 0xFF;
            joinOut.frame  = jb.build();
            netThread.outTcp.push(std::move(joinOut));

            broadcastAuthorityMap();
            break;
        }

        case MT::WELCOME: {
            // Client receives from server
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
                session.addParticipant(nick); // returns pid; we then assign
                session.assignZones(pid, zones);
                session.setRole(pid, roleId, config.get().roles);
            }
            cp::ParticipantId pm = r.u8();
            session.setPhysicsMaster(pm);

            connWin.setState(cp::ui::ConnState::CONNECTED);
            Log("Joined session as participant %u", myId);
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
            syncEngine.applyIncoming(idx, val, msg.sender);

            // If server, relay to all others
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target = 0xFF; // server will skip the sender internally
                // Rebuild the frame (easiest; payload already contains it)
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
            syncEngine.applyIncomingCommand(idx, msg.sender);
            // Relay on server
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

        case MT::KICK: {
            cp::ParticipantId pid = r.u8();
            if (pid == session.myId()) {
                // We were kicked
                onDisconnect();
                connWin.setState(cp::ui::ConnState::CONNECT_ERROR, "You were kicked from the session.");
            }
            break;
        }

        case MT::HEARTBEAT:
            break; // nothing to do

        default:
            LogWarning("Unhandled TCP message type 0x%02X", msg.type);
            break;
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────
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

    // ── Session actions ───────────────────────────────────────────────────
    void onHost(const cp::ui::ConnectionConfig& cfg)
    {
        onDisconnect();
        connWin.setState(cp::ui::ConnState::CONNECTING);

        // Load aircraft config
        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        // Strip filename from path
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);
        connWin.setData(&session, &config.get());

        // Build registry
        registry.build(config.get().datarefs, config.get().commands);
        syncEngine.init(&registry, &session);
        physicsSync.init(&session, &netThread);

        // Add ourselves as first participant
        session.setIsHost(true);
        cp::ParticipantId myId = session.addParticipant(cfg.nick);
        session.setMyId(myId);

        netThread.startServer(cfg.port, static_cast<uint16_t>(cfg.port + 1));

        // Resolve local IP to show the crew what address to connect to
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

        // Load aircraft config (same aircraft assumed)
        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);

        registry.build(config.get().datarefs, config.get().commands);
        syncEngine.init(&registry, &session);
        physicsSync.init(&session, &netThread);

        netThread.startClient(cfg.host, cfg.port, static_cast<uint16_t>(cfg.port + 1));

        // Send HELLO once connected
        // (we schedule a small check in the flight loop; connection can take a tick)
        // For now, send immediately — NetThread queues until connection is ready.
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
        registry.clear();
        config.reset();
        connWin.setState(cp::ui::ConnState::IDLE);
    }

    // ── Aircraft load/unload via XPluginReceiveMessage ────────────────────
    void onAircraftLoaded()
    {
        // If session is active, rebuild the registry for the new aircraft
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

    // ── Menu ──────────────────────────────────────────────────────────────
    void onMenuClick(int item)
    {
        if (item == 0)
            connWin.setVisible(!connWin.visible());
        else if (item == 1)
            statusHud.setVisible(!statusHud.visible());
    }
};

// ── Global plugin instance ────────────────────────────────────────────────
static CoPilotsPlugin* g_plugin = nullptr;

// ── X-Plane plugin exports ────────────────────────────────────────────────
PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc)
{
    strncpy(outName, "CoPilots",         64);
    strncpy(outSig,  "com.copilots.xp11", 64);
    strncpy(outDesc, "Multi-crew shared cockpit for X-Plane 11 — unlimited crew, zone-based authority", 256);

    g_plugin = new CoPilotsPlugin();

    // Build menu
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

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID /*from*/, long msg, void* /*param*/)
{
    if (!g_plugin) return;
    // XPLM_MSG_PLANE_LOADED = 203
    if (msg == 203) g_plugin->onAircraftLoaded();
}
