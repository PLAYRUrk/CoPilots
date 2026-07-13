# Colimata Concorde FXP — CoPilots config

| | |
|---|---|
| **Aircraft** | Colimata Concorde FXP |
| **Aircraft version** | FXP 1.10 |
| **X-Plane** | 11.x |
| **Config version** | 1.0.0 |
| **Status** | 🚧 draft (not flight tested) |
| **Based on** | smartcopilot.cfg v1.4 by docpan for Colimata Concorde FXP 1.10 |

## Installation

Copy `copilots.json` into the aircraft folder (next to the `.acf`) **on every crew member's machine** — the file must be identical everywhere, otherwise the client fails the compatibility check on join.

## What is synchronised

- **Datarefs (onchange, ~2300)** — the entire three-crew cockpit (CPT, FO, flight engineer): autopilot, FE panel, electrics, hydraulics, fuel panel, radios, transponder, lights, plus the full standard X-Plane failure set (~510, settable by any crew member).
- **Datarefs (continuous, physics-master-only, 13)** — fuel tank quantities (`CON_SYS_FUEL_tank_*`): Concorde CG is managed by fuel transfer, so tank state streams continuously from the physics master.
- **Commands (289, with hold relay)** — buttons and switches driven by the aircraft's logic.

## Deliberately NOT in this config

- Thrust levers, reversers (`CON_CC_THRO_reverser_*`), yoke, pedals, brakes — UDP PhysicsSync stream.
- Standard `sim m_fuel` tanks — UDP fuel stream from the physics master (the custom Colimata tanks go via TCP continuous).
- Simulator time — WeatherSync.

## Known limitations

- **Not flight tested** — generated from docpan's SmartCopilot file and passed the automated audit (0 duplicate channels), but not yet verified in a multi-crew session. Start by checking: thrust levers (smoothness), the FE fuel panel (tank quantities must converge for everyone), visor/nose, autopilot.
- Button indicator lamps (`*_lgt_*`) are synced as in the original SC config; if a lamp flickers/fights — report it and we will exclude it.
- Test switches (`*_test_*`, ~40) are synced as positions, as in the original SC config. If one of them turns out to be spring-loaded (held) and starts jerking — report it: we will move it to the command channel, like the 737's GPU/APU GEN.
