# CoPilots — Multi-Crew Shared Cockpit for X-Plane 11

Fly a single aircraft with multiple people simultaneously. One pilot has **flight control** at a time; all other crew members see the cockpit in real time and can control only the systems assigned to them. The host manages crew assignments live from the lobby.

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

## Flight controls and physics master

Only **one pilot** controls the aircraft at a time — the **physics master**. By default this is the host. When someone has control:

- Their hardware joystick and mouse yoke move the aircraft.
- All other crew members' joystick and mouse input is **completely blocked** — neither hardware joystick nor mouse can affect the aircraft while someone else is flying.
- The active pilot's control positions (yoke, rudder, throttle, reverser, flaps, brakes, etc.) are streamed to all clients at ~60 Hz over UDP.

**Requesting control (client):** Click **Request Controls** in the plugin window. The host receives a notification and can Grant or Deny.

**Transferring control (host):** In the lobby table, click **Phys** next to any participant to make them the physics master. You can also click **Take Controls** to reclaim control for yourself.

---

## Aircraft configuration

### Ready-made configs (recommended)

For complex aircraft (Zibo/LevelUp, IXEG, Colimata, etc.) use the ready-made configs from the [`configs/`](configs/) folder of this repository — it also contains the table of supported aircraft and their test status.

The easiest way: load your aircraft, open **Plugins → CoPilots → Connect / Host** and click **Download config for current aircraft** — the plugin fetches the matching config from this repository and installs it into the aircraft folder automatically (a `.bak` backup of any existing `copilots.json` is kept). Every crew member should do this — the files must be identical.

Manual alternative: copy the aircraft's `copilots.json` from [`configs/`](configs/) into its folder (next to the `.acf`) on every crew member's machine.

Your aircraft is not on the list? [`configs/README.md`](configs/README.md) describes the rules for building a config (usually generated from a community `smartcopilot.cfg`) — PRs are welcome.

### Without a config

CoPilots works **without any configuration file** — it automatically discovers and synchronises hundreds of cockpit datarefs from X-Plane's built-in `DataRefs.txt` (autopilot, switches, GPS, engine actuators, etc.). This is enough for aircraft with standard systems; on aircraft with custom logic (SASL/Gizmo) the custom switches will not sync without a config.

### Custom config

For finer control — custom zones, roles, and specific dataref assignments — place a `copilots.json` file in your aircraft's folder.

**Example** (`copilots.example.json` is included in the `configs` folder):

```json
{
  "aircraft": "Boeing 737-800",
  "port": 56900,
  "zones": [
    { "id": "MCP", "name": "Mode Control Panel" },
    { "id": "OVERHEAD", "name": "Overhead Panel" }
  ],
  "roles": [
    { "id": "captain", "name": "Captain (PF)", "zones": ["MCP"] },
    { "id": "fo",      "name": "First Officer (PM)", "zones": ["OVERHEAD"] }
  ],
  "datarefs": [
    { "path": "sim/cockpit/autopilot/heading_mag", "zone": "MCP", "mode": "onchange" }
  ],
  "commands": [
    { "path": "sim/autopilot/NAV", "zone": "MCP" }
  ]
}
```

**Sync modes:**
- `onchange` — sent only when the value changes (switches, buttons); any crew member can send
- `continuous` — master-authoritative state: sent by the physics master on change plus a ~3 Hz heartbeat (parking brake, trim, EFB state, engine outputs)

Useful fields: `"autoSync": true` appends the auto-discovered standard datarefs to the manual list; the `"_SHARED"` zone gives "anyone can send" semantics without zone authority.

---

## SmartCopilot compatibility

If `copilots.json` is not present but `smartcopilot.cfg` is, CoPilots loads it automatically: `[TRIGGERS]` datarefs and `[COMMANDS]` commands are synchronised between all crew members, while thrust levers/axes are filtered out (they are carried by the UDP physics stream). For complex aircraft a native `copilots.json` from [`configs/`](configs/) works noticeably better — see the supported aircraft table.

---

## Status HUD

A small overlay appears in the bottom-right corner showing your role, zones, ping, and crew count. Toggle it via **Plugins → CoPilots → Toggle HUD**.

---

## Troubleshooting

| Problem | What to check |
|---|---|
| Plugin not in menu | Look for `[CoPilots]` lines in X-Plane's `Log.txt` |
| Can't connect | Firewall — allow X-Plane on TCP/UDP 56900–56901. Confirm the host IP is reachable. |
| Datarefs not syncing | Verify paths in `copilots.json` match your aircraft's actual datarefs. Check `Log.txt` for `SyncEngine::writeDr VERIFY FAILED`. |
| Joystick not responding | Confirm you are the physics master (shown in the plugin window). If you just took control, wait one frame for `override_joystick` to clear. |
| Config mismatch error on join | Host and client must load the same aircraft with the same `copilots.json` / `smartcopilot.cfg`. The `drListHash` values must match. |
| Physics jitter | High ping (>150 ms). Ask the host to reassign the Physics Master role to the lowest-latency pilot. |
| Auto-discovered datarefs missing | `DataRefs.txt` must be present at `X-Plane 11\Resources\plugins\DataRefs.txt`. Check `Log.txt` for `AutoDatarefSync` lines. |

---

## Default ports

| Protocol | Port |
|---|---|
| TCP (lobby + state) | 56900 |
| UDP (physics) | 56901 |

---

## License

Copyright (c) 2026 Egor Beletsky. All Rights Reserved.

This software is proprietary. No permission is granted to use, copy, modify, or distribute it without explicit written consent from the author. See [LICENSE](LICENSE) for details.
