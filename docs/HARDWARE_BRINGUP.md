# Hardware bring-up guide (the day the hardware arrives)

Do the steps **in order**. Each step says what to do, which profile field it fills in, which
tool to run, what success looks like and what to do when it fails. All values go into **one
file**: `config/hardware_profile.toml`. After editing it, run `python3 tools/gen_config.py`
(or just rebuild: CMake re-runs it). `make status` shows what is still `TBD` and which step it
blocks.

Rules for the whole session:
- The default firmware is **read-only towards the ODrive** (it can only send `f 0` / `r ...`).
  Nothing in steps H0-H9 can make the motor move except the explicit calibration in **H7**.
- Keep a hand near the **24 V supply switch** whenever the ODrive is powered.
- In `odrivetool`, only **read** unless a step tells you to run a generated script.
- When a field is confirmed, change its `status` from `"TBD"` to `"measured"`.

The older, more detailed `docs/BENCH_CHECKLIST.md` describes the same checks for the
read-only build (LED meanings, what X should do). This guide adds the order and the tools.

---

## H0. Host checks (no hardware)
```bash
bash tools/check_all.sh          # or: make check
```
Success: last line `ALL CHECKS PASSED` (tests, 4 firmware configurations, binary scans).
Failure: open the log file named in the table.

Install the udev rule once so no `sudo` is needed for the HID tools:
```bash
sudo cp tools/99-steeringwheel.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger      # then replug USB USER
```

## H1. HID path, simulation build (STM32 only)
```bash
cd steeringwheel_firmware
cmake --preset Debug -DSTEERING_SOURCE=ODRIVE -DSIMULATE_INPUTS=ON -DWHEEL_FFB=OFF
cmake --build --preset Debug
openocd -f board/stm32f3discovery.cfg -c "program build/Debug/steeringwheel_firmware.elf verify reset exit"
```
Plug the **USB USER** connector, then:
```bash
python3 tools/hid_probe.py        # live view; X sweeps, buttons 1..10 walk, Y = Z = 0
python3 stm_32_wheel.py           # visual check (needs pygame)
```
Success: X sweeps 0..65535 every 4 s, buttons 1-10 light one after another.
Failure: `lsusb -d 0483:5752` empty -> USB cable on the wrong connector (ST-LINK vs USER);
`Permission denied` -> udev rule (H0).

## H2. ODrive doctor (read-only, ODrive on its own USB)
1. Power the ODrive from the 24 V supply, connect its USB to the PC.
2. With the official `odrive` Python package installed:
   ```bash
   python3 tools/odrive_status.py --live --dump-out docs/odrive_dump.txt
   ```
   Without it: `python3 tools/odrive_status.py --manual` prints the odrivetool commands; save
   the output and run `python3 tools/odrive_status.py --fixture <file> --prefix axis0`.
3. A first real dump is already committed as `docs/odrive_dump.txt` (2026-11-16) and is the
   reference for the real parameter names (`docs/archive/DUMP_RECONCILIATION.md`). Save a fresh one
   (`--dump-out`) after every configuration change; the generated scripts skip names that do
   not exist on your build.
4. The committed dump predates the owner's startup-flag change: it still shows both startup
   flags True and three errors (axis 0x100, motor 0x10, encoder 0x4) that the power cycle
   cleared. The flags are saved False (verified, `docs/hardware.md` line 30). **At the next ODrive
   power-up, make a fresh dump and replace `docs/odrive_dump.txt`** (never edit it by hand); then
   `python3 tools/odrive_status.py` must show the flags False and no errors.

Success: errors `0x0`, `current_state = 1 (IDLE)`, all startup flags `False`. Many `DIFF`
lines are expected now (factory limits); `FFB precheck would FAIL` is expected.
Failure: any error register non-zero -> note the decoded names, clear them in odrivetool
(`odrv0.clear_errors()`) and re-run. A startup flag `True` -> set it `False` before anything else.

## H3. UART wiring and position stream (default firmware)
1. Multimeter checks (`docs/BENCH_CHECKLIST.md` step 4): common GND, ODrive GPIO1 idles at
   about 3.3 V, PA9 idles at about 3.3 V. Then wire **PA9 -> GPIO2**, **PA10 <- GPIO1**.
2. Confirm the ODrive UART baud = `uart.baud` (115200) in the H2 dump
   (`config.uart_baudrate`, saved 115200 in `docs/odrive_dump.txt`).
3. Flash the default build (`-DSIMULATE_INPUTS=OFF -DWHEEL_FFB=OFF`), run `python3 tools/hid_probe.py`.

Success: LD5 (orange) goes **off**, X = 32768 at power-up and follows the encoder shaft.
Fill in: `pins.uart_tx`, `pins.uart_rx`, `uart.baud` -> `measured`.
Failure: LD5 stays on -> TX/RX swapped, no common GND, wrong baud, UART not enabled on the
ODrive, or the ODrive answers something non-numeric (see `docs/BENCH_CHECKLIST.md` step 5.7).

## H4. Encoder counts per encoder turn (gears apart or housing not involved)
1. In odrivetool read `odrv0.axis0.encoder.shadow_count` (raw count, independent of `cpr`).
2. Turn the **encoder shaft** exactly N full turns by hand (mark it; N = 3 or more), read again.
3. `python3 tools/measure_counts.py encoder --turns 3 --start <A> --end <B>`

Success: about 2400 counts per turn, no WARNING. Fill in `encoder.ppr`, `encoder.quadrature`
-> `measured`.
Failure: a different count -> the encoder is not 600 PPR or a channel is missing (count
1200 = one channel dead: check the A/B wires and the 5 V supply of the encoder).

## H5. Gear ratio and cpr (gear pair meshed)
The encoder is driven by a **20:20 gear pair** (external pinion on the encoder shaft, internal gear on
the motor housing; counted by the user 2026-11-16, `belt.teeth_housing = belt.teeth_pulley = 20`,
status `measured`; ratio 1:1, expected `cpr = 2400`). The section is still called `[belt]` in the profile.
1. The tooth counts are entered. **Still verify the real counts per turn:** rotate the **motor housing**
   exactly 10 full turns (mark it) and read `shadow_count` before and after: expect about **24000** counts
   (an earlier rough hand count gave about 5000 per turn, which would be wrong for 1:1).
   Check `motor.motor_turns_per_wheel_rev` (1.0 for direct drive).
2. `python3 tools/gen_config.py --derived` -> `cpr` must be an **integer** (loud warning otherwise).
3. Turn the **steering wheel** (= motor housing) exactly N full turns clockwise (N = 10 recommended,
   at least 2), reading `shadow_count` before and after:
   `python3 tools/measure_counts.py wheel --turns 10 --start <A> --end <B> --current-cpr 2400`

Success: no WARNING; `cpr` equals the derived one. The gear fields are already `measured` (teeth counted); this step confirms them on the bench.
The output also says the direction: if `shadow_count` went **negative** while turning
clockwise, set `wheel.invert = true` (confirmed again in H8).
Failure: `implied tooth ratio ... does not match` or about 5000 counts per turn -> re-count both gears
(an internal gear and its pinion normally differ clearly in tooth count) and check the mesh for slipping or
skipping. Do not continue to H7 until the measured `cpr` is an integer equal to the profile.

## H6. Gentle ODrive profile
(2026-11-16: the profile now proposes `dc_max_negative_current = -1.5 A` for the 50 W / 2 ohm brake resistor and `torque_constant = 0.5`, the live value; use `--read` so the diff is against the live ODrive, not the old dump.)
1. Review the `[odrive]` section of the profile (current limit, vel limit, voltage trips,
   watchdog, torque constant). Regenerate: `python3 tools/gen_config.py`.
2. Look first: `python3 tools/generated/odrive_setup.py` (dry run, prints the commands) and
   `python3 tools/generated/odrive_setup.py --read` (connects, shows old -> new, writes nothing).
3. Apply (RAM only): `python3 tools/generated/odrive_setup.py --apply`
   It refuses unless the axis is IDLE and `vbus_voltage` is inside the profile window, prints
   every old/new value and verifies by reading back. It never requests motion.
4. When happy: `python3 tools/generated/odrive_setup.py --apply --save` (the ODrive reboots).
5. `python3 tools/odrive_status.py --live` -> no `DIFF` lines.

Fill in: the `[odrive]` entries -> `measured` once they are confirmed on the device.
Failure: `SKIP ... (not present on this firmware)` -> your 0.5.x build names that property
differently; look it up in `docs/odrive_dump.txt` and adjust the path in `tools/gen_config.py`
(`setup_settings()`), then regenerate.

## H7. Motor and encoder offset calibration (THE MOTOR MOVES)
Prerequisite: H5 measured `cpr` equals the profile (2400).
**Remove the steering wheel or keep everything clear. Hand on the 24 V switch.**
```bash
python3 tools/generated/odrive_calibrate.py           # asks you to type: I AM CLEAR
```
It runs MOTOR_CALIBRATION if needed (beep, no rotation), then ENCODER_OFFSET_CALIBRATION
(the rotor turns roughly half a turn one way and back for several seconds), then IDLE.
Ctrl+C requests IDLE immediately.

Success: `encoder.is_ready = True`, all errors 0. Then `python3 tools/odrive_status.py --live`.
Record `motor.phase_resistance` from the dump (documentation).
With `--save` the script also sets `motor.config.pre_calibrated = True` and saves.
Note: the LPD3806 has no index pulse. With 0.5.x the **encoder** offset calibration is then
very probably needed after every ODrive power-up (TODO(TBD): confirm). The FFB precheck
refuses to arm while `encoder.is_ready` is false, so a missing calibration cannot be missed.
**Bench result 2026-11-17: the calibration needs `calibration_current` 5 A with `current_lim` 6 A.** At 3 A and at 4 A it fails
with `CPR_POLEPAIRS_MISMATCH` (the 4 kg rotor does not follow the field closely enough; the cpr was already measured as
2398.7 counts per turn and is not the cause); at 5 A / 6 A it passes (repeated twice, rotor turns about 180 deg and
back). `odrive_calibrate.py` therefore refuses (it requires current_lim <= profile 4 A). Per-session procedure until the
script is changed: power up the ODrive (it comes up with the SAVED values 6 A / 5 A, nothing was ever saved with
`--save`), the STM32 running and feeding the watchdog, then in `odrivetool`: `odrv0.axis0.clear_errors()`,
`odrv0.axis0.motor.config.current_lim = 6.0`, `odrv0.axis0.motor.config.calibration_current = 5.0`,
`odrv0.axis0.requested_state = 7`, wait ~10 s, check `current_state 1`, `axis0.error 0`, `encoder.error 0`,
`encoder.is_ready True`; close odrivetool (it holds the USB interface) and apply the profile again with
`odrive_setup.py --apply` (back to 4 A). Do NOT `--save` the watchdog setting before the STM32 feeds it (see F3).

Failure: `CPR_POLEPAIRS_MISMATCH` -> wrong `cpr` (go back to H5), wrong `pole_pairs`, or too little calibration current (see above);
`PHASE_RESISTANCE_OUT_OF_RANGE` -> phase wire loose, or `calibration_current` too low/high.

## H8. Steering direction and lock-to-lock (default firmware)
1. Flash the default build again (it uses the regenerated header). Power-up position = centre.
2. **Bench step (direction is NOT decided by the gear count):** `python3 tools/hid_probe.py`, turn the
   wheel **clockwise** (seen from the driver) and watch X. X must **increase**; if it decreases set
   `wheel.invert = true`, rebuild, repeat. Only then mark `wheel.invert` as `measured`. (An external pinion
   in an internal gear turns the same way as the housing, so the default may be right, but check.)
3. `wheel.lock_to_lock_deg` is 900 (owner decision 2026-11-16, `assumed`): check that X reaches 0 / 65535
   exactly at half that angle each side. Mark `wheel.*` `measured`. Update `MAX_ANGLE` in
   `stm_32_wheel.py` (= lock_to_lock / 2 = 450, already set) for the visualiser.
4. Hold **B1** for 1 s away from centre: LD9 toggles, X jumps to 32768.

## H9. MCP23017 buttons
See `docs/BENCH_CHECKLIST.md` step 7 (wiring, LD10, `evtest`). Mark `mcp23017.address_7bit`
and `pins.i2c` `measured`.

---

## F. Force feedback (only after H0-H9, and only with the owner's decision)
Arm gesture without paddle buttons: build with `-DWHEEL_FFB_ARM_B1_ONLY=1` (B1: tap, tap, hold 3 s; weaker, wheel dismounted only), see `docs/FFB_SAFETY.md` section 3.
Everything about FFB is in **`docs/FFB_SAFETY.md`**: the safety case, what is and what is NOT
protected, the mandatory order (F1 ... F8: wheel dismounted, low current, hand on the power
plug) and the exact build flags (`-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=1`).
Before F1, `make status` must show **no TBD entry** in the safety sections
(belt/encoder/motor/odrive/wheel/uart): the FFB firmware refuses to arm otherwise
(`PROFILE_TBD`, `FFB_REQUIRE_MEASURED_PROFILE` in `Core/Inc/ffb/ffb_config.h`).

### F3 (open question to settle early): does the ODrive watchdog run while IDLE?
With `enable_watchdog = True` saved (H6), power-cycle the ODrive with **nothing** feeding it
and wait 5 s, then read `odrv0.axis0.error` in odrivetool.
- `0x0`: the watchdog only runs while the axis is active. Nothing to do.
- `WATCHDOG_TIMER_EXPIRED (0x800)`: your build checks it while IDLE too. The FFB precheck
  will then refuse (`AXIS_ERROR`) because the firmware never clears errors. Options, decided
  by the owner: build with `-DFFB_FEED_WATCHDOG_WHEN_DISARMED=1` (the STM32 feeds `u 0` from
  boot; `u` does not move anything), or clear errors in odrivetool right before arming.

**Bench result 2026-11-17 (F3/F4):** this build checks the watchdog **while IDLE too**: right after `enable_watchdog = True`
the axis shows `WATCHDOG_TIMER_EXPIRED (0x800)`. Chosen option: the STM32 feeds `u 0` from boot. In `cmake` the flag is
`-DWHEEL_EXTRA_DEFINES=FFB_FEED_WATCHDOG_WHEN_DISARMED=1` (there is no cmake option of that name). Rules learned: enable the
ODrive watchdog only while the STM32 is already feeding, and run `odrv0.axis0.clear_errors()` once afterwards (the first
expiry stays latched); the error then stays 0. A one-shot `odrv0.axis0.watchdog_feed()` from odrivetool did NOT keep the error
away on this build (even with a 5 s timeout), only the continuous `u 0` over the UART does. F4 passed: with the axis in closed
loop (torque 0, requested from odrivetool) the PA9 wire was pulled and the axis dropped to IDLE with 0x800 (exact timing not
measured). `odrive_calibrate.py`'s preflight message mentions `odrv0.clear_errors()`; on this build it is
`odrv0.axis0.clear_errors()` (the device-level method does not exist).

**Bench result 2026-11-17 (F1, ASCII reads):** the first FFB build with ARM_ALLOWED=1 showed `prearm=READ_ERROR` for every
read. Cause, found by sending the same `r <path>` reads to the ODrive CDC port (/dev/ttyACM0, read-only probe): this build
prints `axis0.error` as a decimal integer plus one letter `d` (`0d`, `2048d` = 0x800), while all other integers are plain
(`axis0.motor.error` = `0`) and floats are `4.000000`. The strict parser rejected the `d`. `odr_parse_single_line()` now accepts
exactly one `d` directly after a plain unsigned integer (tests `test_single_line_parser_axis_error_suffix`). Also seen on the
CDC port: late `f 0` replies ("-4.528744 0.000000") arriving as answers to other reads (handled as late position replies),
and one lost reply in 39 (302 ms, two ASCII clients at once).

**Bench result 2026-11-17 (F5, the first arm):** (1) the ODrive velocity limiter clips the torque. With
`enable_current_mode_vel_limit = True` the torque limit is `(vel_limit - speed) * vel_gain`; the saved `vel_gain` 0.02 and the profile's
`vel_limit` 2 gave 0.04 Nm at rest, so the motor delivered ~0.04 Nm although 0.2373 Nm reached `input_torque` (read over the CDC
port: `torque_setpoint 0.2373`, `Iq_setpoint 0.0822 A` = 0.041125 Nm / 0.5, exactly (2 + 0.05625) * 0.02). The spring was not
felt, even at 0.5 A. `vel_gain` is now a profile entry (0.25, written by `odrive_setup.py`) and `gen_config` refuses a profile
where `vel_limit * vel_gain < ffb.max_amps * torque_constant`. With 0.25 the spring pulled and the housing returned to the
centre by itself: the sign of the force is right. The limiter also tapers the torque to 0 as the speed reaches `vel_limit` (a soft
speed limit; revisit for the mounted wheel). (2) In PRECHECK/ARMING a queued read replaced the `f` position request, so arming
ended in POSITION_STALE; fixed (`FFB_PRECHECK_READ_EVERY`). (3) The B1 gesture: LD4 starts to blink about 0.6 s into the third press
but the arm fires only after 3 s of that press; releasing earlier cancels silently.

**Bench result 2026-11-17 (strength and feel, tools/ff_play.py):** at 100 percent of a 0.5 A cap the effects were barely perceptible.
Cause 1: the 0.5 A / 0.25 Nm scale is small against the hub motor's friction and cogging (raised to `ffb.max_amps` 0.8 A, spring 1.5 A/turn,
also set `FFB_SPRING_ONLY_WITHOUT_HOST=0` through `WHEEL_EXTRA_DEFINES` so the centring spring stays on while a game plays effects; since 2026-11-18 this is the profile entry `ffb.spring_only_without_host` and the define must NOT be given, it would override the profile).
Cause 2 (the main one): `ffb.slew_amps_per_s` 5 limited the rate of change of ALL forces; a 12.5 Hz 0.8 A sine came out as about
+-0.1 A (= slew * period / 4) and a 150 ms pulse only ramped up. With 50 A/s the log shows a 0.8 A pulse in one sample, a +-0.8 A sine at
12.5 Hz ("like cobblestones", felt well) and a +-0.25 A sine at 40 Hz ("buzzing"). The emergency stop does not use the limiter. Next
optional steps: 100 A/s, then `max_amps` 1.0 A (the code ceiling); more than 1 A needs a reviewed safety case. Expect no fine gravel
texture from this hardware (200 Hz command rate, friction); impacts, bumps and self-aligning force are realistic.

## What is verified only on the host (not on hardware)
- All tools above: unit tests against the REAL dump `docs/odrive_dump.txt` (plus truncated and
  corrupted copies) and a fake ODrive object built from it.
- The generated `odrive_setup.py` / `odrive_calibrate.py` were **never run** against an ODrive.
- Property names come from the ODrive 0.5.6 documentation; your unnumbered 0.5.x build may differ.
