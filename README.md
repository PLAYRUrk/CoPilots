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
      pdfium.dll
```

> `pdfium.dll` is the PDF renderer used by the Charts window. The plugin works without it, but PDF charts will not display.

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

A small overlay appears in the bottom-right corner showing your role, zones, ping, and crew count. Toggle it via **Plugins → CoPilots → Toggle HUD**. The quick buttons **C** / **N** / **Ch** open the Connect window, the Notepad and the Charts window.

---

## Notepad (shared whiteboard)

**Plugins → CoPilots → Notepad** opens a tabbed drawing board. Tabs are private until you click **Share** — a shared tab becomes visible to the whole crew and everyone can draw on it (Pen / Eraser / Line / Rect / Ellipse; each participant inks in their own colour). The smart eraser deletes whole strokes it touches. Late joiners receive the full shared content automatically.

---

## Laser pointer

A shared cockpit laser pointer: hold the `CoPilots/pointer_hold` command (bind it to a key or joystick button in X-Plane's keyboard/joystick settings) and every crew member sees a coloured dot where you point in the cockpit.

The pointer is **off by default**. Enable it with the **Enable laser pointer** checkbox in the Settings section at the bottom of the Connect / Host window; the choice persists between sessions. Disabling only stops your own pointer — you always see the others'.

---

## Charts (ChartFox)

**Plugins → CoPilots → Charts** opens the ChartFox charts window:

- **Tabs = airports.** Click **+**, enter an ICAO and press **Load** — the chart list appears grouped by type (Ground, Approach, SID, STAR, ...). Charts flagged **[G]** are georeferenced.
- **ChartFox sign-in (OAuth).** Open the *ChartFox account* section at the top of the Charts window and click **Login with ChartFox** — the browser opens, you approve access with your ChartFox account, and the plugin picks up the tokens automatically. Tokens refresh themselves; you sign in once. Each crew member signs in with **their own** ChartFox account — chart files are never transferred over the session (chart source terms of use). The plugin ships with the CoPilots app's OAuth client built in (Public client + PKCE; a different client can be set under *Advanced*, and a raw token can be pasted under *Use a token directly (fallback)*). Tokens are stored in plain text in `Output/preferences/copilots_prefs.json`, like the lobby password.
- **Viewing.** Mouse wheel zooms, right-drag (or the Pan tool) pans, multi-page PDFs get a page selector. The chart source's copyright line is shown on the chart.
- **Drawing.** The full notepad toolset works directly on the chart; ink sticks to chart features at any zoom level. Share the tab and the whole crew sees the same airport, selected chart and drawings, each drawing in their own colour.
- **Aircraft position.** On georeferenced charts (**[G]**) a yellow triangle shows the aircraft's position and heading — for taxi and situational awareness. A line under the chart always says what the symbol is doing: shown, off this chart, or no georeference for this page.
- **Calibrate charts yourself.** Most charts ChartFox serves carry no georeference at all. Press **Calibrate** on the chart toolbar, click a point you can identify on the chart, and say where it is — by **navaid or airport ident** (taken from X-Plane's nav database, so it works parked), by the **aircraft's own position** (handy while taxiing), or by **typing the coordinates** printed on the chart's grid (`55 58 21 N`). Repeat for a second point, press **Apply**, and the aircraft symbol works on that chart from then on. Calibrations are saved in `Output/CoPilots/chart_calibrations.json`, are shared with the rest of the crew, and override a ChartFox georeference that turns out to be wrong (**Clear calibration** removes yours).
- **Shared calibrations.** Calibrating the same approach chart in every cockpit is wasted work, so calibrations are pooled in the project repository. **Calibrations you make are sent automatically** (anonymously — the chart id and four numbers, nothing about you); **Share to community** is there for the case where that has not gone through yet. Calibrations other pilots made are **downloaded once per airport and kept** in `Output/CoPilots/chart_cal_cache/`, so they cost no traffic on later flights and keep working with no internet connection at all — the status line under the chart says *(community calibration)*. Your own calibration always wins over anyone else's on your machine, so a wrong shared one is fixed by pressing **Calibrate**. *ChartFox account → Shared calibrations* has **Refresh from GitHub** (fetch the open airports again) and **Share all my calibrations**.
- **METAR.** The current METAR for the tab's airport is fetched from aviationweather.gov and shown under the chart list; press **Refresh** to update.

Downloaded charts are cached in `X-Plane 11\Output\CoPilots\chartfox_cache\` and are not re-downloaded on restart; **Re-download** on the chart toolbar discards the cached copy if it ever arrives damaged.

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
| Laser pointer does nothing | Enable it in the Settings section of the Connect / Host window and bind `CoPilots/pointer_hold` to a key/button. |
| Charts window shows "Not signed in to ChartFox" | Sign in via *ChartFox account* → **Login with ChartFox** and approve in the browser. |
| "ChartFox granted new permissions" in the account section | The stored token predates a permission the plugin now uses. **Logout**, then **Login with ChartFox** again. |
| A chart fails to load | **Retry** on the chart toolbar; **Re-download** if the cached file itself is damaged. Expired sessions renew themselves. |
| ChartFox login page shows a redirect error | The redirect URI registered for the OAuth client must be exactly `http://localhost:58525/callback` (or change the Redirect field in the plugin to match the registered one). |
| PDF charts show a pdfium error | `pdfium.dll` must sit next to `win.xpl` in `plugins\CoPilots\64\`. Reinstall the plugin from the release archive. |
| Aircraft symbol missing on a chart | The status line under the chart says why. "No georeference for this page" means ChartFox publishes none — press **Calibrate**. |
| Aircraft symbol misplaced on a chart | Georeference accuracy depends on the ChartFox data; **Calibrate** overrides it with your own two points. *ChartFox account → Diagnostics → Show georeference numbers* prints the raw parameters. |
| A shared calibration is wrong | Press **Calibrate** and make your own — yours wins over a community one, and sharing it corrects the database for everyone else. |
| **Share to community** opens a browser instead of sharing | No submission endpoint is published yet, so the plugin falls back to a prefilled GitHub issue — press Submit there and the calibration still reaches the database. |

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
