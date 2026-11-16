# Update 2026-11-16 (evening): the wheel plays in a game
- Real hardware chain works: ODrive (24 V) -> UART PA9/PA10 -> STM32 (default build, WHEEL_FFB=OFF) -> USB USER -> PC. Tested on
  a second (Windows) laptop in a game: steering works. `pins.uart_tx/uart_rx` marked `measured`.
- Measured: gear pair 20:20 (counts), brake resistor 50 W / 2 ohm, clockwise lowers X so `wheel.invert = true`,
  X readings match a 900 deg lock at cpr 2400 within ~2 percent. `wheel.lock_to_lock_deg = 900` (owner decision).
- Still open: exact counts per turn (10-turn test, expect ~24000), MCP23017 buttons (no board), pedals (Y/Z are 0),
  gentle ODrive profile (H6), calibration (H7) and everything FFB; hardware e-stop on the 24 V side (a mains switch exists).

# Update 2026-11-16: gear pair counted (20:20)
- The encoder is driven by a **gear pair**, not a belt: external pinion (20 teeth) on the encoder shaft meshing with an
  internal gear (20 teeth) on the motor housing. `belt.teeth_housing = belt.teeth_pulley = 20`, status `measured`
  ("counted by the user, 2026-11-16"). Derived: ratio 1:1, `cpr = 2400` (integer), `ODRIVE_TURNS_PER_WHEEL_REV = 1.0`.
- **Still open:** the real counts per turn (rotate the housing 10 turns, expect about 24000, `tools/measure_counts.py`;
  the earlier rough hand count gave about 5000 per turn), and the steering direction: `wheel.invert` stays `TBD`
  (bench step in H8: turn clockwise, X must increase). Geometry note in `docs/DECISIONS.md`: an internal gear and its
  pinion with equal tooth counts is suspicious, re-count both gears if the bench count disagrees.

# Status (2026-11-16, after the real-dump reconciliation)

## Reconciliation with the real ODrive dump
- `docs/hardware.md` and the real `docs/odrive_dump.txt` were added by the owner; everything was re-checked:
  `docs/DUMP_RECONCILIATION.md` (mismatches: regen limit -3.0 A not -0.01 A, `enable_current_mode_vel_limit`
  name, UART names, no brake enable switch). The dump predates the owner's startup-flag change (flags are saved False,
  verified after a power cycle, which also cleared the three errors it shows): replace it at the next ODrive power-up.
- Tests now use the real dump (Python + `tests/test_ffb_dump.c`); new pre-arm condition `NOT_IDLE`.

# Status (end of session 2, 2026-11-16)

## Session 2 start (recorded before any change)
- `git status`: branch `wip-local`, up to date with `origin/wip-local`, clean. HEAD `1b70668`.
- `make -C tests`: 64 tests, 0 failed. Default ARM build: 0 warnings, FLASH 56 832 B.

## Done

| Step | What | Verified by |
|---|---|---|
| 1 | `config/hardware_profile.toml` (single source of truth, every value `TBD`/`measured`/`assumed` + how to measure) | `gen_config.py` validation, Python tests |
| 1 | `tools/gen_config.py` -> `wheel_config_generated.h` (CMake regenerates it), `tools/generated/odrive_setup.py` (dry run default), `odrive_calibrate.py` | tests with a fake ODrive; the default binary was **byte-identical** after Step 1 |
| 1 | `tools/odrive_status.py` (read-only doctor, `--fixture`), `hid_probe.py`, `measure_counts.py`, `project_status.py` / `make status`, `check_all.sh`, `99-steeringwheel.rules`; `stm_32_wheel.py` shows 3 axes + 16 buttons | Python tests against the REAL `docs/odrive_dump.txt` (synthetic fixtures removed) |
| 1 | `docs/HARDWARE_BRINGUP.md` | - |
| 2A | FFB layer A in `Core/Src/ffb`: safety state machine, read-only precheck/monitoring, torque pipeline (spring, damper, friction, inertia placeholder, soft end stops, clamp, slew, kt conversion), allowlisted writes, arm gesture, LEDs, status report | C tests incl. a simulated ODrive, every fault path, 17 mutation tests |
| 2B | HID PID descriptor (FFB build only), effect engine (11 effect types, envelope, timing, gains, device control), report dispatcher, ISR queue, own USB class driver (no generated/middleware file changed) | descriptor self-check parser, canned/malformed packets, fuzz |
| 2 | `docs/FFB_SAFETY.md` (safety case, NOT-protected list, F0-F8 bring-up) | - |
| 3 | Persistent settings (CRC, mock flash), IWDG + reset reason, steering deadzone/curve, ODrive-reboot re-centre - all OFF | C tests + compile check with everything ON |
| docs | README, ARCHITECTURE (pedal plug-in rules), DECISIONS, IOC_TODO, BENCH_CHECKLIST (FFB section) | - |

**Results now:** `make -C tests` 150 tests, 0 failed. Python 56 tests OK. `bash tools/check_all.sh`
ALL CHECKS PASSED: default / simulate / ffb_noarm / ffb_arm_compile_only / extras_on_compile_only
build with 0 warnings; default binary has no write token; 17/17 mutations killed.

## Not done
- Button mapping table and long-press combos; I2C bus recovery for a stuck MCP23017 (notes in DECISIONS).
- Persistent settings are not wired into the application (module + glue + tests only).
- Nothing was run on hardware: no flashing, no USB enumeration, no ODrive. The FFB code and the generated
  ODrive scripts have never touched a real device.
- Open questions (TODO(TBD)): watchdog in IDLE (F3), ASCII `u 0` and boolean format in this dev build,
  game-force direction, brake resistor hardware / regen limits, a fresh ODrive dump (the committed one is
  stale for the startup flags and errors), every profile value.

## Generated status
<!-- BEGIN GENERATED: tools/project_status.py -->
_Generated by `make status`; do not edit by hand._

### Profile entries still TBD (22)

| entry | value | how to measure |
|---|---|---|
| `encoder.has_index` | `False` | LPD3806 has no Z/index channel. Keep false. |
| `encoder.ppr` | `600` | LPD3806-600BM: 600 pulses per revolution per channel (label). Confirm with tools/measure_counts.py: one encoder-shaft turn by hand must give 4*PPR counts. |
| `encoder.quadrature` | `4` | ODrive counts every edge of A and B (x4). Keep 4 unless the measurement says otherwise. |
| `mcp23017.active_low` | `True` | Buttons switch the input to GND with internal pull-ups. Documentation only (the firmware assumes active-low). |
| `mcp23017.address_7bit` | `32` | A0..A2 tied to GND -> 0x20. Check with the wiring. |
| `motor.motor_turns_per_wheel_rev` | `1.0` | Turn the steering wheel exactly one full turn and watch the motor housing: number of housing turns. 1.0 for direct drive. |
| `motor.pole_pairs` | `15` | Hoverboard hub motor: 30 magnets = 15 pole pairs (ODrive hoverboard guide). Count magnets or check the motor data. |
| `odrive.calibration_current` | `3.0` | Amps used by motor/encoder calibration. Must be <= current_lim. Increase only if calibration fails. |
| `odrive.current_lim` | `4.0` | Amps. Start low (4 A). The precheck refuses to arm if axis0.motor.config.current_lim is higher. |
| `odrive.dc_bus_overvoltage_trip_level` | `30.0` | Volts. Supply is 24 V; regenerated energy pumps the bus up. 30 V trips well below the 56 V factory value. |
| `odrive.dc_bus_undervoltage_trip_level` | `18.0` | Volts. Below this the ODrive disarms. |
| `odrive.dc_max_negative_current` | `-3.0` | Saved value in docs/odrive_dump.txt: -3.0 A (NOT the -0.01 A factory default assumed earlier). Negative DC-bus current allowed before DC_BUS_OVER_REGEN_CURRENT. This build has a 2 ohm brake resistor armed automatically (brake_resistor_armed True) and max_regen_current 0.0, so regen should go to the resistor, not the PSU. Choosing a value is a human bring-up decision (docs/DECISIONS.md). |
| `odrive.dc_max_positive_current` | `6.0` | Amps drawn from the supply (motoring). |
| `odrive.enable_current_mode_vel_limit` | `True` | Name in THIS build (docs/odrive_dump.txt; 0.5.6 calls it enable_torque_mode_vel_limit). Saved: True. Keep true: in torque mode the ODrive reduces torque above vel_limit. |
| `odrive.enable_overspeed_error` | `True` | Saved: True (with vel_limit_tolerance 1.2). Keep true: the ODrive itself errors above 1.2 x vel_limit. |
| `odrive.enable_watchdog` | `True` | Required by the FFB precheck. Check on the bench whether your 0.5.x build also checks the watchdog while IDLE (docs/HARDWARE_BRINGUP.md step F3). |
| `odrive.encoder_mode` | `0` | ENCODER_MODE_INCREMENTAL = 0. |
| `odrive.max_regen_current` | `0.0` | Saved: 0.0 A (current that may flow back into the PSU; 0 = all regen to the brake resistor). CHECK ONLY (never written). |
| `odrive.torque_constant` | `0.04` | Nm/A (saved: 0.04, docs/odrive_dump.txt). Firmware READS it and converts amps -> torque units. Set 1.0 to make torque units equal amps, or 8.27/KV for real Nm (hoverboard KV is about 16). |
| `odrive.vel_limit` | `2.0` | Motor turns/s. 2 turns/s = 720 deg/s at the wheel (direct drive). The precheck refuses to arm if higher. |
| `odrive.watchdog_timeout` | `0.25` | Seconds. The FFB build feeds 'u 0' every 5 ms while running. The precheck requires enable_watchdog = True and a timeout inside [ffb.watchdog_timeout_min_s, ffb.watchdog_timeout_max_s]. |
| `pins.i2c` | `PB6 = I2C1_SCL, PB7 = I2C1_SDA (MCP23017)` | Check wiring. |

### Bring-up steps and what blocks them

| step | blocked by (TBD) | hardware needed |
|---|---|---|
| H0 host checks (tools/check_all.sh) | - | - |
| H1 HID path, simulation build | - | STM32F3Discovery on USB USER |
| H2 ODrive doctor (read-only dump) | - | ODrive on 24 V + USB |
| H3 UART wiring + position stream | - | UART wires |
| H4 encoder counts per turn (gears off) | `encoder.ppr`, `encoder.quadrature` | - |
| H5 gear ratio / cpr (gears meshed) | `motor.motor_turns_per_wheel_rev` | gear pair meshed |
| H6 gentle ODrive profile (odrive_setup.py) | `odrive.current_lim`, `odrive.calibration_current`, `odrive.vel_limit`, `odrive.dc_bus_overvoltage_trip_level`, `odrive.dc_bus_undervoltage_trip_level` | - |
| H7 motor + encoder offset calibration | `motor.pole_pairs`, `odrive.calibration_current` | gear pair meshed |
| H8 steering direction / lock-to-lock | - | - |
| H9 MCP23017 buttons | `mcp23017.address_7bit`, `pins.i2c` | MCP23017 board |
| F1+ force feedback bring-up (docs/FFB_SAFETY.md) | 19 safety entries TBD | hardware e-stop, wheel dismounted for F1 |

### `TODO(TBD)` markers in code (8)

- `steeringwheel_firmware/Core/Src/settings_flash_stm32.c:11` * (TODO(TBD) when enabling).
- `steeringwheel_firmware/Core/Inc/wheel_config.h:56` * Used by the ENCODER and ODRIVE sources. TODO(TBD): confirm 540. */
- `steeringwheel_firmware/Core/Inc/wheel_config.h:141` * TODO(TBD): measure (tools/measure_counts.py). Fallback 1.0 = direct drive. */
- `steeringwheel_firmware/Core/Inc/wheel_config.h:153` /* TODO(TBD): 1 = swap the steering direction of the ODrive source. */
- `steeringwheel_firmware/Core/Inc/ffb/ffb_config.h:47` * while IDLE (docs/HARDWARE_BRINGUP.md step F3). TODO(TBD): decide on bench. */
- `steeringwheel_firmware/Core/Inc/ffb/ffb_config.h:64` /* 1 = flip the sign of game forces (HID PID). TODO(TBD): set with fftest. */
- `tools/gen_config.py:638` every power-up (TODO(TBD): confirm on the bench).
- `tools/odrive_enums.py:13` hint and confirm with odrivetool's dump_errors(odrv0) (TODO(TBD)).

### Tests

- host C tests: **PASS** - 159 tests, 0 failed (0 failed checks)
- host Python tests: **PASS** - Ran 63 tests in 0.180s, OK

### Last commits

```
1420bbe wip: UART wiring measured on the bench (position stream works), game test on a second laptop, STATUS summary
4b23696 wip: bench result - clockwise lowers X, so wheel.invert = true (measured); X readings confirm 1:1 / cpr 2400 for a 900 deg lock; link tests pinned to non-inverted
f5b2f6f wip: lock-to-lock 900 deg (owner decision, +-450): profile, generated header, visualiser MAX_ANGLE 450, docs; link tests pinned to 540 so they do not depend on the profile
691edea wip: brake resistor label read by the user (50 W, 2 ohm on AUX) recorded as measured; heat sink and peak power (-3 A) noted as open
dde681c wip: udev rule also grants plugdev (uaccess ACL was not applied on replug); first fresh live ODrive status dump (flat, 2026-11-16, errors 0, IDLE, startup flags False)
cfa8412 wip: docs - 20:20 gear pair replaces belt TBD; bench steps for 10-turn count check (about 24000) and steering direction
355063a wip: gear pair 20:20 counted (profile measured, cpr 2400, turns per wheel 1.0), optional note field, gear wording in comments/tests; wheel.invert stays TBD
4d0f1d1 wip: docs - odrive_dump.txt predates the startup-flag change (flags saved False, errors cleared); replace it with a fresh dump at the next ODrive power-up; hardware.md note replaced
2ddff52 wip: docs - DUMP_RECONCILIATION table, regen/brake correction and decisions, startup-flag contradiction noted in hardware.md, STATUS/HARDWARE_BRINGUP/BENCH_CHECKLIST/FFB_SAFETY updated for the real dump
539135f wip: reconcile with the real ODrive dump - shared dump parser for the IPython format, tests on docs/odrive_dump.txt (synthetic fixtures removed), real property names (enable_current_mode_vel_limit, enable_uart/uart_baudrate), regen -3.0 A, check-only brake entries, NOT_IDLE pre-arm check, dry-run diff against the dump
098c49c wip: add verified hardware facts and real ODrive dump
9bd5c00 wip: STATUS.md (session 2 summary + generated status), wheel_config comment
```
<!-- END GENERATED: tools/project_status.py -->
