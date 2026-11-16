# Reconciliation: our assumptions vs the real ODrive dump

Source of truth: `docs/odrive_dump.txt` (odrivetool 0.5.4 session on the bench board, 2026-11-16;
hw 3.6 variant 56, fw 0.0.0 "unreleased", early 0.5.x / 0.5.1-like API). Paths are relative to `odrv0`.
"Where used": FW = firmware (`Core/Src/ffb/ffb_precheck.c`, `ffb_cmd.c`), SET = generated
`tools/generated/odrive_setup.py`, CAL = `odrive_calibrate.py`, DOC = `tools/odrive_status.py`,
PROF = `config/hardware_profile.toml`, docs = `docs/*.md`.

Automatic checks that keep this table true: `tests/test_ffb_dump.c` (every firmware precheck path
and both write targets exist in the dump; real values give the expected verdicts) and
`tools/tests/test_odrive_status.py` / `test_gen_config.py` (every path read or written by the tools
exists; dry-run diff against the dump).

## Mismatches found (action taken)

| Item | Where used | What we assumed | What the dump says | Action |
|---|---|---|---|---|
| `config.dc_max_negative_current` | PROF, SET, docs (DECISIONS, FFB_SAFETY, STATUS) | -0.01 A (factory default) | **-3.0** (float) | Profile = -3.0 (TBD, no change proposed); docs corrected; implications + "human decision" in DECISIONS |
| `controller.config.enable_torque_mode_vel_limit` | PROF, SET, FFB_SAFETY | exists (0.5.6 name) | **does not exist**; this build has `enable_current_mode_vel_limit: True` | Renamed everywhere; the old name would have been silently skipped |
| `config.enable_uart_a`, `config.uart_a_baudrate`, `config.uart0_protocol` | DOC, BENCH_CHECKLIST, HARDWARE_BRINGUP | possible names | **do not exist**; `config.enable_uart: True`, `config.uart_baudrate: 115200` | DOC reads the real names (old ones only as optional fallbacks); docs fixed; `uart.baud` -> measured |
| `config.enable_brake_resistor` | DOC, PROF text | may exist | **does not exist**; `brake_resistor_armed: True`, `config.brake_resistance: 2.0` | Removed from reads; brake values are new check-only profile entries |
| device-level `error` | DOC, setup/cal scripts | may exist (0.5.2+) | **does not exist** (early API) | Read only if present; doctor prints "(not in dump)" |
| `axis0.config.startup_closed_loop_control`, `startup_encoder_offset_calibration` | DOC, SET, docs/hardware.md | False (hardware.md line 30) | **True** (dump lines 165, 167) | Reported as ERROR by DOC; SET would set False; new FW pre-arm check `current_state == IDLE` (NOT_IDLE); contradiction noted in hardware.md |
| Error registers | DOC, FW precheck | 0 | `axis0.error` 256 = ENCODER_FAILED, `motor.error` 16 = CONTROL_DEADLINE_MISSED, `encoder.error` 4 = NO_RESPONSE | Decoded by DOC; FW precheck/monitor refuse (tested with the real values) |
| `axis0.current_state` path | FW (`FFB_VERIFY_CLOSED_LOOP`) | TODO(TBD) | exists, int32, 1 | TBD resolved; now also a pre-arm condition |
| Dump format | DOC `--fixture` | flat or `name = value` tree | IPython session, `name: value (type)`, `name: ...`, methods | New parser `tools/odrive_dump.py`, robust + problem reporting |
| Motor error 0.5.1 bits | `odrive_enums.py` | 0.5.6 table only | early API -> 0.5.1 bits possible | Added legacy names for bits the 0.5.6 table lacks, marked unverified |

## Everything else checked (consistent)

| Item | Where used | Assumed | Dump | Action |
|---|---|---|---|---|
| `vbus_voltage` | FW, DOC, SET, CAL | float, ~24 V | 24.186 | none |
| `axis0.config.enable_watchdog` | FW, SET, PROF | exists | False (bool) | FW refuses (WATCHDOG_DISABLED); SET proposes True |
| `axis0.config.watchdog_timeout` | FW, SET, PROF | exists | 0.0 (float) | FW refuses (range); SET proposes 0.25 |
| `axis0.watchdog_feed()` / ASCII `u 0` | FW (write) | exists | method present; ASCII `u` not visible in a dump (TBD on bench) | none |
| `axis0.controller.config.control_mode` | FW, SET | 1 = torque required | 3 (POSITION) | FW refuses; SET proposes 1 |
| `axis0.controller.config.input_mode` | SET | 1 passthrough | 5 (TRAP_TRAJ) | SET proposes 1 |
| `axis0.controller.config.vel_limit` | FW, SET | <= 2 | 50.0 | FW refuses; SET proposes 2 |
| `axis0.controller.config.enable_overspeed_error` | PROF, SET | (new) | True | added, keep True |
| `axis0.controller.config.pos_gain`, `vel_gain`, `vel_integrator_gain` | DOC (info) | - | 30, 0.02, 0.2 | shown only; irrelevant in torque mode |
| `axis0.motor.config.current_lim` | FW, SET | <= 4 | 12.0 | FW refuses; SET proposes 4 |
| `axis0.motor.config.calibration_current` | SET, CAL | 3 | 5.0 (worked for motor calibration) | SET proposes 3; DECISIONS notes 5 A known-good |
| `axis0.motor.config.torque_constant` | FW (read + convert), SET | 0.04 | 0.03999999910593033 | equal within tolerance; SET writes nothing (tolerant compare in apply and dry run) |
| `axis0.motor.config.torque_lim` | DOC | - | inf | shown; not relied on |
| `axis0.motor.effective_current_lim` | DOC | - | 10.0 | shown |
| `axis0.motor.config.pole_pairs` | PROF, SET | 15 | 15 (int32) | none (still TBD in PROF: config value, not a measurement) |
| `axis0.motor.config.phase_resistance` | PROF | 0.166 | 0.166108 | PROF -> measured |
| `axis0.motor.config.direction` | DOC | - | -1 (stale) | shown; never written; reset by encoder calibration |
| `axis0.motor.config.pre_calibrated`, `axis0.motor.is_calibrated` | FW, CAL | - | True, True | FW accepts; CAL skips motor calibration |
| `axis0.encoder.config.mode` | SET | 0 incremental | 0 | none |
| `axis0.encoder.config.cpr` | SET, CAL | 2400 placeholder | 2400 | none (TBD until the belt) |
| `axis0.encoder.config.use_index` | SET | False | False | none |
| `axis0.encoder.config.pre_calibrated`, `offset` | DOC | - | False, -9298 (stale) | shown |
| `axis0.encoder.config.calib_scan_distance` | CAL text ("about half a turn") | 16*pi | 50.265 rad electrical | consistent: 50.27/2pi/15 pp = 0.53 turn |
| `axis0.encoder.is_ready` | FW | - | False | FW refuses (ENCODER_NOT_READY) |
| `axis0.encoder.hall_state` | DOC | - | 7 (no Halls) | shown |
| `axis0.encoder.shadow_count`, `pos_estimate`, `vel_estimate` | DOC, measure_counts | exist | 0, 0.0, 0.0 | none |
| `axis0.controller.input_torque` | FW write target | exists | 0.0 (float) | none |
| `axis0.requested_state` | FW write target, CAL | exists | 0 (int32) | none |
| AxisState 1/4/7/8, ControlMode 1/3, InputMode 1/5 | FW, SET, CAL, DOC | 0.5.x enums | consistent with hardware.md (POSITION, TRAP_TRAJ) | INPUT_MODES table added |
| `config.dc_bus_overvoltage_trip_level` | SET | 30 | 56.0 | SET proposes 30 |
| `config.dc_bus_undervoltage_trip_level` | SET | 18 | 8.0 | SET proposes 18 |
| `config.dc_max_positive_current` | SET | 6 | 20.0 | SET proposes 6 |
| `config.max_regen_current` | PROF (check only) | (new) | 0.0 | check-only entry |
| `config.enable_ascii_protocol_on_usb` | DOC | - | True | shown |
| `axis0.config.startup_motor_calibration`, `_index_search`, `_homing`, `_sensorless_control` | DOC | False | False | none |
| `fw_version_*`, `hw_version_*` | DOC | - | 0.0.0 unreleased, 3.6-56 | shown + API generation guess |
| ASCII `f 0`, `r <path>` | default firmware | exist | not visible in a dump | unverified until bench step H3 |

## Still unverified (cannot be decided from a dump)
- Whether the watchdog is checked while IDLE (bring-up F3), whether ASCII `u 0` exists in this dev build,
  how `r` prints booleans over UART (parser accepts `0/1` and `True/False`).
- The brake resistor hardware (presence, value, power rating) and the right regen limits.
- The startup-flag contradiction (dump: True, hardware.md: False after a power cycle).
- Error-bit names outside the ones present in the dump.
