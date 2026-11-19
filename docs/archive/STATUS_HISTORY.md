# Status history (newest first)

Hand-written status notes moved out of `docs/STATUS.md` on 2026-11-18. Numbers here are the values of THAT day (for example `max_amps` 0.5 A); the current values are always in `config/hardware_profile.toml`.

# Update 2026-11-17: force feedback runs on the bench (wheel dismounted, Linux)
Done on the real hardware, in order (details and the numbers are in `docs/HARDWARE_BRINGUP.md`, the bench notes):
- **F1** FFB build (ARM_ALLOWED=1, B1_ONLY=1, `FFB_FEED_WATCHDOG_WHEN_DISARMED=1`): the Linux kernel accepts the PID descriptor
  (hid-pidff, `wDescriptorLength` 853), `evtest` lists all FF effects, `fftest` uploads six effects.
- **H4/H5/H6/H7**: 2398.7 counts per turn (cpr 2400, 1:1); the gentle profile is applied to RAM (never `--save`d); the encoder offset
  calibration needs 5 A / 6 A (3 A and 4 A fail with CPR_POLEPAIRS_MISMATCH) and is done by hand at every ODrive power-up.
- **F3/F4/F6**: the watchdog runs in IDLE too, the STM32 feeds it; pulling PA9 or PA10 while armed ends in IDLE (ODrive watchdog) and a
  latched FAULT (POSITION_STALE); B1 disarms. **Not tested on hardware:** OVERSPEED (hand-spinning a 4 kg rotor was judged unsafe).
- **F5**: first arm worked after two firmware fixes found on the bench (`axis0.error` prints as `2048d`; reads starved the position request
  in PRECHECK/ARMING) and one profile fix (the ODrive velocity limiter clipped the torque to `vel_limit * vel_gain`, now `vel_gain` 0.25).
  The spring pulls the housing back to the centre by itself (sign confirmed by motion, not by feel). Limits now: `ffb.max_amps` 0.5 A, spring 1.5 A/turn.
- **F7** (`fftest`): spring, damper and constant all produce torque, capped at 0.5 A. The `fftest` levels (25 %) are too small to feel.
- **Open:** direction of game constant forces (`FFB_INVERT_HOST_FORCE`, convention PID/DirectInput 90 deg = +X) is verified only against the spec
  until a game runs; the game test itself (BeamNG is available on another machine, Windows behaves differently from Linux hid-pidff);
  F8 (mounting the wheel) needs the hardware e-stop (owner reports the safety items done) and a mechanical rotation stop (not confirmed);
  `odrive_calibrate.py` still refuses 5 A / 6 A; pedals (Y/Z = 0) and the MCP23017 buttons; `--save` of the profile deliberately not done.

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
  `docs/archive/DUMP_RECONCILIATION.md` (mismatches: regen limit -3.0 A not -0.01 A, `enable_current_mode_vel_limit`
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
