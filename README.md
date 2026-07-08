# CoPilots — Multi-Crew Shared Cockpit for X-Plane 11

**CoPilots** lets an unlimited number of pilots share a single cockpit in X-Plane 11.
Each pilot gets a **role** and **zones** they control; every other position is read-only.
The session admin (host) manages assignments live from the lobby panel.

---

## Features (MVP)

| Feature | Status |
|---|---|
| Unlimited crew members | ✅ |
| TCP (reliable state) + UDP (high-freq physics) | ✅ |
| Zone-based authority (each zone has exactly one owner) | ✅ |
| Roles (Captain, FO, FE, Observer …) | ✅ |
| Admin lobby: assign roles/zones, kick, set physics master | ✅ |
| Dear ImGui modern dark UI | ✅ |
| copilots.json aircraft config | ✅ |
| smartcopilot.cfg fallback (basic sync, no zones) | ✅ |

---

## Prerequisites

### 1 — Visual Studio 2022 (Windows)

Download [Visual Studio 2022 Community](https://visualstudio.microsoft.com/vs/community/) (free).
During installation select workload:
> **Desktop development with C++**

This installs MSVC compiler, CMake, and Windows SDK automatically.

### 2 — CMake 3.15+

CMake is bundled with Visual Studio. Alternatively install from [cmake.org](https://cmake.org/download/).

Verify: open **Developer Command Prompt** (search in Start) and run:
```
cmake --version
```

### 3 — X-Plane 11 SDK

1. Go to [developer.x-plane.com/sdk/plugin-sdk-downloads/](https://developer.x-plane.com/sdk/plugin-sdk-downloads/)
2. Download **SDK 3.0.3** (the XPLM SDK for XP11)
3. Extract the zip so that the structure looks like:

```
CoPilots/
  third_party/
    SDK/
      CHeaders/
        XPLM/
          XPLMPlugin.h   ← must exist
          ...
        XPWidgets/
          ...
      Libraries/
        Win/
          XPLM_64.lib
          XPWidgets_64.lib
```

---

## Build (Windows)

Open **Developer Command Prompt for VS 2022** (or any terminal where `cmake` is on PATH):

```cmd
cd C:\Users\egorb\Documents\GitHub\CoPilots

:: Configure (downloads ImGui + nlohmann/json automatically)
cmake -B build -A x64

:: Build Release
cmake --build build --config Release
```

After building, the plugin is staged to:
```
build\install\CoPilots\
  64\
    win.xpl        ← the plugin
  configs\
    copilots.example.json
```

---

## Install

Copy the `CoPilots` folder into X-Plane's plugin directory:

```
X-Plane 11\
  Resources\
    plugins\
      CoPilots\          ← paste here
        64\
          win.xpl
        configs\
          copilots.example.json
```

---

## Aircraft Configuration

### New format — `copilots.json`

Place `copilots.json` in your aircraft's folder
(e.g. `X-Plane 11/Aircraft/Extra Aircraft/B737-800/copilots.json`).

See `configs/copilots.example.json` for a full example.

Key sections:

```jsonc
{
  "version": 1,
  "aircraft": "Boeing 737-800",
  "port": 56900,

  "zones": [
    { "id": "MCP", "name": "Mode Control Panel" },
    { "id": "OVERHEAD", "name": "Overhead Panel" }
    // ...
  ],

  "roles": [
    { "id": "captain", "name": "Captain (PF)", "zones": ["MCP", "EFIS_CAPT"] },
    { "id": "first_officer", "name": "First Officer (PM)", "zones": ["FMC", "EFIS_FO"] }
  ],

  "datarefs": [
    { "path": "sim/cockpit/autopilot/heading_mag", "zone": "MCP", "type": "float", "mode": "continuous" }
  ],

  "commands": [
    { "path": "sim/autopilot/NAV", "zone": "MCP" }
  ]
}
```

**Sync modes:**
- `"onchange"` — sent only when value changes (toggles, mode selectors)
- `"continuous"` — sent every tick (~20 Hz) regardless (knobs, analog values)

### Fallback — `smartcopilot.cfg`

If `copilots.json` is absent, CoPilots parses `smartcopilot.cfg` automatically.
All datarefs go into a single `SHARED` zone; zone/role features are unavailable.
The UI shows a warning banner in the lobby.

---

## Usage

1. Start X-Plane 11 with the aircraft loaded.
2. Go to **Plugins → CoPilots → Connect / Host**.
3. **Host:** Enter your nickname → click **Start Hosting**. Share your IP with others.
4. **Join:** Enter the host's IP, port, your nickname → click **Join**.
5. **Admin (host):** Open **Plugins → CoPilots → Lobby (Admin Panel)**.
   - Assign roles via dropdown.
   - Click **Edit** to customise individual zone assignments.
   - Click **Physics** to transfer physics master to another pilot.
   - Click **Kick** to remove a participant.

### Default port

TCP: **56900**, UDP: **56901**

> Make sure X-Plane is allowed through Windows Firewall on these ports if hosting.

---

## Network topology

```
[Captain PC]  ──TCP+UDP──┐
[FO PC]       ──TCP+UDP──┤── [Host/Server PC + X-Plane]
[FE PC]       ──TCP+UDP──┘
```

- **TCP**: lobby state, dataref changes, commands (guaranteed delivery)
- **UDP**: physics position/attitude/velocities from physics master (~20 Hz, low latency)
- The **physics master** is the sole source of aircraft position (default = host). Can be reassigned.
- Each participant's zone controls are **authoritative** — others receive the values and apply them locally.

---

## Troubleshooting

| Problem | Solution |
|---|---|
| Plugin not visible in menu | Check `Log.txt` for `[CoPilots]` lines. Ensure SDK `Libraries/Win/*.lib` exist. |
| `XPLM_64.lib` not found | Re-extract SDK, verify `third_party/SDK/Libraries/Win/` exists. |
| Can't connect | Firewall: allow X-Plane on TCP/UDP 56900-56901. Check host IP is correct. |
| Datarefs not syncing | Verify `copilots.json` has correct `path` values for your aircraft. |
| Physics jitter | Normal at high ping (>150 ms). Reassign physics master to lowest-latency participant. |

---

## Architecture Overview

```
plugin.cpp               ← XPlugin* entry points, main loop, message routing
├── config/Config.cpp    ← loads copilots.json or smartcopilot.cfg
├── net/Transport.cpp    ← cross-platform Winsock/BSD sockets (TCP+UDP)
├── net/NetThread.cpp    ← background I/O thread, safe queues
├── session/Session.cpp  ← lobby state: participants, roles, zone→owner map
├── sync/SyncEngine.cpp  ← dataref change detection, apply, echo-suppression
├── sync/PhysicsSync.cpp ← UDP physics state send/receive/apply
└── ui/
    ├── ImguiBackend.cpp ← Dear ImGui ↔ X-Plane draw/input integration
    ├── Theme.cpp        ← dark modern colour theme
    ├── ConnectionWindow.cpp
    ├── LobbyWindow.cpp
    └── StatusHud.cpp
```

---

## License

MIT — free to use, modify, and distribute.
