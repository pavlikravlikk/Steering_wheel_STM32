# Hardware facts (put this file at `docs/hardware.md` in the repo)

Status as of 2026-11-16. Written from photos, bench output and the team's notes. Items marked **TBD** are NOT verified. Do not assume them.

## Architecture

PC --USB HID (USER port, PA11/PA12)--> STM32F3Discovery (F303VCT6)
STM32 --UART (3 wires)--> MKS ODrive Mini V1.0 --> hoverboard hub motor.
The LPD3806 encoder is wired to the ODrive (not to the STM32), and is driven through a **gear pair**: a 20-tooth external pinion on the encoder shaft meshes with a 20-tooth internal gear on the motor housing (ratio 1:1, teeth counted by the user 2026-11-16; counts per turn still to be verified on the bench).
Host OS for development: Linux. The ST-LINK USB port is used for flashing and board power; the wheel itself talks to the PC over the USER port.

## UART wiring (per teammate's notes; confirm with a multimeter)

| STM32F3Discovery | MKS ODrive Mini (GPIO connector) |
|---|---|
| PA9  (USART1_TX) | GPIO2 (ODrive UART RX) |
| PA10 (USART1_RX) | GPIO1 (ODrive UART TX) |
| GND | GND |

Never connect 3V3/5V between the two boards. Logic is 3.3 V.
USART1 is NOT yet enabled in `steeringwheel_firmware.ioc` (no CubeMX available at the moment). Initialise it in separate source files; a teammate will mirror it in the .ioc later. Do not regenerate or edit the .ioc from code.

## ODrive Mini (verified on the bench)

- Hardware 3.6 variant 56 (ODrive 3.6 clone). Firmware is an unnumbered dev build: `fw_version 0.0.0`, `fw_version_unreleased = 1`. Parameter names match the 0.5.x family. The full tree is in `docs/odrive_dump.txt`; use those names, not memory.
- **The ODrive is powered only from the 24 V supply. USB is data only** (the board disappears when the PSU is switched off).
- Host tool: odrivetool 0.5.4 in a venv (`pip install "odrive<0.6"`, plus `ipython<9` on Python 3.14).
- UART is enabled in the saved config: `config.enable_uart = True`, `config.uart_baudrate = 115200`. There is no `enable_brake_resistor`; the resistor is armed automatically (`brake_resistor_armed = True`, `brake_resistance = 2.0`).
- Watchdog parameters exist: `axis0.config.enable_watchdog` (False), `axis0.config.watchdog_timeout`, `axis0.watchdog_feed()`.
- **Startup flags now saved as False** (verified after a power cycle): `startup_encoder_offset_calibration`, `startup_closed_loop_control`. They used to be True, so the board calibrated and engaged closed loop by itself at every power-up. Firmware or scripts must never turn them back on.
  - Note: the committed `docs/odrive_dump.txt` is OLDER than this change. Its lines 165 and 167 still show both flags True, and its three active errors (axis 0x100, motor 0x10, encoder 0x4) were cleared by the power cycle. The dump is stale for those lines and must be replaced by a fresh dump at the next ODrive power-up (do not edit it by hand).
- Current saved limits are still loose or default (values read from `docs/odrive_dump.txt`): `dc_bus_overvoltage_trip_level 56`, `dc_bus_undervoltage_trip_level 8`, `dc_max_positive_current 20`, `dc_max_negative_current -3.0` (this is the real saved value; do not assume the ODrive factory default), `motor.current_lim 12`, `torque_constant 0.04`, `enable_watchdog False`, `watchdog_timeout 0.0`, `calibration_current 5`, `controller vel_limit 50`, `control_mode POSITION`, `input_mode TRAP_TRAJ`, `pos_gain 30`, `vel_integrator_gain 0.2`. These are stiff. A gentle profile (lower limits, smaller gains) is a later, human-approved step.
- Motor: `pole_pairs 15`, `phase_resistance 0.166 ohm`, `phase_inductance 0.44 mH`, `motor.config.pre_calibrated True`, `direction -1`. Encoder config: `mode 0` (incremental), `cpr 2400` (placeholder), `pre_calibrated False`, stale `offset`.
- ASCII protocol over UART: `r <path>` read, `w <path> <value>` write, `f 0` position/velocity, `u 0` feed watchdog.

## Motor

- Hoverboard hub motor, 15 pole pairs. The housing (outer rotor) rotates; the shaft is fixed in a vise.
- Rotating mass about 4 kg. Housing OD about 180 mm, shaft OD 13 mm.
- **Only 3 phase wires, no Hall sensors** (`hall_state = 7`). Commutation therefore depends entirely on the encoder through the gear pair. Until the gear mesh is verified on the bench and the encoder is calibrated, closed-loop control and torque are NOT possible, and no torque command may be sent.
- No mechanical end stop. Lock-to-lock angle is set in software (TBD).

## Encoder

- LPD3806-600BM-G5-24C (label confirmed: 600BM), quadrature A/B, no Z index, 5-24 V supply. Expected 2400 counts/rev in x4 mode.
- Bench test: the counter works, moves smoothly in both directions with hand rotation; a rough hand count gave about 5000 per "turn", unverified (an exact marked turn is still TBD).
- Driven by a gear pair: external pinion on the encoder shaft (20 teeth) meshing with an internal gear on the motor housing (20 teeth), ratio 1:1 (counted by the user, 2026-11-16, `belt.teeth_*` in the profile). Same rotation direction in principle, but the steering direction (`wheel.invert`) is NOT decided: check it on the bench.
- `cpr` for the ODrive = 2400 * (teeth on housing gear / teeth on encoder pinion) = 2400 * 20/20 = **2400**. It must be an integer, otherwise commutation drifts.
- **Still to verify on the bench:** rotate the motor housing 10 full turns and read `encoder.shadow_count` before and after: expect about 24000 counts (`tools/measure_counts.py`). The rough hand count above (about 5000 per turn) does not match 2400, so do not trust the 20:20 until the 10-turn measurement agrees. Open question: an internal gear with the same tooth count as its pinion has zero centre distance, so re-count both gears if the measurement disagrees.
- Incremental only: wheel centre = position at power-up, or a "set centre" button.

## Power and safety state

- PSU: Mean Well RSP-320-24 (24 V, 13.4 A), mains earth connected.
- Mains terminals are open on the bench (TBD: cover). There is no DC fuse and no E-stop. A hand stays on the mains plug during first runs.
- The motor must not be commanded while the housing is within reach of hands or cables.

## Known-unreliable information

A Gemini chat from a teammate is NOT a trusted source. In particular:
- It assumed the on-board magnetic encoder. The real encoder is the LPD3806.
- It claimed `encoder.config.mode = 0` "disables" the encoder. Mode 0 is the plain incremental mode.
- Its test code used open-loop lock-in spin at 5 A and blocking UART calls (50 ms timeouts, `HAL_Delay`); do not reuse it.

## Original (upstream) repository state, before the local `wip:` work

(Historical. The local branch `wip-local` has since replaced this with a 3-axis, 16-button HID report at 1 ms, an ODrive UART position reader, MCP23017 buttons and the gated FFB module. See `docs/STATUS.md` and `docs/ARCHITECTURE.md` for the current state.)

- HID: one 16-bit X axis (0-4095), no buttons, no report ID. Data is sent every 10 ms (HAL_Delay).
- ADC potentiometer on PA1 (will be replaced by ODrive position).
- I2C1 (PB6/PB7) and SPI1 are enabled but unused.
- Board pins already used: PA0 (user button), PA5-PA7 (gyro SPI), PB6/PB7 (LSM303 I2C), PE0-PE5, PE8-PE15 (LEDs), PA11/PA12 (USB), PA13/PA14 (SWD).

## Links

- ODrive docs: https://docs.odriverobotics.com/v/0.5.6/ (reference only; the real parameter names are in `docs/odrive_dump.txt`)
- OpenFFBoard wiki (ODrive guide): https://github.com/Ultrawipf/OpenFFBoard_wiki
