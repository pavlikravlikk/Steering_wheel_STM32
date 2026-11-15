# Bench checklist (in this order)

Everything here was written without hardware. Each step lists **what to do** and **what you
should see**. Stop at the first step that does not match.

The firmware is **read-only towards the ODrive**: the only bytes it can transmit are
`f 0\n` (and, by the allowlist, `r <path>\n`). It cannot change the ODrive state, torque,
setpoints or configuration. Still, keep a hand near the 24 V supply switch whenever the
ODrive is powered.

---

## 0. Host side (no hardware)
1. `make -C tests` → last line `... tests, 0 failed`.
2. Build:
   ```bash
   cd steeringwheel_firmware
   cmake --preset Debug -DSTEERING_SOURCE=ODRIVE -DSIMULATE_INPUTS=OFF
   cmake --build --preset Debug
   ```
   **Important:** an existing `build/Debug` keeps the *old* cached `STEERING_SOURCE=ENCODER`.
   Pass `-DSTEERING_SOURCE=ODRIVE` once (as above) or delete `build/Debug/CMakeCache.txt`.
   The configure output does not print the source; check with
   `grep STEERING_SOURCE build/Debug/CMakeCache.txt`.

## 1. HID path with nothing wired (simulation build)
1. `cmake --preset Debug -DSIMULATE_INPUTS=ON && cmake --build --preset Debug`, flash
   (`openocd -f board/stm32f3discovery.cfg -c "program build/Debug/steeringwheel_firmware.elf verify reset exit"`),
   plug the **USB USER** port.
2. `lsusb -d 0483:5752` → device present. `sudo lsusb -v -d 0483:5752` → `bInterval 1`,
   `wDescriptorLength 42`.
3. `jstest --event /dev/input/js0` (or `sudo evtest`) → X sweeps (4 s sine), buttons 1…10
   pulse one after another (they now pass through the 5 ms debouncer — no visible difference).
   Y and Z stay at 0.
4. `python stm_32_wheel.py` → wheel animates, buttons light up. (The report format is
   unchanged, the script needs no update.)
5. LEDs: LD3 blinks 1 Hz, LD7 on. LD10 must **not** blink in simulation.

## 2. Default build, ODrive NOT connected
1. Rebuild with `-DSIMULATE_INPUTS=OFF -DSTEERING_SOURCE=ODRIVE`, flash, plug USB USER.
2. Expect: LD3 blinking, LD7 on, **LD5 (orange) on** (no ODrive data), **LD10 blinking**
   (no MCP23017), X steady at **32768**, no buttons.
3. Hold the blue **B1** for > 1 s → nothing happens (centre is refused without ODrive data;
   LD9 must *not* toggle).

## 3. ODrive side, before wiring (odrivetool over the ODrive's USB, read-only)
1. ODrive powered from its 24 V supply. In `odrivetool` *only read*:
   `odrv0.axis0.current_state` → `1` (IDLE); `odrv0.axis0.error`, `.motor.error`,
   `.encoder.error` → `0`; `odrv0.axis0.config.startup_closed_loop_control` and
   `startup_encoder_offset_calibration` → `False`.
2. Confirm the UART is enabled at 115200 on GPIO1/GPIO2. The property names differ between
   0.5.x builds (TBD for your unnumbered firmware), e.g. `odrv0.config.enable_uart_a` /
   `uart_a_baudrate`, or `enable_uart` / `uart_baudrate` on older 0.5.x.
3. Optional, isolates the ODrive from the STM32: a **3.3 V** USB-UART adapter on GPIO1/GPIO2/GND,
   `picocom -b 115200 --omap crlf --echo /dev/ttyUSB0`, type `f 0` + Enter → a line like
   `0.000000 0.000000`. Type only `f 0` / `r ...` commands.

## 4. UART wiring (multimeter — this was marked TBD)
With both boards powered but the signal wires **not** yet connected:
1. GND–GND continuity between STM32 GND and ODrive GND.
2. ODrive GPIO1 (its TX) idles at **≈3.3 V** (never 5 V).
3. STM32 PA9 idles at **≈3.3 V** after flashing the default build.
4. Then wire **PA9 → ODrive GPIO2 (RX)**, **PA10 ← ODrive GPIO1 (TX)**. (TX goes to RX.)

## 5. Reading the position
1. Reset the STM32 with the ODrive connected → **LD5 goes off** within ~0.1 s; X ≈ **32768**
   (the power-up position is the centre).
2. Belt is not fitted: turn the **encoder shaft by hand**. With the defaults (540° lock-to-lock,
   `ODRIVE_TURNS_PER_WHEEL_REV = 1`), **0.75 encoder turn = full lock** (X reaches 0 or 65535
   and clamps there). Movement must be smooth, no jumps.
   - Note: `pos_estimate` depends on the placeholder `cpr`; if 0.75 turn does not reach the end,
     the `cpr` is not 2400 — that is expected until calibration.
3. Direction: turning clockwise should increase X. If not, set `ODRIVE_INVERT_DIRECTION 1` in
   `wheel_config.h` (TODO(TBD) there).
4. Stale handling: unplug the PA10 wire → LD5 on within ~0.1 s and X **holds** its last value.
   Re-plug → LD5 off, X follows again.
5. Optional, with the debugger (`arm-none-eabi-gdb` + OpenOCD):
   `p 'odrive_link.c'::s_odr.stats` → `replies_ok` grows by ~100 per second, `parse_errors` and
   `overflows` ≈ 0, `timeouts` small. `p 'odrive_uart.c'::s_stats` → `tx_refused == 0`,
   `hw_errors` ≈ 0. `p 'odrive_link.c'::s_odr.pos_turns`.
6. While this runs, in `odrivetool`: `odrv0.axis0.current_state` stays `1`, errors stay `0`
   (proves the STM32 changes nothing).
7. Troubleshooting: LD5 stays on although wired → check TX/RX not swapped, GND, baud and UART
   enable on the ODrive. If `parse_errors` climbs, the ODrive answers something non-numeric
   (e.g. `nan` while the encoder is not ready, or `invalid ...`) — the firmware rejects it on
   purpose; check with the USB-UART adapter what `f 0` returns.

## 6. Set centre
1. Turn the encoder away from centre, hold **B1 ≥ 1 s** → **LD9 (blue) toggles**, X jumps to
   32768. A short press does nothing. Holding longer does not repeat it.
2. `python stm_32_wheel.py` → the wheel display follows the encoder (`MAX_ANGLE = 270` matches
   the 540° default).

## 7. MCP23017 buttons (when the board exists)
1. Wiring: VDD = **3.3 V** (the bus is shared with the 3.3 V LSM303), VSS = GND, SCL = PB6,
   SDA = PB7, **A0/A1/A2 = GND** (address 0x20), **RESET tied to 3.3 V** (floating RESET = random
   resets). Buttons/paddles switch the input to GND (internal pull-ups are enabled by firmware).
2. Power up → **LD10 stops blinking** within ~0.25 s.
3. `evtest`: GPA0…GPA7 → buttons 1…8, **GPB0 → button 9 (left paddle)**, **GPB1 → button 10
   (right paddle)**. No ghost presses, no chatter.
4. Hot-unplug the expander → all buttons read released, LD10 blinks; re-plug → recovers
   within ~0.25 s, X keeps working throughout (loop never blocks).

## 8. Later (needs a human decision; NOT done by this firmware)
1. Fit the belt, count the teeth → set `ODRIVE_TURNS_PER_WHEEL_REV` (= encoder turns per wheel
   turn) and confirm `WHEEL_LOCK_TO_LOCK_DEG`.
2. Set the real `cpr`, run encoder offset calibration from `odrivetool` (by a human, motor
   free to move, supply in reach).
3. Set a gentle ODrive profile (`current_lim`, `vel_limit`, torque limits), enable and test the
   ODrive watchdog, add an e-stop.
4. Only then, with explicit approval, review `centering_law.c` for wiring to the ODrive (it is
   currently not compiled into the firmware and no torque path exists).
