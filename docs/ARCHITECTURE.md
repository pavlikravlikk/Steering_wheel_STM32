# Architecture

## Module diagram

```
                          config/hardware_profile.toml
                                     |  tools/gen_config.py (also run by CMake)
                                     v
                     Core/Inc/wheel_config_generated.h -> wheel_config.h (all builds)

 main.c loop (never blocks)
   |
   |-- every pass ----> odrive_link_process()   USART1 RX ring -> odrive_ascii (parser)
   |-- 100 Hz --------> odrive_link_request()   "f 0" / queued "r <path>"   [default build]
   |-- FFB cycle -----> ffb_board_poll()                                    [WHEEL_FFB only]
   |                      |- ffb_pid_poll()     USB reports -> effect engine (layer B)
   |                      `- ffb_service_poll() precheck/monitor -> torque pipeline
   |                                            -> safety SM -> odrive_link_request + writes
   `-- TIM6 1 kHz ----> hid_report_poll()
                          |- X: odrive_link_axis16()  (or encoder / pot fallback)
                          |- buttons_update()          MCP23017 core + debounce
                          |- B1: set centre / FFB gesture
                          `- USB IN report (CustomHID 8 bytes | FFB class 9 bytes)

 Pure, host-tested (tests/):  hid_pack, odrive_ascii, odrive_link, steering_map, debounce,
   mcp23017_core, centering_law (host only), ffb/{cmd, safety, precheck, torque, ui,
   service, effects, pid, hid_desc}
 Hardware glue (firmware only): odrive_uart, buttons_mcp23017, hid_report, encoder,
   steering_pot, ffb/ffb_board, ffb/usbd_ffb_hid
```

## Rules

1. **Read-only by default.** Only `Core/Src/ffb` can produce ODrive writes; it is compiled
   only with `-DWHEEL_FFB=ON`. Outside `ffb/`, FFB references must be inside
   `#if WHEEL_FFB` (enforced by `tests/test_safety.c`).
2. **Never block** in the main loop: UART and I2C are interrupt driven, USB answers from
   snapshots, timing uses `HAL_GetTick()` / the TIM6 flag.
3. **Pure logic in pure files** (no HAL include) so it can be unit-tested on the PC; HAL
   access in thin glue files.
4. **One source of truth for hardware values:** the profile. New tunables that depend on
   the hardware go into the profile + generator, not as bare numbers in code.
5. **CubeMX-generated files** are edited only inside `USER CODE` blocks
   (see `docs/IOC_TODO.md`).

## Adding the pedals later (axes Y and Z are reserved)

- The HID report already contains Y (throttle) and Z (brake), 16 bit, 0..65535, currently
  `HID_AXIS_Y_NEUTRAL` / `HID_AXIS_Z_NEUTRAL` (0 = released). Both descriptors (default and
  FFB) already declare them, so **no descriptor change** is needed.
- Throttle (SS49E Hall sensor): an ADC channel. Follow `steering_pot.c`: start a conversion
  on one 1 ms tick, collect it on the next (never wait), filter, map to 0..65535. ADC1_IN2
  (PA1) is used by the potentiometer fallback; pick a free ADC pin (check `main.h` and the
  `.ioc`) and add it in CubeMX (`docs/IOC_TODO.md`).
- Brake (load cell + HX711): the HX711 is a bit-banged serial device (DOUT/SCK). Read it as a
  state machine from the 1 kHz tick, a few clock pulses per tick, never with busy waits (the
  HX711 needs < 50 us SCK high time; one full 24-bit read can be split over several ticks).
  Put the conversion logic in a pure `hx711_core.c` (testable like `mcp23017_core.c`).
- Plug-in point: in `hid_report_poll()` replace `s_report.y = HID_AXIS_Y_NEUTRAL;` and
  `s_report.z = ...` with the new modules' outputs; keep the neutral values when a sensor
  is missing (fail released, like the buttons).
- Add their calibration values (min/max raw, deadband) to `config/hardware_profile.toml`
  under a `[pedals]` section and to `tools/gen_config.py`.
- Tests: `tools/hid_probe.py` and `stm_32_wheel.py` already show Y and Z.

## Optional features (Step 3, each behind a config option, default OFF)
See `docs/DECISIONS.md` "Step 3" for the state of persistent settings, IWDG, button mapping,
steering curve and robustness features.
