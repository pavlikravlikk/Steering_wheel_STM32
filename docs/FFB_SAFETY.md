# Force feedback: safety case, limits and bring-up

**Status: written and tested on the host only. Nothing in this document has been run on the
real motor.** The FFB code has never commanded a real ODrive. Treat the first power-up with
FFB as an experiment with a 4 kg rotor that has **no mechanical end stop**.

## 1. What exists

| Build | Can write to the ODrive? | How |
|---|---|---|
| default (`WHEEL_FFB=OFF`) | **No.** Only `f 0` / `r <path>` can leave the UART (allowlist). No write code is compiled; `tools/check_all.sh` proves the binary contains no write strings. | `cmake --preset Debug` |
| `WHEEL_FFB=ON`, `WHEEL_FFB_ARM_ALLOWED=0` (FFB default) | Never arms. The arm gesture is refused (`ARM_NOT_ALLOWED`). USB HID PID is active, so games see a force-feedback wheel, but no torque is ever sent. | `-DWHEEL_FFB=ON` |
| `WHEEL_FFB=ON`, `WHEEL_FFB_ARM_ALLOWED=1` | Can arm after **all** conditions in section 3. | `-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=1` |

Code: `Core/Src/ffb/` and `Core/Inc/ffb/` (compiled only with `WHEEL_FFB=ON`).

## 2. The only things the firmware can write

`Core/Src/ffb/ffb_cmd.c` (`ffb_cmd_is_allowed()`), checked again inside
`odrive_uart_send_ffb()`, the only write entry point:

1. `w axis0.controller.input_torque <value>` with `<value>` = `[-]d.dddd`,
   |value| <= `FFB_HARD_CAP_AMPS * FFB_TORQUE_CONSTANT_MAX` (1 A x 2 Nm/A).
2. `w axis0.requested_state 1` (IDLE) and `w axis0.requested_state 8` (CLOSED_LOOP), nothing else.
3. `u 0` (feed the watchdog).

The firmware **never** writes configuration (`config.*`, `startup_*`, `current_lim`,
`vel_limit`, `control_mode`, watchdog settings, `torque_constant`), never clears errors and
never calls `save_configuration`. It only **reads** those values to decide whether it may
arm. (Tests: `tests/test_ffb_core.c` allowlist cases, `tests/test_safety.c`, 17 mutation tests.)

## 3. What must be true before the motor can be driven

All of these, in this order:

1. Built with `WHEEL_FFB=ON` **and** `WHEEL_FFB_ARM_ALLOWED=1`.
2. Not a `SIMULATE_INPUTS` build.
3. No `TBD` left in the safety sections of `config/hardware_profile.toml`
   (`belt`, `encoder`, `motor`, `odrive`, `wheel`, `uart`). Override only by building with
   `-DFFB_REQUIRE_MEASURED_PROFILE=0` (not recommended).
4. **Arm gesture:** hold **B1 (blue) + left paddle + right paddle** together for 3 s
   (`ffb.arm_hold_ms`). All three must first have been released (a stuck button at power-up
   cannot arm). Releasing any of them cancels.
   **Bench WITHOUT paddle buttons (2026-11-16, owner decision):** build with `-DWHEEL_FFB_ARM_B1_ONLY=1`
   (default 0). Then **B1 alone** can arm with a deliberate sequence: **tap B1 twice** (each press shorter than
   0.6 s, gap between presses under 1.5 s), then **press B1 a third time and HOLD it for 3 s** (LD4 blinks fast during
   the hold). Fail-safe parts: B1 must be seen released first (held at power-up = never arms); exactly two taps
   (one, three, too-slow or too-long taps never arm); releasing early cancels; fires once, the next arm starts over;
   set-centre is blocked during the final hold. All the other arming conditions (precheck, watchdog, limits...) are
   unchanged. **This is weaker than B1 + both paddles** (one button, on the controller board): use it only for the first
   tests with the steering wheel DISMOUNTED (F4-F7), and rebuild with the default (3-button gesture, add paddle
   buttons) before the wheel is mounted (F8). Code: `ffb_b1seq_*` in `ffb_ui.c`, tests `test_b1seq_*` and
   `test_svc_b1_only_mode_*`.
   Optional (off by default, `FFB_ARM_PINS=1` through `-DWHEEL_EXTRA_DEFINES=FFB_ARM_PINS=1`): two push buttons PD0 / PD1
   to GND act as the two paddles (`ffb_buttons_with_arm_pins()`); nothing is configured on those pins unless enabled.
5. **Precheck** (read-only, all values re-read after the gesture, within 2 s):
   `axis0.config.enable_watchdog` = True; `watchdog_timeout` inside
   [`ffb.watchdog_timeout_min_s`, `ffb.watchdog_timeout_max_s`];
   `controller.config.control_mode` = 1 (torque); `motor.config.current_lim` <= `odrive.current_lim`
   of the profile; `controller.config.vel_limit` <= `odrive.vel_limit`; `torque_constant`
   in [0.001, 2.0]; `motor.is_calibrated`; `encoder.is_ready`; `axis0.error`, `motor.error`,
   `encoder.error` all 0; `axis0.current_state` = 1 (IDLE); `vbus_voltage` in
   [`ffb.vbus_min_v`, `ffb.vbus_max_v`]; fresh position. All names verified against
   `docs/odrive_dump.txt` (`tests/test_ffb_dump.c`).
6. **ARMING:** `input_torque 0`, then `requested_state 8`, watchdog fed. The axis must report
   `current_state = 8` within 1 s (`FFB_VERIFY_CLOSED_LOOP`), else FAULT.
7. **RUNNING.** Any fault -> FAULT (section 4).

If a check fails the wheel stays DISARMED and shows the reason (section 6).

## 4. What protects against what

| Hazard | Protection | Where |
|---|---|---|
| Accidental motor command in the normal build | No write code compiled; binary scan; source scan; tests | `check_all.sh`, `test_safety.c` |
| Firmware writes a wrong setting | Writes restricted to 3 command kinds by an allowlist | `ffb_cmd.c` |
| Too much current | Total command clamped to `ffb.max_amps` (<= 1 A hard cap in code); the ODrive's own `current_lim` must be <= the profile or no arming | `ffb_torque.c`, precheck |
| Sudden jumps | Slew-rate limit (`ffb.slew_amps_per_s`, 5 A/s); RUNNING starts from 0 A | `ffb_torque.c` |
| Wheel spinning away (no end stop, 4 kg) | Soft end stops beyond +-lock/2 with damping; OVERSPEED fault at `ffb.overspeed_turns_s`; the ODrive's `vel_limit` (<= 2 turns/s) with `enable_current_mode_vel_limit` and `enable_overspeed_error` (names in this build) | `ffb_torque.c`, `ffb_safety.c`, ODrive |
| STM32 hangs or the UART is unplugged | ODrive **watchdog** (required by the precheck) stops the axis after `watchdog_timeout` without `u 0`; on the STM32 side UART_TIMEOUT / POSITION_STALE faults | ODrive, `ffb_safety.c` |
| Position data stale or garbage | POSITION_STALE after `ffb.stale_ms` (50 ms); strict number parser | `ffb_safety.c`, `odrive_ascii.c` |
| ODrive reports an error / leaves closed loop | Errors and `current_state` are re-read while running (every 3rd cycle); any change -> FAULT | `ffb_precheck.c` |
| Supply voltage out of range | `vbus_voltage` monitored -> VBUS_RANGE fault; ODrive trips at the profile's over/under-voltage levels | precheck, ODrive |
| Main loop stalls | LOOP_OVERRUN fault if a cycle is late by > 25 ms | `ffb_safety.c` |
| A game sends nonsense | Reports validated (ID, type, length, ranges); total game force limited to full scale and then to `ffb.max_amps`; disable-actuators / stop-all / reset / USB unplug or suspend -> no game force | `ffb_pid.c`, `ffb_effects.c` |
| Fault recovery hides a problem | FAULT is latched until power cycle; it re-sends `input_torque 0` + IDLE every 500 ms | `ffb_safety.c` |
| User wants it to stop | Press **B1** (alone): torque 0 + IDLE at once (DISARMED) | `ffb_ui.c` |

## 5. What is NOT protected (read this)

- **There is no hardware emergency stop yet.** The software paths above all depend on the
  STM32 and the ODrive firmware working. The only independent protection is the ODrive
  watchdog and your hand on the 24 V switch. **Fit an e-stop that cuts the motor supply
  before using FFB with the wheel mounted.**
- With 4 kg of rotating mass and no mechanical stop, the wheel can hurt fingers, cables or
  wrists, even at 1 A. The soft end stop is only a force, not a stop.
- Commutation depends on the gear-driven encoder (20:20 pinion + internal gear). A slipping, skipping or
  backlash-affected gear mesh gives wrong
  commutation: the motor can produce torque in an unexpected direction. Monitor the encoder
  error and the gear mesh; never run FFB with a loose or skipping gear.
- The LPD3806 has no index pulse: the encoder offset calibration is (probably) needed after
  every ODrive power-up, and **the centre is the power-up position**. End stops are relative
  to that centre; power up with the wheel straight.
- Brake/regen (from the REAL dump): `dc_max_negative_current = -3.0 A`, a **2 ohm brake
  resistor armed automatically** (`brake_resistor_armed True`, no enable switch in this build)
  and `max_regen_current = 0.0` (no regen current into the PSU allowed). Back-driving the
  motor dumps energy into that resistor. Its label reads 50 W / 2 ohm (user, 2026-11-16); heat sink,
  wiring and the peak power (about 70-90 W at -3 A, above 50 W continuous) are unverified (TBD). The Mean Well RSP-320-24 cannot sink current: if the resistor path does not absorb
  the energy, the bus rises until the over-voltage trip (56 V saved, 30 V in the gentle
  profile). Choosing the regen limits is a human bring-up decision (`docs/DECISIONS.md`).
- The direction of game forces (`FFB_INVERT_HOST_FORCE`) and of the spring sign were derived
  from the conventions, not measured. Verify with small values (F5, F6).
- Unknowns about the unnumbered ODrive 0.5.x build: whether the watchdog runs while IDLE
  (F3), the exact property names (the precheck fails safe if one is missing: READ_ERROR),
  the ASCII format of booleans (both `1`/`0` and `True`/`False` are accepted).

## 6. Reading the status

LEDs (Discovery board, FFB builds):

| LED | Meaning |
|---|---|
| LD6 green | off = not ready; 1 Hz blink = ready to arm; fast blink = PRECHECK/ARMING; solid = RUNNING |
| LD8 orange | blink code, N flashes then a pause: DISARMED -> pre-arm reason N; FAULT -> fault N |
| LD4 blue | solid = FAULT latched (power-cycle the STM32); fast blink = arm gesture being held |

Pre-arm reasons (N): 1 ARM_NOT_ALLOWED, 2 PROFILE_TBD, 3 SIMULATION_BUILD, 4 POSITION_STALE,
5 READ_TIMEOUT, 6 WATCHDOG_DISABLED, 7 WATCHDOG_TIMEOUT_RANGE, 8 CONTROL_MODE,
9 CURRENT_LIM_HIGH, 10 VEL_LIMIT_HIGH, 11 MOTOR_NOT_CALIBRATED, 12 ENCODER_NOT_READY,
13 AXIS_ERROR, 14 MOTOR_ERROR, 15 ENCODER_ERROR, 16 VBUS_RANGE, 17 TORQUE_CONSTANT_RANGE,
18 NOT_CHECKED_YET, 19 READ_ERROR, 20 NOT_IDLE (axis not IDLE before arming, e.g.
because `startup_closed_loop_control` is True).
Faults (N): 1 POSITION_STALE, 2 ODRIVE_ERROR, 3 UART_TIMEOUT, 4 VBUS_RANGE,
5 WATCHDOG_FEED_FAILED, 6 TORQUE_INVALID, 7 OVERSPEED, 8 ARMING_TIMEOUT, 9 LOOP_OVERRUN,
10 READ_FAILED, 11 INTERNAL.

On the PC: `python3 tools/hid_probe.py --status` reads the vendor feature report 0x40
(state, reason, fault, commanded current, command rate, loop time, ODrive timeouts, vbus).

## 7. Timing (UART 115200)

One control cycle = one ODrive command cycle = `ffb.command_period_ms` (5 ms, 200 Hz):
position/property request first (`f 0`, 4 bytes, reply about 20 bytes on the other wire),
then `w axis0.controller.input_torque -0.1234` (39 bytes) and `u 0` (4 bytes): about
48 bytes = 4.2 ms of 5 ms (83 %). **1 kHz is not possible at 115200 baud**;
`tools/gen_config.py` refuses periods above 90 % UART load. At 921 600 baud (future profile
setting `uart.baud`, must be set in the ODrive too) 1 ms would use 52 %. If the queue is full
the cycle's writes are refused and counted; 3 in a row -> WATCHDOG_FEED_FAILED fault.
Measured rate and the worst cycle time are in the status report.

## 8. Mandatory bring-up order (F-steps; after docs/HARDWARE_BRINGUP.md H0-H9)

Keep a hand on the 24 V switch for every F-step. Stop at the first surprise.

- **F0 Host:** `bash tools/check_all.sh` -> ALL CHECKS PASSED.
- **F1 FFB build that cannot arm:** build `-DWHEEL_FFB=ON` (ARM_ALLOWED=0), flash. Linux:
  `dmesg | grep -i -E "pidff|force"` must show the force-feedback driver accepted the device
  (no "unknown set_effect report layout" or "... missing"); `lsusb -v -d 0483:5752` shows the
  HID report descriptor length (> 255). `python3 tools/hid_probe.py --status` -> DISARMED,
  reason ARM_NOT_ALLOWED. Do the arm gesture: nothing happens, LD8 shows 1 flash.
- **F2 Precheck reasons with the real ODrive (still ARM_ALLOWED=0):** apply the gentle
  profile (H6), calibrate (H7). The status must walk through the reasons as you fix them;
  `python3 tools/odrive_status.py --live` must say "FFB precheck would PASS" except the
  ARM_NOT_ALLOWED gate.
- **F3 Watchdog while IDLE?** See `docs/HARDWARE_BRINGUP.md` F3. Decide
  `FFB_FEED_WATCHDOG_WHEN_DISARMED`.
- **F4 Watchdog really stops the motor (odrivetool only, wheel dismounted):** with the
  STM32 unplugged, in odrivetool: `odrv0.axis0.controller.input_torque = 0`,
  `odrv0.axis0.requested_state = 8`, do not feed -> within `watchdog_timeout` the axis must
  go to IDLE with WATCHDOG_TIMER_EXPIRED. If it does not, **do not continue**.
- **F5 First arm, wheel DISMOUNTED, gains small:** in the profile set `ffb.max_amps = 0.3`,
  `ffb.spring_kp_amps_per_turn = 0.2`, rebuild with `-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=1`
  (without paddle buttons add `-DWHEEL_FFB_ARM_B1_ONLY=1`, section 3),
  flash, power the ODrive, do the gesture (B1 only: tap, tap, hold 3 s). Expect LD6 fast blink then solid. Turn the rotor
  by hand: it must pull back towards centre (gently). If it pushes away, the sign is wrong:
  press B1, power off, investigate (encoder direction / calibration) - do not "fix" it by
  flipping signs blindly.
- **F6 Each fault on purpose:** unplug PA10 (POSITION_STALE), unplug PA9 (ODrive watchdog
  + UART_TIMEOUT), spin fast by hand (OVERSPEED), press B1 (disarm). Every case: torque goes
  to 0 and the axis to IDLE; FAULT needs a power cycle.
- **F7 Game force (wheel still dismounted, gain low):** `fftest /dev/input/eventN` (from
  linuxconsoletools): constant force, then spring, then sine. Check the direction of the
  constant force; set `FFB_INVERT_HOST_FORCE` if it pulls the wrong way. `evtest` shows the
  FF capabilities. Then a game, with its FFB strength at 20 %.
- **F8 Wheel mounted:** only after F0-F7 and with the hardware e-stop fitted. Raise
  `ffb.max_amps` in small steps; the code ceiling stays 1 A (`FFB_HARD_CAP_AMPS`) until a
  reviewed decision.

## 9. Verified only against the spec / host tests

- HID PID descriptor: parsed by our own parser in `tests/test_ffb_desc.c` against the USB
  PID 1.0 usage table and the hid-pidff requirements (required reports, nested arrays with
  Logical Minimum 1, one usage per element, ms time units, same effect-type order in Set
  Effect and Create New Effect, block index range). **Not yet enumerated by a real Linux
  kernel.**
- The USB class driver (`usbd_ffb_hid.c`): compiled, not run. In particular the GET_REPORT
  / SET_REPORT paths and the class swap in `usbd_ffb_install()` need F1.
- The effect engine: unit tests per effect type, envelope, timing, gains, device control
  and random input. Feel and direction: not verified.
- The ODrive side: a simulated ODrive in `tests/test_ffb_service.c`; real reply timing,
  property names and boolean format are unverified.
