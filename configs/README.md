# CoPilots — aircraft config library

Ready-made `copilots.json` files for specific aircraft. Copy the config for your aircraft into the aircraft's folder (next to the `.acf`) **on every crew member's machine** — the files must be identical.

If your aircraft is not listed here, the plugin still works without a config: auto-discovery synchronises the standard X-Plane datarefs, and a `smartcopilot.cfg` (if present in the aircraft folder) is picked up as a fallback.

## Supported aircraft

| Aircraft | Folder | X-Plane | Status |
|---|---|---|---|
| LevelUp Boeing 737NG | [`levelup-737ng/`](levelup-737ng/) | 11.5x | 🧪 flight testing |
| Colimata Concorde FXP | [`colimata-concorde/`](colimata-concorde/) | 11.x | 🚧 draft |
| IXEG Boeing 737-300 | [`ixeg-737-300/`](ixeg-737-300/) | 11.x | 🚧 draft |

Status legend: ✅ flight tested · 🧪 flight testing · 🚧 draft

## How to build a config for your aircraft

The golden rule: **one cockpit control — exactly one sync channel** (a dataref *or* a command, never both — otherwise switches snap back and skip positions).

1. **Switch positions — as datarefs** (`"mode": "onchange"` is the default), but only if the aircraft accepts writes into them. A good reference is the `[TRIGGERS]` section of the aircraft's SmartCopilot config: everything listed there is write-controllable.
2. **Buttons and Lua-driven toggles — as commands.** The plugin relays both press AND hold (Begin/End), so held controls (fire-warning tests, spring-loaded switches) work correctly.
3. **State owned by the master's simulation** (parking brake, trims, EFB/tablet state, engine outputs) — as datarefs with `"mode": "continuous"`: only the physics master sends them, on change plus a ~3 Hz heartbeat.
4. **Spring-loaded (momentary-hold) switches — commands only, never positions.** Position sync breaks the hold: the receiver's spring returns the value, the revert echoes back and knocks the sender's switch down (the LevelUp APU GEN/GPU lesson).
5. **Pulse button datarefs (0→1→0) — do not sync at all**; use the matching command instead.
6. **Never add**: thrust/reverse levers (`*throttle*`, `*thrust*lever*`, `*reverse*`), yoke (`*yoke*`), toe brakes, axes (`*/axis/*`) — all of that is carried by the UDP PhysicsSync stream; a TCP duplicate causes lever jitter. Also skip `sim m_fuel` (UDP fuel stream) and `sim/time/*` (WeatherSync), and ever-growing counters (`total_running_time_sec` etc.).
7. **`"autoSync"`**: `true` — append auto-discovered standard `sim/` datarefs (fine for simple aircraft); `false` — sync strictly the listed set (mandatory for aircraft with custom logic like Zibo/LevelUp — the auto-discovered `sim/*` mirrors fight the custom state and cause knob jitter).
8. From SmartCopilot configs, do **not** carry over `[SEND_BACK]` (axes → UDP) or `[CONTINUED]` as-is (simulation outputs); take only parking brake / trim / genuinely master-owned state from it as `continuous`.
9. After generating, audit for duplicate channels: exact duplicates, same path in both lists, and stem collisions (a command like `heading_up` next to a dataref like `mcp_hdg_dial` controlling the same knob).

Folder layout: `configs/<developer>-<model>/copilots.json` + `README.md` (aircraft version, X-Plane version, status, limitations).
