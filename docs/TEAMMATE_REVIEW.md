# Review of the teammates' `stm32_odrive_ffb.zip` (read-only)

Extracted to a scratch directory outside the repo. Nothing was built, run or copied. 225 files: a CubeMX/CMake project `stm32_odrive_ffb/` (incl. a prebuilt `build/Debug/*.elf/.bin` that was **not** used), with the interesting code in `Core/Src/main.c` (683 lines) and `Core/Src/ffb_engine.c/h` (717 + 103 lines).

## What is there
- `main.c`: one big loop that polls the ODrive over USART1 with HAL, computes force feedback, sends torque (`c 0 <Nm>`), reads 17 buttons from PA0/PC0-7/PD0-7 directly, one pedal on PA1, and sends a 25-byte gamepad report + PID state report.
- `ffb_engine.c/h`: a USB HID PID (DirectInput-style) force-feedback engine: 1078-byte report descriptor with Report IDs, effect slots (constant, ramp, periodic, spring, damper, inertia, friction), `FFB_CalculateTorque()`.
- `SystemClock_Config` at 72 MHz with an HSI fallback, `MX_USART1_UART_Init` with optional TX/RX swap.

## Unsafe / wrong (must not be reused as is)
1. **Arms the motor automatically**: the first successful UART reply calls `ODrive_EnableTorqueControl()`, which writes `w axis0.requested_state 8` (closed loop) — no user action, no check that the encoder is calibrated. With our hardware (no belt, no Halls, uncalibrated encoder) this would at best error out and at worst produce an uncontrolled spin/lock-in.
2. **No watchdog**: torque commands are streamed without `axis0.config.enable_watchdog` / `u 0`; if the STM32 hangs after a non-zero `c 0 x`, the ODrive keeps applying it.
3. **Writes ODrive configuration from firmware**: `current_lim 6.0`, `torque_constant 0.5` (a guess, not measured), `enable_torque_mode_vel_limit 0` (**removes the velocity safety limit**), `control_mode`, `input_mode`; also `sc` (clear errors) which silently hides faults.
4. **Short press of B1 re-arms closed loop** even after a long-press disarm; no confirmation, no fault check.
5. **Blocking I/O in the loop**: `HAL_UART_Transmit(..., 15 ms)`, `ODrive_ReadLine()` busy-waits up to 8 ms twice per poll, `HAL_ADC_PollForConversion`, `HAL_Delay(5)` x7 inside the arm sequence. Loop timing and USB reports stall.
6. **RX flushing before each request** (`ODrive_ClearRx`) throws away bytes; a reply that straddles the flush is lost; no ring buffer/interrupts, so 115200-baud bytes are dropped whenever the CPU is busy elsewhere.
7. **Automatic TX/RX pin swap every 1.2 s** while no reply: driving the ODrive's TX line with our TX is a (mild) electrical fault and masks real wiring errors.
8. `strtof` parsing accepts `nan`/`inf`/partial numbers; parsing errors are not counted.
9. Position/torque scaling assumes `pos_estimate` is in *wheel* turns (`900°` lock, `/720`), i.e. assumes a 1:1 encoder; also `MAX_MOTOR_TORQUE_NM 0.45` with a guessed torque constant.
10. Clock: 72 MHz with `RCC_USBCLKSOURCE_PLL_DIV1_5` and FLASH latency 2 — plausible, but differs from our verified 48 MHz `.ioc`; not needed.
11. PA12 is driven low for 50 ms *after* `MX_GPIO_Init` and never released before `MX_USB_DEVICE_Init` — works only because USB init re-muxes the pin.
12. No assumption about an on-board magnetic encoder or `encoder.config.mode` was found in the code — the comment says "External AB Incremental Encoder", which matches our hardware. Nothing touches `encoder.config.*`.

## Genuinely reusable ideas (ported only as ideas, re-written and tested)
- Use `f 0\n` to get position *and* velocity in one reply → adopted (`odrive_ascii.c`), with a strict parser and a non-blocking interrupt-driven UART.
- Fallback to `r axis0.encoder.pos_estimate` → not needed (`f 0` exists on this board); the read-only allowlist still permits `r ...` for future diagnostics.
- Deadband around centre and velocity-proportional damping for a desktop spring → the same structure appears in the pure `centering_law.c`, but with ampere units, a hard 1 A cap, slew limiting and a fail-safe — and **not wired to the ODrive**.
- Status LED idea (LED shows ODrive link state) → adopted (LD5 orange = ODrive link not OK).
- The HID PID FFB descriptor is a possible starting point for real force feedback later; it must be reviewed against the USB PID spec and Linux `hid-pidff` before use. Not ported.

Each adopted idea is noted in `docs/DECISIONS.md`.
