#include "Log.h"
#include "config/Config.h"
#include "config/Prefs.h"
#include "sync/DatarefRegistry.h"
#include "sync/SyncEngine.h"
#include "sync/PhysicsSync.h"
#include "sync/PointerSync.h"
#include "sync/WeatherSync.h"
#include "sync/FmsPlanSync.h"
#include "session/Session.h"
#include "net/NetThread.h"
#include "net/Protocol.h"
#include "net/Transport.h"
#include "net/ConfigDownloader.h"
#include "ui/ConnectionWindow.h"
#include "ui/StatusHud.h"
#include "ui/NotepadWindow.h"
#include "ui/Notepad.h"
#include "ui/ChartsWindow.h"
#include "chart/ChartFoxAppConfig.h"
#include "chart/ChartTypes.h"

#include <XPLM/XPLMPlugin.h>
#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMProcessing.h>
#include <XPLM/XPLMUtilities.h>
#include <XPLM/XPLMMenus.h>
#include <XPLM/XPLMPlanes.h>
#include <XPLM/XPLMDataAccess.h>

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
    cp::net::ConfigDownloader cfgDownloader;
    cp::SyncEngine          syncEngine;
    cp::PhysicsSync         physicsSync;
    cp::PointerSync         pointerSync;
    cp::WeatherSync         weatherSync;
    cp::FmsPlanSync         fmsSync;

    cp::ui::ConnectionWindow connWin;
    cp::ui::StatusHud        statusHud;
    cp::ui::NotepadWindow    notepadWin;
    cp::ui::ChartsWindow     chartsWin;

    // Host-authoritative shared notepad state (host only; cleared on disconnect).
    cp::notepad::Notepad     sharedNotepad_;
    // Host-authoritative shared ChartFox tabs (host only; cleared on disconnect).
    cp::chart::SharedCharts  sharedCharts_;

    // Maps TCP connection ID (from network thread) → participant ID (from session)
    std::map<uint8_t, cp::ParticipantId> connIdMap_;
    // Maps TCP connection ID → remote IP (received in 0xFD message)
    std::map<uint8_t, std::string> connRemoteIp_;

    // Lobby settings (set when hosting begins)
    std::string lobbyPassword_;
    bool        requireJoinApproval_    = true;
    bool        requireControlApproval_ = true;

    struct PendingConn { uint8_t connId; std::string nick; uint32_t drHash; };
    std::vector<PendingConn> pendingConns_;

    // Deferred physics-master restore after a rejoin: granting instantly would
    // teleport the crew to the rejoiner's stale position; the delay lets its
    // PhysicsSync snap to the current master first.
    struct PendingPhysRestore {
        cp::ParticipantId pid = cp::INVALID_PARTICIPANT_ID;
        float delayS = 0.f;
    } pendingPhysRestore_;

    XPLMMenuID menuId    = nullptr;
    int        menuItem  = -1;

    XPLMFlightLoopID flLoop = nullptr;

    // Shared laser pointer: hold-to-aim command + 3-D draw callback.
    XPLMCommandRef cmdPointer_ = nullptr;
    bool pointerEnabledPref_ = false;  // Prefs::pointerEnabled, loaded in onEnable

    bool active = false;

    // ── Pause synchronisation ────────────────────────────────────────────────
    // The pause STATE (sim/time/paused) is mirrored, not the toggle command:
    // state sync also covers auto-pause from menus/settings screens, and two
    // diverged sims converge instead of swapping states like a relayed toggle.
    XPLMDataRef    drPaused_       = nullptr;
    XPLMCommandRef cmdPauseToggle_ = nullptr;
    int  lastPaused_     = -1;   // last observed local state
    int  expectedPaused_ = -1;   // state we just commanded on behalf of the network
    bool pauseConnected_ = false;

    // X-Plane root (from XPLMGetSystemPath, trailing separator included).
    std::string xpSystemPath_;

    void onEnable()
    {
        Log("Plugin enabled");
        cp::net::InitNetwork();
        {
            char xpPath[512] = {};
            XPLMGetSystemPath(xpPath);
            xpSystemPath_ = xpPath;
        }

        // Prefill the connect form from persisted prefs so a crew member whose
        // sim crashed mid-flight can rejoin without retyping anything.
        {
            cp::Prefs prefs;
            if (cp::LoadPrefs(xpSystemPath_, prefs))
                connWin.setConnectDefaults(prefs.nick, prefs.address, prefs.hostPort,
                                           prefs.password, prefs.bindIp,
                                           prefs.requireJoinApproval,
                                           prefs.requireControlApproval);
            pointerEnabledPref_ = prefs.pointerEnabled;
            connWin.setPointerEnabled(prefs.pointerEnabled);
        }

        connWin.onPointerEnabledChanged = [this](bool en) {
            pointerEnabledPref_ = en;
            pointerSync.setEnabled(en);
            cp::Prefs p;
            cp::LoadPrefs(xpSystemPath_, p);   // keep untouched fields
            p.pointerEnabled = en;
            cp::SavePrefs(xpSystemPath_, p);
        };

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
            handleParticipantGone(pid);
        };
        connWin.onPhysicsMasterSet = [this](cp::ParticipantId pid) {
            hostSetPhysicsMaster(pid);
        };
        connWin.onWeatherMasterSet = [this](cp::ParticipantId pid) {
            hostSetWeatherMaster(pid);
        };

        connWin.onAcceptJoin = [this](uint8_t connId) {
            auto it = std::find_if(pendingConns_.begin(), pendingConns_.end(),
                                   [connId](const PendingConn& p){ return p.connId == connId; });
            if (it != pendingConns_.end()) {
                acceptJoin(it->connId, it->nick);
                pendingConns_.erase(it);
            }
        };
        connWin.onRejectJoin = [this](uint8_t connId) {
            auto rf = cp::proto::MsgBuilder(cp::proto::MsgType::REJECT)
                      .str("Connection rejected by host.").build();
            cp::net::OutboundMsg ro;
            ro.target = connId;
            ro.frame  = std::move(rf);
            netThread.outTcp.push(std::move(ro));
            pendingConns_.erase(
                std::remove_if(pendingConns_.begin(), pendingConns_.end(),
                               [connId](const PendingConn& p){ return p.connId == connId; }),
                pendingConns_.end());
        };
        connWin.onDownloadConfig = [this]() {
            char acfPath[512]; char acfFile[256];
            XPLMGetNthAircraftModel(0, acfFile, acfPath);
            std::string dir = acfPath;
            size_t sep = dir.find_last_of("/\\");
            if (sep != std::string::npos) dir = dir.substr(0, sep);
            if (cfgDownloader.start(dir, acfFile))
                connWin.setDownloadStatus("Downloading...");
        };
        connWin.onDownloadConfigEntry = [this](const std::string& folder,
                                               const std::string& aircraft) {
            char acfPath[512]; char acfFile[256];
            XPLMGetNthAircraftModel(0, acfFile, acfPath);
            std::string dir = acfPath;
            size_t sep = dir.find_last_of("/\\");
            if (sep != std::string::npos) dir = dir.substr(0, sep);
            if (cfgDownloader.startFolder(dir, folder, aircraft))
                connWin.setDownloadStatus("Downloading...");
        };
        connWin.onRequestControl = [this]() {
            auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::CONTROL_REQUEST).build();
            cp::net::OutboundMsg out;
            out.target = 0;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
        };
        connWin.onGrantControl = [this](cp::ParticipantId pid) {
            hostSetPhysicsMaster(pid);
        };
        connWin.onDenyControl = [this](cp::ParticipantId pid) {
            for (const auto& [cid, ppid] : connIdMap_) {
                if (ppid == pid) {
                    auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::CONTROL_DENY)
                                 .str("Control request denied by host.").build();
                    cp::net::OutboundMsg out;
                    out.target = cid;
                    out.frame  = std::move(frame);
                    netThread.outTcp.push(std::move(out));
                    break;
                }
            }
        };

        connWin.init();
        statusHud.init();
        statusHud.setSession(&session);
        connWin.setData(&session, &config.get());

        notepadWin.init();
        notepadWin.setSession(&session);
        // Wire up the notepad send function: push a framed TCP message to the network
        // (target=0xFF on host broadcasts; on clients the transport ignores target).
        notepadWin.sendFn = [this](std::vector<uint8_t> frame) {
            cp::net::OutboundMsg out;
            out.target = 0xFF;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
        };

        // ChartFox charts window: pdfium.dll lives next to the plugin binary.
        {
            std::string pluginDir;
            char path[512] = {};
            XPLMGetPluginInfo(XPLMGetMyID(), nullptr, path, nullptr, nullptr);
            pluginDir = path;
            size_t sep = pluginDir.find_last_of("/\\");
            if (sep != std::string::npos) pluginDir = pluginDir.substr(0, sep);
            chartsWin.init(pluginDir, xpSystemPath_);
        }
        chartsWin.setSession(&session);
        chartsWin.sendFn = [this](std::vector<uint8_t> frame) {
            cp::net::OutboundMsg out;
            out.target = 0xFF;
            out.frame  = std::move(frame);
            netThread.outTcp.push(std::move(out));
        };
        {
            cp::Prefs prefs;
            cp::LoadPrefs(xpSystemPath_, prefs);
            chartsWin.setToken(prefs.chartfoxToken);
            chartsWin.setCalSubmitUrl(prefs.calSubmitUrl);
            // A stored scope list equal to an older build's built-in default is
            // a leftover, not a deliberate override: drop it so the current
            // built-in set (which gained charts:geos) takes effect.
            std::string scopes = prefs.chartfoxScopes;
            if (scopes == cp::chart::kChartFoxLegacyScopes) scopes.clear();
            chartsWin.setOauth(prefs.chartfoxClientId, prefs.chartfoxClientSecret,
                               prefs.chartfoxRedirectUri, scopes,
                               prefs.chartfoxAccessToken, prefs.chartfoxRefreshToken,
                               prefs.chartfoxTokenExpiry, prefs.chartfoxTokenScopes);
        }
        chartsWin.onTokenChanged = [this](const std::string& token) {
            cp::Prefs p;
            cp::LoadPrefs(xpSystemPath_, p);   // keep untouched fields
            p.chartfoxToken = token;
            cp::SavePrefs(xpSystemPath_, p);
        };
        chartsWin.onOauthClientChanged = [this](const std::string& id,
                                                const std::string& secret,
                                                const std::string& redirect,
                                                const std::string& scopes) {
            cp::Prefs p;
            cp::LoadPrefs(xpSystemPath_, p);
            p.chartfoxClientId     = id;
            p.chartfoxClientSecret = secret;
            p.chartfoxRedirectUri  = redirect;
            p.chartfoxScopes       = scopes;
            cp::SavePrefs(xpSystemPath_, p);
        };
        chartsWin.onOauthTokensChanged = [this](const std::string& access,
                                                const std::string& refresh,
                                                int64_t expiry,
                                                const std::string& tokenScopes) {
            cp::Prefs p;
            cp::LoadPrefs(xpSystemPath_, p);
            p.chartfoxAccessToken  = access;
            p.chartfoxRefreshToken = refresh;
            p.chartfoxTokenExpiry  = expiry;
            p.chartfoxTokenScopes  = tokenScopes;
            cp::SavePrefs(xpSystemPath_, p);
        };
        chartsWin.setVisible(false);   // opened via menu / HUD button

        // StatusHud quick-open buttons
        statusHud.onToggleConn    = [this]() { connWin.setVisible(!connWin.visible()); };
        statusHud.onToggleNotepad = [this]() { notepadWin.setVisible(!notepadWin.visible()); };
        statusHud.onToggleCharts  = [this]() { chartsWin.setVisible(!chartsWin.visible()); };
        weatherSync.init(&session, &netThread, xpSystemPath_);
        fmsSync.init(&netThread, xpSystemPath_);
        drPaused_       = XPLMFindDataRef("sim/time/paused");
        cmdPauseToggle_ = XPLMFindCommand("sim/operation/pause_toggle");

        // Shared laser pointer: bindable hold command + 3-D draw callback.
        // The command is plugin-owned and never enters the DatarefRegistry, so
        // it is not relayed by the SyncEngine command sync.
        pointerSync.init(&session, &netThread);
        pointerSync.setEnabled(pointerEnabledPref_);
        cmdPointer_ = XPLMCreateCommand("CoPilots/pointer_hold",
                                        "Hold to aim shared laser pointer");
        XPLMRegisterCommandHandler(cmdPointer_, pointerCmdCb, 1, this);
        XPLMRegisterDrawCallback(pointerDrawCb, xplm_Phase_Airplanes, 0 /*after*/, this);

        XPLMCreateFlightLoop_t params{};
        params.structSize = sizeof(params);
        params.phase      = xplm_FlightLoop_Phase_AfterFlightModel;
        params.callbackFunc = [](float inElapsed, float ,
                                 int , void* ref) -> float {
            static_cast<CoPilotsPlugin*>(ref)->onFlightLoop(inElapsed);
            return -1.f;
        };
        params.refcon = this;
        flLoop = XPLMCreateFlightLoop(&params);
        XPLMScheduleFlightLoop(flLoop, -1.f, 1);

        active = true;
        Log("CoPilots ready — open Plugins > CoPilots to connect");
    }

    static int pointerCmdCb(XPLMCommandRef, XPLMCommandPhase phase, void* ref)
    {
        auto* self = static_cast<CoPilotsPlugin*>(ref);
        if (phase == xplm_CommandBegin)    self->pointerSync.setHolding(true);
        else if (phase == xplm_CommandEnd) self->pointerSync.setHolding(false);
        return 1;
    }

    static int pointerDrawCb(XPLMDrawingPhase, int, void* ref)
    {
        static_cast<CoPilotsPlugin*>(ref)->pointerSync.onDraw();
        return 1;
    }

    void onDisablePlugin()
    {
        if (!active) return;
        active = false;

        XPLMUnregisterDrawCallback(pointerDrawCb, xplm_Phase_Airplanes, 0, this);
        if (cmdPointer_) {
            XPLMUnregisterCommandHandler(cmdPointer_, pointerCmdCb, 1, this);
            cmdPointer_ = nullptr;
        }

        onDisconnect();

        XPLMDestroyFlightLoop(flLoop);
        flLoop = nullptr;

        connWin.shutdown();
        statusHud.shutdown();
        notepadWin.shutdown();
        chartsWin.shutdown();
        cp::net::ShutdownNetwork();
        Log("Plugin disabled");
    }

    void onFlightLoop(float inElapsed)
    {
        if (!active) return;

        // ChartFox OAuth: finish a code exchange and refresh tokens even when the
        // Charts window is closed (it used to tick only while rendering, so a
        // login started and then dismissed never completed).
        chartsWin.tickBackground();

        // Clamp dt to a sane range: guard against first-frame zero, paused sim
        // spike, or sim-speed values that would blow up the feed-forward integrator.
        double dt = static_cast<double>(inElapsed);
        if (dt <= 0.0 || dt > 0.1) dt = 1.0 / 60.0;

        // Detect and send local user changes FIRST, before processing any incoming
        // network messages. This prevents SASL callbacks triggered by applyIncoming()
        // from being mistaken for local user changes on this tick.
        if (registry.datarefs().size() > 0 && netThread.connected) {
            syncEngine.tick(
                [this](uint16_t idx, const cp::DrValue& val) {
                    sendDatarefSet(idx, val);
                },
                [this](uint16_t idx, uint8_t phase) {
                    sendCommandFire(idx, phase);
                }
            );
        }

        bool anyDatarefApplied = false;
        cp::net::InboundMsg msg;
        while (netThread.inTcp.pop(msg)) {
            if (static_cast<cp::proto::MsgType>(msg.type) == cp::proto::MsgType::DATAREF_SET)
                anyDatarefApplied = true;
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
            } else if (t == static_cast<uint8_t>(cp::proto::UdpType::POINTER_STATE)) {
                pointerSync.onUdpDatagram(dg.data.data(), dg.data.size());
                if (session.isHost()) {
                    cp::net::UdpDatagram relay;
                    relay.data = dg.data;
                    // empty `to` triggers broadcast in serverLoop; the sender
                    // drops its own echo by sender_id
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

        if (netThread.connected)
            physicsSync.tick(dt);

        // On non-masters: refresh the SyncEngine cache after applyState() has written
        // throttle/brake/flap datarefs and after SASL callbacks may have fired from
        // applyIncoming(). Without this, SyncEngine would detect those writes as local
        // user changes on the next tick and send them back to the physics master,
        // causing the snap-back / jitter feedback loop.
        if (netThread.connected && !session.isPhysicsMaster())
            syncEngine.refreshCache();

        // RESYNC_REQUEST and periodic full resync were removed.
        //
        // The RESYNC_REQUEST mechanism asked the host for a full resync every ~2 seconds
        // whenever the client received any dataref update.  The host responded by sending
        // ALL datarefs, which overwrote any values the client had just set locally —
        // causing switch snap-back.  Similarly, the host's periodic 5-second full resync
        // pushed stale host values back to clients even for switches the client had moved.
        //
        // TCP is reliable: no packets are dropped, so a missed change cannot happen.
        // Each side sends only when its own value changes; the suppress window handles
        // SASL side-effects.  Full resync is only needed at join time (acceptJoin).

        if (netThread.connected)
            weatherSync.tick(1.f / 60.f);

        // Deferred physics-master restore after a rejoin (see acceptJoin).
        if (session.isHost() && pendingPhysRestore_.pid != cp::INVALID_PARTICIPANT_ID) {
            pendingPhysRestore_.delayS -= static_cast<float>(dt);
            if (pendingPhysRestore_.delayS <= 0.f) {
                cp::ParticipantId pid = pendingPhysRestore_.pid;
                pendingPhysRestore_ = {};
                if (session.find(pid)) {
                    Log("CoPilots: restoring physics master to rejoined participant %u", pid);
                    hostSetPhysicsMaster(pid);
                }
            }
        }

        // Flight-plan folder sync (Output/FMS plans): announce + periodic rescan.
        fmsSync.tick(static_cast<float>(dt), netThread.connected);

        // Pause-state sync: broadcast local changes of sim/time/paused.
        if (drPaused_) {
            if (netThread.connected) {
                int p = XPLMGetDatai(drPaused_);
                if (!pauseConnected_) {
                    // Session just started — take the current state as the baseline
                    // without broadcasting it (a joining client must not force its
                    // pause state onto a flying crew).
                    pauseConnected_ = true;
                    lastPaused_     = p;
                    expectedPaused_ = -1;
                } else if (p != lastPaused_) {
                    lastPaused_ = p;
                    if (p == expectedPaused_) {
                        // This change was commanded by the network — don't echo it.
                        expectedPaused_ = -1;
                    } else {
                        auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::PAUSE_STATE)
                                         .u8(static_cast<uint8_t>(p != 0)).build();
                        cp::net::OutboundMsg out;
                        out.target = 0xFF;
                        out.frame  = std::move(frame);
                        netThread.outTcp.push(std::move(out));
                    }
                }
            } else {
                pauseConnected_ = false;
            }
        }

        if (netThread.hasError) {
            connWin.setState(cp::ui::ConnState::CONNECT_ERROR, netThread.lastError);
            netThread.hasError = false;
            session.clear();
        }

        // Config-library download result → UI status line.  The download runs on
        // a worker thread; the installed copilots.json is picked up on the next
        // Host/Join (an active session is never mutated mid-flight).
        {
            auto dlState = cfgDownloader.state();
            if (dlState == cp::net::ConfigDownloader::State::SUCCESS ||
                dlState == cp::net::ConfigDownloader::State::FAILED) {
                std::string msg = cfgDownloader.message();
                if (dlState == cp::net::ConfigDownloader::State::SUCCESS)
                    msg += netThread.connected
                         ? " Restart the session to apply."
                         : " It will be used on the next Host / Join.";
                connWin.setDownloadStatus(msg);
                // Publish the library list for the manual picker (available after
                // any attempt that managed to fetch the manifest).
                {
                    std::vector<std::pair<std::string,std::string>> list;
                    for (const auto& e : cfgDownloader.entries())
                        list.emplace_back(e.folder, e.aircraft);
                    connWin.setLibraryList(list);
                }
                cfgDownloader.acknowledge();
            }
        }

    }

    // Host-side master reassignment: apply locally AND broadcast, so clients
    // unlatch overrides / retarget their stale-packet filter.  (The silent
    // Session-internal fallback on removeParticipant is not enough.)
    void hostSetPhysicsMaster(cp::ParticipantId pid)
    {
        pendingPhysRestore_ = {};
        session.setPhysicsMaster(pid);
        physicsSync.onMasterChanged();
        auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::PHYSICS_MASTER_SET)
                     .u8(pid).build();
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = std::move(frame);
        netThread.outTcp.push(std::move(out));
    }

    void hostSetWeatherMaster(cp::ParticipantId pid)
    {
        session.setWeatherMaster(pid);
        auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::WEATHER_MASTER_SET)
                     .u8(pid).build();
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = std::move(frame);
        netThread.outTcp.push(std::move(out));
    }

    // Host-side departure handling shared by disconnect (0xFE) and kick: if the
    // departed participant was a master, take over explicitly with a broadcast.
    void handleParticipantGone(cp::ParticipantId pid)
    {
        cp::ParticipantId prevPhys = session.physicsMasterId();
        cp::ParticipantId prevWx   = session.weatherMasterId();
        session.removeParticipant(pid);
        if (pendingPhysRestore_.pid == pid) pendingPhysRestore_ = {};
        if (prevPhys == pid) {
            Log("CoPilots: physics master (pid=%u) left — host taking over", pid);
            hostSetPhysicsMaster(session.myId());
        }
        if (prevWx == pid) {
            Log("CoPilots: weather master (pid=%u) left — host taking over", pid);
            hostSetWeatherMaster(session.myId());
        }
    }

    void acceptJoin(uint8_t connId, const std::string& nick)
    {
        using MT = cp::proto::MsgType;

        // Rejoin restore: if this nick departed recently (sim crash), give back
        // its role/zones/master status.  Skipped when the nick is already online
        // (a second player picking the same name must not steal a ghost).
        cp::Ghost ghost;
        bool restore = !session.hasNickOnline(nick) && session.popGhost(nick, ghost);

        cp::ParticipantId pid = session.addParticipant(nick);

        if (restore) {
            if (!ghost.roleId.empty())
                session.setRole(pid, ghost.roleId, config.get().roles);
            // Restore only zones that are still unowned (or already ours via the
            // role defaults) — a zone the host manually reassigned to someone else
            // during the outage is not stolen back.
            std::vector<std::string> zones;
            for (const auto& z : ghost.zoneIds) {
                cp::ParticipantId owner = session.authorityFor(z);
                if (owner == cp::INVALID_PARTICIPANT_ID || owner == pid)
                    zones.push_back(z);
            }
            session.assignZones(pid, zones);
            Log("CoPilots: restoring '%s' (pid=%u): role='%s', %zu/%zu zones%s%s",
                nick.c_str(), pid, ghost.roleId.c_str(),
                zones.size(), ghost.zoneIds.size(),
                ghost.wasPhysicsMaster ? ", physics master (deferred)" : "",
                ghost.wasWeatherMaster ? ", weather master" : "");
        }

        auto wb = cp::proto::MsgBuilder(MT::WELCOME).u8(pid);
        wb.u16(static_cast<uint16_t>(session.participants().size()));
        for (const auto& p : session.participants()) {
            wb.u8(p.id).str(p.nick).str(p.roleId);
            wb.u8(static_cast<uint8_t>(p.zoneIds.size()));
            for (const auto& z : p.zoneIds) wb.str(z);
        }
        wb.u8(session.physicsMasterId());
        cp::net::OutboundMsg welcomeOut;
        welcomeOut.target = connId;
        welcomeOut.frame  = wb.build();
        netThread.outTcp.push(std::move(welcomeOut));

        // WELCOME carries only the physics master; tell the joiner who the
        // weather master is (TCP per-connection ordering puts this after WELCOME).
        {
            auto mb = cp::proto::MsgBuilder(MT::WEATHER_MASTER_SET)
                      .u8(session.weatherMasterId()).build();
            cp::net::OutboundMsg mout;
            mout.target = connId;
            mout.frame  = std::move(mb);
            netThread.outTcp.push(std::move(mout));
        }

        auto jb = cp::proto::MsgBuilder(MT::PARTICIPANT_JOIN).u8(pid).str(nick);
        cp::net::OutboundMsg joinOut;
        joinOut.target = 0xFF;
        joinOut.frame  = jb.build();
        netThread.outTcp.push(std::move(joinOut));

        broadcastAuthorityMap();
        connIdMap_[connId] = pid;
        syncEngine.requestFullSync();
        // Send the host's flight-plan inventory directly to the new client —
        // the session-start broadcast happened before this client connected.
        fmsSync.announce(connId);

        if (restore) {
            if (ghost.wasWeatherMaster)
                hostSetWeatherMaster(pid);
            if (ghost.wasPhysicsMaster)
                pendingPhysRestore_ = {pid, 5.0f};
        }
        Log("CoPilots: accepted join from '%s' as participant %u", nick.c_str(), pid);
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
            if (msg.sender == 0) {
                // Client side: the host closed the connection (or went silent —
                // NetThread puts "timeout" in the payload for a heartbeat timeout).
                bool timeout = !msg.payload.empty();
                Log("CoPilots: host disconnected%s", timeout ? " (timeout)" : "");
                onDisconnect();
                connWin.setState(cp::ui::ConnState::CONNECT_ERROR,
                                 timeout ? "Connection lost (timeout) — you can rejoin."
                                         : "Host disconnected.");
                break;
            }
            // Server side: a client disconnected
            auto it = connIdMap_.find(msg.sender);
            if (it != connIdMap_.end()) {
                cp::ParticipantId pid = it->second;
                handleParticipantGone(pid);
                auto frame = cp::proto::MsgBuilder(MT::PARTICIPANT_LEAVE).u8(pid).build();
                cp::net::OutboundMsg leaveOut;
                leaveOut.target = 0xFF;
                leaveOut.frame  = std::move(frame);
                netThread.outTcp.push(std::move(leaveOut));
                connIdMap_.erase(it);
            }
            // Also clean up pending joins (client disconnected before being accepted)
            pendingConns_.erase(
                std::remove_if(pendingConns_.begin(), pendingConns_.end(),
                               [&](const PendingConn& p){ return p.connId == msg.sender; }),
                pendingConns_.end());
            connWin.removePendingJoin(msg.sender);
            connRemoteIp_.erase(msg.sender);
            break;
        }

        case MT::HELLO: {
            uint8_t ver = r.u8();
            std::string nick      = r.str();
            uint32_t clientDrHash = r.empty() ? 0 : r.u32();
            std::string clientPwd = r.empty() ? "" : r.str();

            // Reject clients running a different protocol version.  PROTOCOL_VERSION is
            // bumped whenever the binary layout of PhysicsState (UDP) changes, so mismatched
            // peers would silently misinterpret packet fields (e.g. the new G/AoA fields
            // would be read as throttle or prop data on an old client).  A clear error here
            // is better than silent data corruption in the session.
            if (ver != cp::proto::PROTOCOL_VERSION) {
                char buf[192];
                snprintf(buf, sizeof(buf),
                         "Protocol version mismatch: you are running v%u, host requires v%u. "
                         "Please update all CoPilots installations to the same version.",
                         ver, cp::proto::PROTOCOL_VERSION);
                auto rf = cp::proto::MsgBuilder(MT::REJECT).str(std::string(buf)).build();
                cp::net::OutboundMsg ro;
                ro.target = msg.sender; ro.frame = std::move(rf);
                netThread.outTcp.push(std::move(ro));
                Log("CoPilots: rejected '%s' — protocol version mismatch client=%u host=%u",
                    nick.c_str(), ver, cp::proto::PROTOCOL_VERSION);
                break;
            }

            // Check lobby password
            if (!lobbyPassword_.empty() && clientPwd != lobbyPassword_) {
                auto rf = cp::proto::MsgBuilder(MT::REJECT)
                          .str("Wrong password.").build();
                cp::net::OutboundMsg ro;
                ro.target = msg.sender; ro.frame = std::move(rf);
                netThread.outTcp.push(std::move(ro));
                Log("CoPilots: rejected '%s' — wrong password", nick.c_str());
                break;
            }

            // Check dataref list hash
            if (clientDrHash != 0 && clientDrHash != config.get().drListHash) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "smartcopilot.cfg mismatch: client=0x%08X host=0x%08X — "
                         "ensure both sides use the same aircraft version.",
                         clientDrHash, config.get().drListHash);
                auto rf = cp::proto::MsgBuilder(MT::REJECT).str(std::string(buf)).build();
                cp::net::OutboundMsg ro;
                ro.target = msg.sender; ro.frame = std::move(rf);
                netThread.outTcp.push(std::move(ro));
                Log("CoPilots: rejected '%s' — dr hash mismatch client=0x%08X host=0x%08X",
                    nick.c_str(), clientDrHash, config.get().drListHash);
                break;
            }

            // Queue for host approval or accept immediately
            if (requireJoinApproval_) {
                pendingConns_.push_back({msg.sender, nick, clientDrHash});
                connWin.addPendingJoin(msg.sender, nick);
                Log("CoPilots: queued join request from '%s' (connId=%u)", nick.c_str(), msg.sender);
            } else {
                acceptJoin(msg.sender, nick);
            }
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
                // Adopt the host-assigned ID verbatim: after departures the roster
                // has ID gaps (IDs are never reused), so a local counter diverges.
                session.addParticipantWithId(pid, nick);
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

            // Request a snapshot of all shared notepad state from the host
            {
                auto snapReq = cp::proto::MsgBuilder(MT::NP_SNAP_REQ).build();
                cp::net::OutboundMsg sreq;
                sreq.target = 0;
                sreq.frame  = std::move(snapReq);
                netThread.outTcp.push(std::move(sreq));
            }
            // ... and of all shared chart tabs
            {
                auto snapReq = cp::proto::MsgBuilder(MT::CF_SNAP_REQ).build();
                cp::net::OutboundMsg sreq;
                sreq.target = 0;
                sreq.frame  = std::move(snapReq);
                netThread.outTcp.push(std::move(sreq));
            }
            break;
        }

        case MT::PARTICIPANT_JOIN: {
            cp::ParticipantId pid = r.u8();
            std::string nick = r.str();
            // Skip if we already know this participant (received in WELCOME)
            if (!session.find(pid))
                session.addParticipantWithId(pid, nick);
            // Re-broadcast our flight-plan inventory so the new joiner learns
            // about plans announced before it connected (host relays to all;
            // receivers request only what they are missing, so this is cheap).
            if (pid != session.myId())
                fmsSync.announce();
            break;
        }

        case MT::PARTICIPANT_UPDATE: {
            cp::ParticipantId pid = r.u8();
            std::string roleId = r.str();
            // Server notifying us of a role change — apply on client (host already applied locally)
            if (!session.isHost())
                session.setRole(pid, roleId, config.get().roles);
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
                case cp::DrType::DATA: {
                    uint16_t n = r.u16();
                    val.ba.resize(n);
                    for (auto& x : val.ba) x = r.u8();
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
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;  // don't echo back to the originator
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

        case MT::PAUSE_STATE: {
            uint8_t p = r.u8();
            if (drPaused_ && cmdPauseToggle_) {
                int cur = XPLMGetDatai(drPaused_);
                if ((cur != 0) != (p != 0)) {
                    expectedPaused_ = (p != 0) ? 1 : 0;
                    XPLMCommandOnce(cmdPauseToggle_);
                }
            }
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;
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

        case MT::FMS_REQUEST: {
            // The host serves requested files itself and relays only the names
            // it could not serve; otherwise every participant holding a file
            // would broadcast its own copy (N duplicate transfers).
            std::vector<std::string> unserved;
            fmsSync.onRequest(r, session.isHost() ? &unserved : nullptr);
            if (session.isHost() && !unserved.empty()) {
                auto b = cp::proto::MsgBuilder(MT::FMS_REQUEST)
                             .u16(static_cast<uint16_t>(unserved.size()));
                for (const auto& n : unserved) b.str(n);
                cp::net::OutboundMsg relay;
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;  // don't echo back to the originator
                relay.frame         = b.build();
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::FMS_LIST:
        case MT::FMS_FILE: {
            if (static_cast<MT>(msg.type) == MT::FMS_LIST)
                fmsSync.onList(r);
            else
                fmsSync.onFile(r);
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;  // don't echo back to the originator
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
            uint16_t idx   = r.u16();
            uint8_t  phase = r.u8();   // CMD_PHASE_*; reads 0 (ONCE) on legacy frames
            {
                uint8_t effectiveSender = msg.sender;
                auto it = connIdMap_.find(msg.sender);
                if (it != connIdMap_.end()) effectiveSender = it->second;
                syncEngine.applyIncomingCommand(idx, phase, effectiveSender);
            }
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;  // don't echo back to the originator
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

        case MT::CONTROL_REQUEST: {
            if (!session.isHost()) break;
            auto it = connIdMap_.find(msg.sender);
            if (it == connIdMap_.end()) break;
            cp::ParticipantId pid = it->second;
            const cp::Participant* p = session.find(pid);
            if (!p) break;
            if (requireControlApproval_) {
                connWin.setPendingControlRequest(pid, p->nick);
                Log("CoPilots: control request from '%s' (pid=%u)", p->nick.c_str(), pid);
            } else {
                // Auto-approve
                hostSetPhysicsMaster(pid);
                Log("CoPilots: auto-granted controls to '%s'", p->nick.c_str());
            }
            break;
        }

        case MT::CONTROL_DENY: {
            std::string reason = r.str();
            connWin.showControlDenied(reason);
            Log("CoPilots: controls denied — %s", reason.c_str());
            break;
        }

        case MT::PHYSICS_MASTER_SET: {
            cp::ParticipantId pid = r.u8();
            session.setPhysicsMaster(pid);
            physicsSync.onMasterChanged();
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

        case MT::WEATHER_METAR: {
            weatherSync.onMetarChunk(r);
            if (session.isHost()) {
                cp::net::OutboundMsg relay;
                relay.target        = 0xFF;
                relay.excludeTarget = msg.sender;
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

        case MT::REJECT: {
            std::string reason = r.str();
            Log("CoPilots: connection rejected by host: %s", reason.c_str());
            onDisconnect();
            connWin.setState(cp::ui::ConnState::CONNECT_ERROR, "Rejected: " + reason);
            break;
        }

        case MT::RESYNC_REQUEST: {
            // No longer acted upon — full resyncs cause switch snap-back (host values
            // overwrite client-set values).  Message type kept for protocol compatibility.
            break;
        }

        // ── Notepad messages ──────────────────────────────────────────────────
        case MT::NP_TAB_SHARE: {
            using namespace cp::notepad;
            NpId tabId      = r.u32();
            std::string name = r.str();
            notepadWin.onTabShare(tabId, name);
            if (session.isHost()) {
                sharedNotepad_.ensureSharedTab(tabId, name);
                // relay
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_SHEET_NEW: {
            using namespace cp::notepad;
            NpId tabId   = r.u32();
            NpId sheetId = r.u32();
            float w = r.f32(), h = r.f32();
            notepadWin.onSheetNew(tabId, sheetId, w, h);
            if (session.isHost()) {
                Tab* tab = sharedNotepad_.findTab(tabId);
                if (tab && !tab->findSheet(sheetId)) {
                    Sheet s; s.id=sheetId; s.w=w; s.h=h;
                    tab->sheets.push_back(std::move(s));
                }
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_STROKE_ADD: {
            using namespace cp::notepad;
            NpId tabId   = r.u32();
            NpId sheetId = r.u32();
            Stroke stroke;
            stroke.id        = r.u32();
            stroke.tool      = static_cast<Tool>(r.u8());
            stroke.colorRGBA = r.u32();
            stroke.thickness = r.f32();
            uint16_t n       = r.u16();
            stroke.pts.reserve(n);
            for (uint16_t i = 0; i < n; ++i) {
                float px = r.f32(), py = r.f32();
                stroke.pts.push_back({px, py});
            }
            notepadWin.onStrokeAdd(tabId, sheetId, stroke);
            if (session.isHost()) {
                Sheet* s = sharedNotepad_.findSheet(tabId, sheetId);
                if (s) s->applyStroke(stroke);
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_SHEET_DEL: {
            using namespace cp::notepad;
            NpId tabId   = r.u32();
            NpId sheetId = r.u32();
            notepadWin.onSheetDel(tabId, sheetId);
            if (session.isHost()) {
                Tab* tab = sharedNotepad_.findTab(tabId);
                if (tab) {
                    tab->sheets.erase(
                        std::remove_if(tab->sheets.begin(), tab->sheets.end(),
                            [sheetId](const Sheet& s){ return s.id == sheetId; }),
                        tab->sheets.end());
                }
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_STROKE_DEL: {
            // Smart eraser: one stroke deleted by ID on the originating participant.
            // Host removes it from the authoritative copy and relays to all others.
            using namespace cp::notepad;
            NpId tabId    = r.u32();
            NpId sheetId  = r.u32();
            NpId strokeId = r.u32();
            notepadWin.onStrokeDel(tabId, sheetId, strokeId);
            if (session.isHost()) {
                Sheet* s = sharedNotepad_.findSheet(tabId, sheetId);
                if (s) s->removeStroke(strokeId);
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_SHEET_PARAM: {
            using namespace cp::notepad;
            NpId tabId   = r.u32();
            NpId sheetId = r.u32();
            float w = r.f32(), h = r.f32();
            notepadWin.onSheetParam(tabId, sheetId, w, h);
            if (session.isHost()) {
                Sheet* s = sharedNotepad_.findSheet(tabId, sheetId);
                if (s) { s->w = w; s->h = h; }
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_TAB_DEL: {
            using namespace cp::notepad;
            NpId tabId = r.u32();
            notepadWin.onTabDel(tabId);
            if (session.isHost()) {
                sharedNotepad_.tabs.erase(
                    std::remove_if(sharedNotepad_.tabs.begin(), sharedNotepad_.tabs.end(),
                                   [tabId](const Tab& t){ return t.id == tabId; }),
                    sharedNotepad_.tabs.end());
                cp::net::OutboundMsg relay;
                relay.target = 0xFF;
                relay.frame.resize(5 + msg.payload.size());
                relay.frame[0] = msg.type;
                uint32_t plen = static_cast<uint32_t>(msg.payload.size());
                relay.frame[1]=plen&0xFF; relay.frame[2]=(plen>>8)&0xFF;
                relay.frame[3]=(plen>>16)&0xFF; relay.frame[4]=(plen>>24)&0xFF;
                std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin()+5);
                netThread.outTcp.push(std::move(relay));
            }
            break;
        }

        case MT::NP_SNAP_REQ: {
            // Only the host answers snapshot requests.
            if (!session.isHost()) break;
            // Find the requesting client's connId (msg.sender on host = connId).
            uint8_t target = msg.sender;

            // Stream each shared tab/sheet to the requester in chunks (~900 KiB each).
            for (const auto& tab : sharedNotepad_.tabs) {
                if (!tab.shared) continue;
                for (const auto& sheet : tab.sheets) {
                    // Pack strokes into chunks.
                    size_t idx = 0;
                    bool firstChunk = true;
                    while (idx <= sheet.strokes.size()) {
                        // Estimate payload size; flush when close to 900 KiB or at end.
                        auto b = cp::proto::MsgBuilder(MT::NP_SNAP_SHEET)
                                 .u32(tab.id)
                                 .str(tab.name)
                                 .u32(sheet.id)
                                 .f32(sheet.w)
                                 .f32(sheet.h)
                                 .u8(firstChunk ? 1 : 0);

                        // Count how many strokes fit in ~900 KiB.
                        size_t chunkStart = idx;
                        size_t approxBytes = 20; // header overhead
                        uint16_t cnt = 0;
                        while (idx < sheet.strokes.size()) {
                            const auto& stroke = sheet.strokes[idx];
                            size_t strokeBytes = 4+4+4+1+4+4+2 + stroke.pts.size()*8;
                            if (approxBytes + strokeBytes > 900*1024) break;
                            approxBytes += strokeBytes;
                            ++cnt;
                            ++idx;
                        }

                        b.u16(cnt);
                        for (size_t si = chunkStart; si < chunkStart + cnt; ++si) {
                            const auto& stroke = sheet.strokes[si];
                            b.u32(stroke.id)
                             .u8(static_cast<uint8_t>(stroke.tool))
                             .u32(stroke.colorRGBA)
                             .f32(stroke.thickness)
                             .u16(static_cast<uint16_t>(stroke.pts.size()));
                            for (const auto& p : stroke.pts) b.f32(p.x).f32(p.y);
                        }

                        cp::net::OutboundMsg out;
                        out.target = target;
                        out.frame  = b.build();
                        netThread.outTcp.push(std::move(out));

                        firstChunk = false;
                        if (idx >= sheet.strokes.size()) break;
                    }
                }
            }

            // Signal end of snapshot
            {
                auto end = cp::proto::MsgBuilder(MT::NP_SNAP_END).build();
                cp::net::OutboundMsg out;
                out.target = target;
                out.frame  = std::move(end);
                netThread.outTcp.push(std::move(out));
            }
            break;
        }

        case MT::NP_SNAP_SHEET: {
            // Received by clients during snapshot.
            using namespace cp::notepad;
            NpId tabId    = r.u32();
            std::string tabName = r.str();
            NpId sheetId  = r.u32();
            float w = r.f32(), h = r.f32();
            bool isFirst  = (r.u8() != 0);
            uint16_t cnt  = r.u16();
            std::vector<Stroke> strokes;
            strokes.reserve(cnt);
            for (uint16_t i = 0; i < cnt; ++i) {
                Stroke s;
                s.id        = r.u32();
                s.tool      = static_cast<Tool>(r.u8());
                s.colorRGBA = r.u32();
                s.thickness = r.f32();
                uint16_t np = r.u16();
                s.pts.reserve(np);
                for (uint16_t j = 0; j < np; ++j) {
                    float px = r.f32(), py = r.f32();
                    s.pts.push_back({px, py});
                }
                strokes.push_back(std::move(s));
            }
            notepadWin.onSnapSheet(tabId, tabName, sheetId, w, h, isFirst, strokes);
            break;
        }

        case MT::NP_SNAP_END:
            notepadWin.onSnapEnd();
            break;

        // ── ChartFox chart-tab messages ───────────────────────────────────────
        case MT::CF_TAB_SHARE: {
            using namespace cp::chart;
            NpId tabId       = r.u32();
            std::string icao = r.str();
            std::string name = r.str();
            chartsWin.onTabShare(tabId, icao, name);
            if (session.isHost()) {
                sharedCharts_.ensureSharedTab(tabId, icao, name);
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_TAB_SET_AIRPORT: {
            using namespace cp::chart;
            NpId tabId       = r.u32();
            std::string icao = r.str();
            chartsWin.onTabSetAirport(tabId, icao);
            if (session.isHost()) {
                ChartTab* tab = sharedCharts_.findTab(tabId);
                if (tab && tab->icao != icao) {
                    tab->icao = icao;
                    tab->name = icao;
                    tab->activeChartId.clear();
                }
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_TAB_SET_CHART: {
            using namespace cp::chart;
            NpId tabId          = r.u32();
            std::string chartId = r.str();
            chartsWin.onTabSetChart(tabId, chartId);
            if (session.isHost()) {
                ChartTab* tab = sharedCharts_.findTab(tabId);
                if (tab) tab->activeChartId = chartId;
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_STROKE_ADD: {
            using namespace cp::chart;
            NpId tabId          = r.u32();
            std::string chartId = r.str();
            uint8_t page        = r.u8();
            Stroke stroke;
            stroke.id        = r.u32();
            stroke.tool      = static_cast<cp::notepad::Tool>(r.u8());
            stroke.colorRGBA = r.u32();
            stroke.thickness = r.f32();
            uint16_t n       = r.u16();
            stroke.pts.reserve(n);
            for (uint16_t i = 0; i < n; ++i) {
                float px = r.f32(), py = r.f32();
                stroke.pts.push_back({px, py});
            }
            chartsWin.onStrokeAdd(tabId, chartId, page, stroke);
            if (session.isHost()) {
                ChartTab* tab = sharedCharts_.findTab(tabId);
                if (tab) tab->annotsFor(chartId, page).applyStroke(stroke);
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_STROKE_DEL: {
            using namespace cp::chart;
            NpId tabId          = r.u32();
            std::string chartId = r.str();
            uint8_t page        = r.u8();
            NpId strokeId       = r.u32();
            chartsWin.onStrokeDel(tabId, chartId, page, strokeId);
            if (session.isHost()) {
                ChartTab* tab = sharedCharts_.findTab(tabId);
                if (tab) {
                    ChartAnnots* a = tab->findAnnots(chartId, page);
                    if (a) a->removeStroke(strokeId);
                }
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_CAL_SET: {
            using namespace cp::chart;
            std::string chartId = r.str();
            uint8_t page1       = r.u8();
            bool clear          = r.u8() != 0;
            std::string icao    = r.str();
            bool isOverride     = r.u8() != 0;
            ChartGeoref g;
            g.k              = r.f64();
            g.tx             = r.f64();
            g.ty             = r.f64();
            g.transformAngle = r.f64();
            g.page           = page1;
            chartsWin.onCalSet(chartId, page1, clear, icao, isOverride, g);
            if (session.isHost()) relayRaw(msg);
            break;
        }

        case MT::CF_TAB_DEL: {
            using namespace cp::chart;
            NpId tabId = r.u32();
            chartsWin.onTabDel(tabId);
            if (session.isHost()) {
                sharedCharts_.tabs.erase(
                    std::remove_if(sharedCharts_.tabs.begin(), sharedCharts_.tabs.end(),
                                   [tabId](const ChartTab& t){ return t.id == tabId; }),
                    sharedCharts_.tabs.end());
                relayRaw(msg);
            }
            break;
        }

        case MT::CF_SNAP_REQ: {
            // Only the host answers; stream every shared tab to the requester.
            if (!session.isHost()) break;
            uint8_t target = msg.sender;

            for (const auto& tab : sharedCharts_.tabs) {
                if (!tab.shared) continue;

                if (tab.annots.empty()) {
                    // Tab-only chunk: no annotations yet, but the joiner still
                    // needs the tab, its airport and the selected chart.
                    auto b = cp::proto::MsgBuilder(MT::CF_SNAP_TAB)
                             .u32(tab.id).str(tab.icao).str(tab.name)
                             .str(tab.activeChartId)
                             .str("")            // no annot set in this chunk
                             .u8(0).u8(1).u16(0);
                    cp::net::OutboundMsg out;
                    out.target = target;
                    out.frame  = b.build();
                    netThread.outTcp.push(std::move(out));
                    continue;
                }

                for (const auto& [key, annots] : tab.annots) {
                    size_t idx = 0;
                    bool firstChunk = true;
                    while (idx <= annots.strokes.size()) {
                        auto b = cp::proto::MsgBuilder(MT::CF_SNAP_TAB)
                                 .u32(tab.id).str(tab.icao).str(tab.name)
                                 .str(tab.activeChartId)
                                 .str(key.first)     // chartId of this annot set
                                 .u8(key.second)     // page
                                 .u8(firstChunk ? 1 : 0);

                        // Pack strokes into ~900 KiB chunks (NP_SNAP pattern).
                        size_t chunkStart  = idx;
                        size_t approxBytes = 64;
                        uint16_t cnt = 0;
                        while (idx < annots.strokes.size()) {
                            const auto& stroke = annots.strokes[idx];
                            size_t strokeBytes = 4+1+4+4+2 + stroke.pts.size()*8;
                            if (approxBytes + strokeBytes > 900*1024) break;
                            approxBytes += strokeBytes;
                            ++cnt;
                            ++idx;
                        }

                        b.u16(cnt);
                        for (size_t si = chunkStart; si < chunkStart + cnt; ++si) {
                            const auto& stroke = annots.strokes[si];
                            b.u32(stroke.id)
                             .u8(static_cast<uint8_t>(stroke.tool))
                             .u32(stroke.colorRGBA)
                             .f32(stroke.thickness)
                             .u16(static_cast<uint16_t>(stroke.pts.size()));
                            for (const auto& p : stroke.pts) b.f32(p.x).f32(p.y);
                        }

                        cp::net::OutboundMsg out;
                        out.target = target;
                        out.frame  = b.build();
                        netThread.outTcp.push(std::move(out));

                        firstChunk = false;
                        if (idx >= annots.strokes.size()) break;
                    }
                }
            }

            // Calibrations are chart-scoped, not tab-scoped: send them all, so a
            // joiner sees the aircraft symbol on charts the crew pinned by hand.
            const auto& cals = chartsWin.calibrations();
            if (!cals.empty()) {
                auto b = cp::proto::MsgBuilder(MT::CF_SNAP_CAL);
                b.u16(static_cast<uint16_t>(cals.size()));
                for (const auto& [key, c] : cals) {
                    std::string chartId;
                    int page1 = 0;
                    if (!cp::chart::ChartCalStore::splitKey(key, chartId, page1)) continue;
                    b.str(chartId).u8(static_cast<uint8_t>(page1))
                     .str(c.icao).u8(c.isOverride ? 1 : 0)
                     .f64(c.g.k).f64(c.g.tx).f64(c.g.ty).f64(c.g.transformAngle);
                }
                cp::net::OutboundMsg out;
                out.target = target;
                out.frame  = b.build();
                netThread.outTcp.push(std::move(out));
            }

            {
                auto end = cp::proto::MsgBuilder(MT::CF_SNAP_END).build();
                cp::net::OutboundMsg out;
                out.target = target;
                out.frame  = std::move(end);
                netThread.outTcp.push(std::move(out));
            }
            break;
        }

        case MT::CF_SNAP_TAB: {
            using namespace cp::chart;
            NpId tabId           = r.u32();
            std::string icao     = r.str();
            std::string name     = r.str();
            std::string activeId = r.str();
            std::string chartId  = r.str();
            uint8_t page         = r.u8();
            bool isFirst         = (r.u8() != 0);
            uint16_t cnt         = r.u16();
            std::vector<Stroke> strokes;
            strokes.reserve(cnt);
            for (uint16_t i = 0; i < cnt; ++i) {
                Stroke s;
                s.id        = r.u32();
                s.tool      = static_cast<cp::notepad::Tool>(r.u8());
                s.colorRGBA = r.u32();
                s.thickness = r.f32();
                uint16_t np = r.u16();
                s.pts.reserve(np);
                for (uint16_t j = 0; j < np; ++j) {
                    float px = r.f32(), py = r.f32();
                    s.pts.push_back({px, py});
                }
                strokes.push_back(std::move(s));
            }
            chartsWin.onSnapTab(tabId, icao, name, activeId, chartId, page,
                                isFirst, strokes);
            break;
        }

        case MT::CF_SNAP_CAL: {
            using namespace cp::chart;
            uint16_t count = r.u16();
            for (uint16_t i = 0; i < count; ++i) {
                std::string chartId = r.str();
                uint8_t page1       = r.u8();
                std::string icao    = r.str();
                bool isOverride     = r.u8() != 0;
                ChartGeoref g;
                g.k              = r.f64();
                g.tx             = r.f64();
                g.ty             = r.f64();
                g.transformAngle = r.f64();
                g.page           = page1;
                chartsWin.onCalSet(chartId, page1, false, icao, isOverride, g);
            }
            break;
        }

        case MT::CF_SNAP_END:
            chartsWin.onSnapEnd();
            break;

        case MT::HEARTBEAT:
            break;

        default:
            LogWarning("Unhandled TCP message type 0x%02X", msg.type);
            break;
        }
    }

    // Host: re-frame an inbound message verbatim and broadcast it to all
    // clients (originator excluded — it already applied the change locally).
    void relayRaw(const cp::net::InboundMsg& msg)
    {
        cp::net::OutboundMsg relay;
        relay.target        = 0xFF;
        relay.excludeTarget = msg.sender;
        relay.frame.resize(5 + msg.payload.size());
        relay.frame[0] = msg.type;
        uint32_t plen = static_cast<uint32_t>(msg.payload.size());
        relay.frame[1] = plen & 0xFF;
        relay.frame[2] = (plen >> 8)  & 0xFF;
        relay.frame[3] = (plen >> 16) & 0xFF;
        relay.frame[4] = (plen >> 24) & 0xFF;
        std::copy(msg.payload.begin(), msg.payload.end(), relay.frame.begin() + 5);
        netThread.outTcp.push(std::move(relay));
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

        // Also tell clients about each participant's current role (so their UI stays in sync)
        for (const auto& p : session.participants()) {
            auto pf = cp::proto::MsgBuilder(cp::proto::MsgType::PARTICIPANT_UPDATE)
                      .u8(p.id).str(p.roleId).build();
            cp::net::OutboundMsg pout;
            pout.target = 0xFF;
            pout.frame  = std::move(pf);
            netThread.outTcp.push(std::move(pout));
        }
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
            case cp::DrType::DATA:
                b.u16(static_cast<uint16_t>(val.ba.size()));
                for (uint8_t x : val.ba) b.u8(x);
                break;
            default: break;
        }
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        netThread.outTcp.push(std::move(out));
    }

    void sendCommandFire(uint16_t idx, uint8_t phase)
    {
        auto b = cp::proto::MsgBuilder(cp::proto::MsgType::COMMAND_FIRE).u16(idx).u8(phase);
        cp::net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = b.build();
        netThread.outTcp.push(std::move(out));
    }

    // Persist the connection form on every Host/Join click (not on success):
    // after a sim restart the form must be prefilled even if the attempt failed.
    void savePrefs(const cp::ui::ConnectionConfig& cfg)
    {
        cp::Prefs p;
        // Keep whichever fields this click did not touch from the stored file.
        cp::LoadPrefs(xpSystemPath_, p);
        p.nick        = cfg.nick;
        p.password    = cfg.password;
        p.lastWasHost = cfg.asHost;
        if (cfg.asHost) {
            p.hostPort = cfg.port;
            p.bindIp   = cfg.bindIp;
            p.requireJoinApproval    = cfg.requireJoinApproval;
            p.requireControlApproval = cfg.requireControlApproval;
        } else {
            p.address = cfg.host + ":" + std::to_string(cfg.port);
        }
        cp::SavePrefs(xpSystemPath_, p);
    }

    void onHost(const cp::ui::ConnectionConfig& cfg)
    {
        savePrefs(cfg);
        onDisconnect();
        connWin.setState(cp::ui::ConnState::CONNECTING);
        lobbyPassword_          = cfg.password;
        requireJoinApproval_    = cfg.requireJoinApproval;
        requireControlApproval_ = cfg.requireControlApproval;

        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);
        {
            char xpPath[512] = {};
            XPLMGetSystemPath(xpPath);
            config.applyAutoSync(xpPath);
        }
        connWin.setData(&session, &config.get());

        registry.build(config.get().datarefs, config.get().commands,
                       [this](uint16_t idx, uint8_t phase) { syncEngine.notifyCommandFired(idx, phase); });
        Log("onHost: registry built (cfg=%zu reg=%zu cmds=%zu), initialising engines",
            config.get().datarefs.size(), registry.datarefs().size(),
            registry.commands().size());
        syncEngine.init(&registry, &session);
        Log("onHost: syncEngine ready");
        syncEngine.setSmartCopilotMode(config.get().fromSmartCopilot);
        physicsSync.init(&session, &netThread);
        Log("onHost: physicsSync ready");
        weatherSync.init(&session, &netThread, xpSystemPath_);
        Log("onHost: weatherSync ready");

        session.setIsHost(true);
        cp::ParticipantId myId = session.addParticipant(cfg.nick);
        session.setMyId(myId);

        Log("onHost: starting server");
        netThread.startServer(cfg.port, cfg.port, cfg.bindIp);

        std::vector<std::string> allIps;
        if (!cfg.bindIp.empty()) {
            // Sockets are bound to one specific interface — only that address
            // can accept connections, so show it alone.
            allIps.push_back(cfg.bindIp);
        } else {
            allIps = cp::net::ListLocalIPv4();
        }
        if (allIps.empty()) allIps.push_back("127.0.0.1");
        connWin.setIsHost(true);
        connWin.setLocalEndpoint(allIps, cfg.port);
        connWin.setState(cp::ui::ConnState::CONNECTED);
        Log("Hosting on port %u  (IPs: %zu found)", cfg.port, allIps.size());
    }

    void onJoin(const cp::ui::ConnectionConfig& cfg)
    {
        savePrefs(cfg);
        onDisconnect();
        connWin.setState(cp::ui::ConnState::CONNECTING);

        char aircraftPath[512]; char aircraftFile[256];
        XPLMGetNthAircraftModel(0, aircraftFile, aircraftPath);
        std::string dir = aircraftPath;
        size_t sep = dir.find_last_of("/\\");
        if (sep != std::string::npos) dir = dir.substr(0, sep);
        config.load(dir);
        {
            char xpPath[512] = {};
            XPLMGetSystemPath(xpPath);
            config.applyAutoSync(xpPath);
        }

        registry.build(config.get().datarefs, config.get().commands,
                       [this](uint16_t idx, uint8_t phase) { syncEngine.notifyCommandFired(idx, phase); });
        Log("onJoin: registry built, initialising engines");
        syncEngine.init(&registry, &session);
        Log("onJoin: syncEngine ready");
        syncEngine.setSmartCopilotMode(config.get().fromSmartCopilot);
        physicsSync.init(&session, &netThread);
        Log("onJoin: physicsSync ready");
        weatherSync.init(&session, &netThread, xpSystemPath_);
        Log("onJoin: weatherSync ready, connecting");

        netThread.startClient(cfg.host, cfg.port, cfg.port);

        auto frame = cp::proto::MsgBuilder(cp::proto::MsgType::HELLO)
                     .u8(cp::proto::PROTOCOL_VERSION)
                     .str(cfg.nick)
                     .u32(config.get().drListHash)
                     .str(cfg.password).build();
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
        pointerSync.reset();
        weatherSync.reset();
        registry.clear();
        config.reset();
        connIdMap_.clear();
        connRemoteIp_.clear();
        pendingConns_.clear();
        pendingPhysRestore_ = {};
        lobbyPassword_.clear();
        sharedNotepad_.clear();
        notepadWin.resetShared();
        sharedCharts_.clear();
        chartsWin.resetShared();
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
            {
                char xpPath[512] = {};
                XPLMGetSystemPath(xpPath);
                config.applyAutoSync(xpPath);
            }
            registry.build(config.get().datarefs, config.get().commands,
                           [this](uint16_t idx, uint8_t phase) { syncEngine.notifyCommandFired(idx, phase); });
            syncEngine.reset();
        }
    }

    void onMenuClick(int item)
    {
        if (item == 0)
            connWin.setVisible(!connWin.visible());
        else if (item == 1)
            statusHud.setVisible(!statusHud.visible());
        else if (item == 2)
            notepadWin.setVisible(!notepadWin.visible());
        else if (item == 3)
            chartsWin.setVisible(!chartsWin.visible());
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
    XPLMAppendMenuItem(g_plugin->menuId, "Notepad",        reinterpret_cast<void*>(2), 1);
    XPLMAppendMenuItem(g_plugin->menuId, "Charts",         reinterpret_cast<void*>(3), 1);

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
