# Status

## Current state (2026-11-18)

Numbers below are the values of the profile on this date; the profile (`config/hardware_profile.toml`) is always the truth.
Older, dated notes: `docs/archive/STATUS_HISTORY.md`.

### Works on the real hardware
- **HID gamepad:** X (steering) from the ODrive position over USART1, 900 deg lock-to-lock, `wheel.invert = true`, 2398.7 counts per turn (cpr 2400, gear pair 20:20 = 1:1). Played in a game on the Windows laptop (2026-11-16).
- **HID PID:** Linux (`hid-pidff`) accepts the descriptor (853 bytes), `evtest` lists the FF effects, `fftest` uploads effects.
- **FFB:** arm (B1 tap, tap, hold 3 s), spring pulls the housing to the centre, game forces felt in BeamNG; watchdog works in IDLE; pulling PA9/PA10 ends in IDLE + FAULT; B1 disarms.
- **Verification:** 163 C tests, Python tests, 20 mutation tests, `bash tools/check_all.sh` (5 firmware configurations).

### Current feel and limits (profile, owner decisions step by step)
| value | now | note |
|---|---|---|
| `ffb.max_amps` | 5.0 A (about 2.5 Nm) | code ceiling `FFB_HARD_CAP_AMPS` is 6 A (raised 1 -> 3 -> 6 on 2026-11-18); steps were 0.3, 0.5, 0.8, 1.0, 3.0, 6.0, then back to 4.0 after the supply rose 24 -> 28 V (regeneration) against the 6 A wall; needs `odrive.current_lim` 6 A. **Wheel must stay dismounted at this value** |
| `ffb.host_gain` | 1.0 (off) | game force x gain, then clamped; 5.0 until 2026-11-18, removed after the wheel swung between the end stops |
| `ffb.spring_only_without_host` / `stop_start_deg` / `stop_full_deg` / `stop_amps` | true / 360 / 540 / 5 A | while the GAME plays effects our centring spring is OFF (a standing car does not pull the wheel to the centre; the game's self-aligning force does it with the speed) and a soft stop builds up from 1 turn to 1.5 turns instead. Without a game the spring below works |
| `ffb.spring_amps_90` ... `spring_amps_540` | 0.6, 1.2, 1.8, 2.4, 3.4, 5.0 A (used when no game plays) | the centring force is a CURVE (A at 90, 180, 270, 360, 450, 540 deg from the centre, linear between, held beyond): stiff in the middle and still rising up to 1.5 turns. `spring_kp_amps_per_turn` (3.0) is used only if all six are 0 |
| `ffb.damper_amps_per_turn_s` / `friction_amps` | 0.3 / 0.04 A | more damping and a little friction against the hunting of the wheel on a straight road (gravel and viscous effects, which add damping, were fine) |
| `ffb.slew_amps_per_s` | 80 | the current changes by at most 0.4 A per cycle (100 at first, 40 for a while: impacts were too soft) |
| `ffb.constant_max_amps` | 2.0 A | the STRENGTH OF AN IMPACT: the game's constant force / ramp (crash, skid, self-aligning) alone is limited to this (it was all of max_amps = 5 A, which threw the wheel almost two turns). Gravel and the spring/wall are not limited by it |
| `ffb.constant_deadband_amps` | 0.4 A | the game's small restless constant-force corrections below this are ignored (jerking on a straight road), above it the rest is rescaled |
| `ffb.constant_filter_hz` | 4 | low-pass on the game's constant force / ramp only (the game changes it in steps; each step was a jerk). Gravel and the conditions are not filtered |
| `ffb.vel_filter_hz` | 12 | low-pass on the wheel speed used by damper / friction / wall damping (less jerk from the noisy speed) |
| `ffb.invert_game_direction` | true | MIRRORS the direction of the game's constant force / ramp / periodic effects (not spring/damper/friction/inertia). Set because the wheel wove on a straight road and pushed the wrong way; open until confirmed with a game: turn right at speed and let go, the wheel must come back to the centre |
| `ffb.gain_periodic` / `gain_constant` / `gain_condition` | 8 / 1.2 / 1 | gain per kind of GAME effect: texture (gravel, rumble) x5, constant force and impacts x2, spring/damper/friction of the game x1 (closed loop, keep low); `host_gain` 1.0 stays as an overall multiplier |
| `ffb.periodic_max_amps` | 2.2 A | the texture part alone is limited to this, so a strong rumble cannot drown the main force |
| `ffb.endstop_width_deg` / `endstop_amps` | 60 deg / 1 A | only an EXTRA wall beyond the end of the range (the spring curve already reaches 5 A at 540 deg) |
| `ffb.regen_power_limit_w` | 20 W | braking power limit: the current that OPPOSES the wheel's motion is limited so that kt x A x 2 pi x speed stays below this (a held wall keeps full force, a fast hit is cushioned); keeps the supply from rising |
| `ffb.vbus_derate_start_v` / `_end_v` | 25.0 V / 26.5 V | regeneration guard: the force fades out linearly as the supply rises, instead of the VBUS_RANGE fault at 28 V (flag `vbus_derating` in the status report) |
| `odrive.vel_limit` / `vel_gain` | 4 turns/s / 0.8 (0.25 up to 1 A, 0.4 at 3 A) | `vel_limit * vel_gain >= max_amps * torque_constant` is enforced by `gen_config.py` |
| `ffb.overspeed_turns_s` | 5 | firmware backup of the ODrive overspeed error (a game crash tripped it at 2.4 turns/s with the old limit 2) |

### Not done / open
- **Before the steering wheel is mounted (F8):** mechanical rotation stop, hardware e-stop on the 24 V side, DC fuse, heat sink on the brake resistor, go back from the B1-only arm gesture to B1 + both paddles. Not tested on hardware: OVERSPEED fault.
- **FFB:** direction of game constant forces: `ffb.invert_game_direction` is now true (owner report, not yet confirmed to fix it); separate gains per effect type (gravel, impacts) do not exist yet; the ODrive calibration is repeated at every power-up (`tools/session_start.py`, written and tested with a fake ODrive, not yet run on the real one; the profile is never `--save`d).
- **HID:** pedals (Y/Z are 0), MCP23017 button board not connected, button mapping, I2C bus recovery, persistent settings not wired in.
- **Repository:** the commits of 2026-11-17 are local until pushed; see `git status -sb`.

## Generated status
<!-- BEGIN GENERATED: tools/project_status.py -->
_Generated by `make status`; do not edit by hand._

### Profile entries still TBD (3)

| entry | value | how to measure |
|---|---|---|
| `mcp23017.active_low` | `True` | Buttons switch the input to GND with internal pull-ups. Documentation only (the firmware assumes active-low). |
| `mcp23017.address_7bit` | `32` | A0..A2 tied to GND -> 0x20. Check with the wiring. |
| `pins.i2c` | `PB6 = I2C1_SCL, PB7 = I2C1_SDA (MCP23017)` | Check wiring. |

### Bring-up steps and what blocks them

| step | blocked by (TBD) | hardware needed |
|---|---|---|
| H0 host checks (tools/check_all.sh) | - | - |
| H1 HID path, simulation build | - | STM32F3Discovery on USB USER |
| H2 ODrive doctor (read-only dump) | - | ODrive on 24 V + USB |
| H3 UART wiring + position stream | - | UART wires |
| H4 encoder counts per turn (gears off) | - | - |
| H5 gear ratio / cpr (gears meshed) | - | gear pair meshed |
| H6 gentle ODrive profile (odrive_setup.py) | - | - |
| H7 motor + encoder offset calibration | - | gear pair meshed |
| H8 steering direction / lock-to-lock | - | - |
| H9 MCP23017 buttons | `mcp23017.address_7bit`, `pins.i2c` | MCP23017 board |
| F1+ force feedback bring-up (docs/FFB_SAFETY.md) | - | hardware e-stop, wheel dismounted for F1 |

### `TODO(TBD)` markers in code (8)

- `steeringwheel_firmware/Core/Src/settings_flash_stm32.c:11` * (TODO(TBD) when enabling).
- `steeringwheel_firmware/Core/Inc/wheel_config.h:56` * Used by the ENCODER and ODRIVE sources. TODO(TBD): confirm 540. */
- `steeringwheel_firmware/Core/Inc/wheel_config.h:141` * TODO(TBD): measure (tools/measure_counts.py). Fallback 1.0 = direct drive. */
- `steeringwheel_firmware/Core/Inc/wheel_config.h:153` /* TODO(TBD): 1 = swap the steering direction of the ODrive source. */
- `steeringwheel_firmware/Core/Inc/ffb/ffb_config.h:47` * while IDLE (docs/HARDWARE_BRINGUP.md step F3). TODO(TBD): decide on bench. */
- `steeringwheel_firmware/Core/Inc/ffb/ffb_config.h:64` /* 1 = flip the sign of game forces (HID PID). TODO(TBD): set with fftest. */
- `tools/gen_config.py:655` every power-up (TODO(TBD): confirm on the bench).
- `tools/odrive_enums.py:13` hint and confirm with odrivetool's dump_errors(odrv0) (TODO(TBD)).

### Tests

- host C tests: **PASS** - 163 tests, 0 failed (0 failed checks)
- host Python tests: **PASS** - Ran 87 tests in 1.002s, OK

### Last commits

```
354145d wip: more resistance (owner decision) - spring_kp 1.5 -> 3.0 A/turn, built-in damper 0.05 -> 0.25 A per turn/s; caps, slew and speed limits unchanged; the gentle-defaults guard raised to 4.0 on purpose; 163 tests, all checks pass
540decc wip: odrive.vel_limit 2 -> 4 turns/s and ffb.overspeed_turns_s 3 -> 5 (owner decision): the ODrive tripped OVERSPEED at 2.4 turns/s in a game crash (likely cause of ODRIVE_ERROR) and the velocity limiter weakened every fast force; vel_limit * vel_gain = 1.0 >= 0.5; test no longer pins vel_limit; all checks pass
00b7862 wip: tools/ff_soak.py - imitates a game session without a game (repeatable random mix of cornering force, bumps, rumble, texture), dry-run plan, tests; 87 Python tests, all checks pass
4b7e21a wip: tools/session_start.py - one command for the start of a session (errors read with names, only the watchdog leftover is cleared, calibration at 5 A / 6 A after a typed confirmation, profile to RAM, verdict); profile gets encoder_cal_current 5 A / encoder_cal_current_lim 6 A (measured); 9 tests with a fake ODrive; NOT run on the real ODrive yet; 74+9 Python tests, all checks pass
694c7fd wip: F5 step 4 (owner decision) - ffb.max_amps 1.0 A (code ceiling), slew 100 A/s, host_gain 5.0: the game felt but was weak at 0.8 A / gain 3; vel_limit * vel_gain >= max_amps * kt holds exactly; 163 tests, all checks pass
9b6b694 wip: ffb.host_gain (new profile entry, 3.0): game force x gain BEFORE the clamp, max_amps stays the cap; BeamNG at Strength 400-500 was barely perceptible while ff_play at 100 percent was clear (the game normalises for a wheel with several Nm); tests (gain before clamp, cap never exceeded); 163 tests, all checks pass
8c147d1 wip: tools/wheel_status.html - read-only WebHID monitor of the FFB status report for the Windows laptop (state, reason, fault, torque, host_effects_playing, log); decoder checked against hid_probe with node; NOT tried in a browser yet
d15b910 wip: docs - bench notes on strength and feel (0.8 A, slew 50 A/s, constant spring, ff_play results: pulse, 12 Hz rumble, 40 Hz buzz)
48c5355 wip: ffb.slew_amps_per_s 5 -> 50 A/s (owner decision): at 5 A/s the bench showed a 12.5 Hz 0.8 A sine reduced to about +-0.1 A (slew*period/4) and impacts only ramping; emergency stop does not use the limiter; 162 tests, all checks pass
ae8ea4b wip: F5 step 3 (owner decision) - ffb.max_amps 0.8 A (about 0.40 Nm): at 100 percent of 0.5 A the ff_play effects were barely perceptible; vel_limit * vel_gain >= max_amps * kt still holds; 162 tests, all checks pass
c04579a wip: tools/ff_play.py - play one FF effect (constant, sine, square, pulse) with a chosen strength and direction, finds the wheel by name (fftest levels are too weak and its eventN changes); struct layout and ioctl numbers checked by tests; 71 Python tests
5aa8b1c wip: docs - GAME_DAY runbook (BeamNG on the Windows laptop, hardware stays at the Linux laptop): power-up, manual calibration, cable move, arming, direction check, shutdown; marks what is not verified
```
<!-- END GENERATED: tools/project_status.py -->
