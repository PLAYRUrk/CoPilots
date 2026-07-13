# IXEG Boeing 737-300 — CoPilots config

| | |
|---|---|
| **Aircraft** | IXEG 737 Classic (737-300) |
| **Aircraft version** | 1.33 / 1.1x era |
| **X-Plane** | 11.x |
| **Config version** | 1.0.3 |
| **Status** | 🚧 draft (not flight tested) |
| **Based on** | smartcopilot.cfg 2020-12-29 by Birdy.dma for IXEG 737 V1.33 |

## Installation

Copy `copilots.json` into the aircraft folder (next to the `.acf`) **on every crew member's machine** — the file must be identical everywhere, otherwise the client fails the compatibility check on join.

## What is synchronised

- **Datarefs (onchange, ~1160)** — the whole cockpit: overhead, autopilot/MCP, radios, transponder, audio panels, lights, the full standard failure set (~515) plus all 29 IXEG custom failures (electrics + hydraulics, settable by any crew member via the IXEG menu), **airframe options** (`ixeg/733/misc/winglets`) and other IXEG preferences.
- **Datarefs (continuous, physics-master-only, ~40)** — engine parameters (EPR, fuel flow, oil pressure/temperature/quantity), battery charge, parking brake, trims.
- **Commands (23, with hold relay)** — FMC CLR, tests (EGPWS, cargo fire, bleed ovht, transponder), gear release, trim, TOGA, BetterPushback, shared pause.

## Deliberately NOT in this config

- Thrust levers, reversers (`eng*_rev_angle`), yoke, rudder, brakes, axes — UDP PhysicsSync stream.
- `sim m_fuel` — UDP fuel stream from the physics master.
- Simulator time — WeatherSync.
- Keyboard speedbrake step commands (`speed_brakes_*_one`) — the lever position is synced as a dataref.

## Known limitations

- **Not flight tested.** Check first: thrust levers (smoothness), engine start, EPR/oil gauges on the client, spring-loaded/held switches (starters, tests).
- IXEG's own failure menu exposes only 29 datarefs (electrics + hydraulics) — those sync. Other entries of the IXEG failure menu live inside Gizmo Lua and cannot be synced; use the standard X-Plane failure menu for everything else (all of those sync).
- IXEG CDU keys are mouse zones without commands (only CLR exists in the SC config) — client FMC keystrokes may not propagate; this is a known aircraft limitation.
- Audio panels (VHF selectors) are synced, as in the original SC config.
