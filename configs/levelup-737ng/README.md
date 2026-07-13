# LevelUp Boeing 737NG — CoPilots config

| | |
|---|---|
| **Aircraft** | LevelUp 737NG series (-600/-700/-800/-900) |
| **X-Plane** | 11.5x |
| **Config version** | 1.0.0 |
| **Status** | 🧪 flight testing |
| **Based on** | smartcopilot.cfg by Birdy.dma for Zibo 3.40.19 / XP 11.41 (closest base to LevelUp) |

## Installation

Copy `copilots.json` into the aircraft folder (next to the `.acf`) **on every crew member's machine** — the file must be identical everywhere, otherwise the client fails the compatibility check on join. If a `smartcopilot.cfg` is present in the folder, it is ignored (`copilots.json` takes priority).

## What is synchronised

- **Datarefs (onchange, ~700)** — switch positions, MCP dials, baro, EFIS, lights, brightness, radios, the full failure set (~520), aircraft options from the FMC menu (units, effects, payload/pax).
- **Datarefs (continuous, physics-master-only)** — parking brake, trims, EFB/tablet state (whole `efb` array), physical failure states (collapsed gear, blown tires).
- **Commands (652, with hold relay)** — every button and Lua-driven toggle: both CDUs, fire panel, tests (stall/overspeed/fire), generators (GEN/APU GEN/GPU — spring-loaded, hold works), packs, spring switches, cockpit windows.

## Deliberately NOT in this config

- Thrust levers / reverse levers / trim values / toe brakes — streamed by UDP PhysicsSync at 60 Hz (adding them here causes lever jitter).
- Engine parameters (N1/N2), fuel per tank — UDP stream from the physics master.
- Audio panel mic selectors — per-pilot local state.

## Known limitations

- Parking brake, trim and EFB state can only be changed from the current physics master's side (same as SmartCopilot).
- Some brightness knobs were added using Zibo 4.x dataref names — if LevelUp lacks them they are silently ignored (harmless).
