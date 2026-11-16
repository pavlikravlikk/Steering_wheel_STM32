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
   reference for the real parameter names (`docs/DUMP_RECONCILIATION.md`). Save a fresh one
   (`--dump-out`) after every configuration change; the generated scripts skip names that do
   not exist on your build.
4. Known from that dump: `axis0.error` 0x100 ENCODER_FAILED, `motor.error` 0x10
   CONTROL_DEADLINE_MISSED, `encoder.error` 0x4 NO_RESPONSE (expected: no belt, so the startup
   offset calibration fails), and **`startup_closed_loop_control` / `startup_encoder_offset_calibration`
   = True in the dump** although `docs/hardware.md` says they were saved False. Re-check them
   first; they must be False.

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

## H4. Encoder counts per encoder turn (belt OFF)
1. In odrivetool read `odrv0.axis0.encoder.shadow_count` (raw count, independent of `cpr`).
2. Turn the **encoder shaft** exactly N full turns by hand (mark it; N = 3 or more), read again.
3. `python3 tools/measure_counts.py encoder --turns 3 --start <A> --end <B>`

Success: about 2400 counts per turn, no WARNING. Fill in `encoder.ppr`, `encoder.quadrature`
-> `measured`.
Failure: a different count -> the encoder is not 600 PPR or a channel is missing (count
1200 = one channel dead: check the A/B wires and the 5 V supply of the encoder).

## H5. Belt ratio and cpr (belt ON)
1. Count the teeth on the housing gear and on the encoder pulley; enter them in
   `belt.teeth_housing` / `belt.teeth_pulley`. Check `motor.motor_turns_per_wheel_rev`
   (1.0 for direct drive).
2. `python3 tools/gen_config.py --derived` -> `cpr` must be an **integer** (loud warning otherwise).
3. Turn the **steering wheel** exactly N full turns clockwise (N >= 2), reading `shadow_count`
   before and after:
   `python3 tools/measure_counts.py wheel --turns 2 --start <A> --end <B> --current-cpr 2400`

Success: no WARNING; `cpr` equals the derived one. Mark the belt fields `measured`.
The output also says the direction: if `shadow_count` went **negative** while turning
clockwise, set `wheel.invert = true` (confirmed again in H8).
Failure: `implied tooth ratio ... does not match` -> re-count the teeth or check for belt slip.

## H6. Gentle ODrive profile
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
Failure: `CPR_POLEPAIRS_MISMATCH` -> wrong `cpr` (go back to H5) or wrong `pole_pairs`;
`PHASE_RESISTANCE_OUT_OF_RANGE` -> phase wire loose, or `calibration_current` too low/high.

## H8. Steering direction and lock-to-lock (default firmware)
1. Flash the default build again (it uses the regenerated header). Power-up position = centre.
2. `python3 tools/hid_probe.py`: turning the wheel **clockwise must increase X**; otherwise
   set `wheel.invert = true`, rebuild.
3. Decide `wheel.lock_to_lock_deg` (540 / 900 / 1080) and check that X reaches 0 / 65535
   exactly at half that angle each side. Mark `wheel.*` `measured`. Update `MAX_ANGLE` in
   `stm_32_wheel.py` (= lock_to_lock / 2) for the visualiser.
4. Hold **B1** for 1 s away from centre: LD9 toggles, X jumps to 32768.

## H9. MCP23017 buttons
See `docs/BENCH_CHECKLIST.md` step 7 (wiring, LD10, `evtest`). Mark `mcp23017.address_7bit`
and `pins.i2c` `measured`.

---

## F. Force feedback (only after H0-H9, and only with the owner's decision)
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

## What is verified only on the host (not on hardware)
- All tools above: unit tests against the REAL dump `docs/odrive_dump.txt` (plus truncated and
  corrupted copies) and a fake ODrive object built from it.
- The generated `odrive_setup.py` / `odrive_calibrate.py` were **never run** against an ODrive.
- Property names come from the ODrive 0.5.6 documentation; your unnumbered 0.5.x build may differ.
