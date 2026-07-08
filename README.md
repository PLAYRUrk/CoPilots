# CoPilots — Multi-Crew Shared Cockpit for X-Plane 11

Fly a single cockpit with multiple people simultaneously. Each pilot controls only the zones assigned to them; everything else is read-only. The host manages crew assignments live from the lobby.

---

## Requirements

- **X-Plane 11** (Windows)
- The plugin file `win.xpl` from the latest release

---

## Installation

1. Download the latest release archive.
2. Extract the `CoPilots` folder.
3. Copy it into your X-Plane plugins directory:

```
X-Plane 11\Resources\plugins\CoPilots\
```

The result should look like:

```
plugins\
  CoPilots\
    64\
      win.xpl
    configs\
      copilots.example.json
```

4. Start X-Plane. The plugin appears under **Plugins → CoPilots**.

---

## Hosting a session

1. Load your aircraft.
2. Open **Plugins → CoPilots → Connect / Host**.
3. Enter your nickname, set a port (default: **56900**), click **Start Hosting**.
4. Share your **IP:Port** with your crew (the address is shown with a Copy button).
5. The lobby opens automatically. Use it to assign roles and zones to each crew member.

> **Firewall:** allow X-Plane through Windows Firewall on TCP and UDP ports 56900 and 56901.

---

## Joining a session

1. Open **Plugins → CoPilots → Connect / Host**.
2. Enter your nickname and the host's **IP:Port**, click **Join**.

---

## Aircraft configuration

Place a `copilots.json` file in your aircraft's folder to define zones, roles, and which datarefs belong to which zone.

**Example** (`copilots.example.json` is included in the `configs` folder):

```json
{
  "version": 1,
  "aircraft": "Boeing 737-800",
  "port": 56900,
  "zones": [
    { "id": "MCP", "name": "Mode Control Panel" },
    { "id": "OVERHEAD", "name": "Overhead Panel" }
  ],
  "roles": [
    { "id": "captain", "name": "Captain (PF)", "zones": ["MCP"] },
    { "id": "fo", "name": "First Officer (PM)", "zones": ["OVERHEAD"] }
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
- `onchange` — sent when value changes (switches, buttons)
- `continuous` — sent every tick at ~20 Hz (knobs, analog values)

**No config file?** CoPilots automatically falls back to `smartcopilot.cfg` if present. Basic sync works; zone and role features are unavailable.

---

## Status HUD

A small overlay appears in the bottom-right corner showing your role, zones, ping, and crew count. Toggle it via **Plugins → CoPilots → Toggle HUD**.

---

## Troubleshooting

| Problem | What to check |
|---|---|
| Plugin not in menu | Look for `[CoPilots]` lines in X-Plane's `Log.txt` |
| Can't connect | Firewall — allow X-Plane on TCP/UDP 56900–56901. Confirm the host IP is reachable. |
| Datarefs not syncing | Check `path` values in `copilots.json` match your aircraft's actual datarefs |
| Physics jitter | High ping (>150 ms). Ask the host to reassign Physics Master to the lowest-latency pilot. |

---

## Default ports

| Protocol | Port |
|---|---|
| TCP (lobby + state) | 56900 |
| UDP (physics) | 56901 |

---

## License

Copyright (c) 2026 Egor Lebedev. All Rights Reserved.

This software is proprietary. No permission is granted to use, copy, modify, or distribute it without explicit written consent from the author. See [LICENSE](LICENSE) for details.
