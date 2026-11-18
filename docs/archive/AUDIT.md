# Audit of the existing firmware (state at `50cc0c3`, before this session)

Repo: `steeringwheel_firmware/` (CubeMX + CMake, STM32F303VCT6, SYSCLK 48 MHz from 8 MHz HSE bypass x6, APB1 24 MHz, APB2 48 MHz).

## Build status
- Toolchain present: `arm-none-eabi-gcc 14.2.1`, `cmake`, `ninja`.
- **The committed tree did not compile**: `hid_report.c:57: 'TIM_TypeDef' has no member named 'EGF'` (typo for `EGR`). This was the only error (checked with `ninja -k 0`). Fixed in the first commit of this session.

## Module by module

| File | What it does | Notes |
|---|---|---|
| `Core/Inc/wheel_config.h` | Central compile-time config: steering source (`POT` / `ENCODER`), `SIMULATE_INPUTS`, encoder counts/rev (2400), lock-to-lock 540°, MCP23017 address/debounce/retry, I2C timing, pot deadband. | No HAL dependency, usable on host. |
| `encoder.c/h` | **Reads a quadrature encoder directly on the STM32 via TIM3 in x4 encoder mode** (PC6 = A, PC7 = B, AF2, pull-ups, input filter 8), register-level CMSIS (no HAL TIM). 16-bit CNT delta accumulated into int32; centre = power-up; `encoder_get_axis16()` maps ±half range to 0..65535 with clamping. `encoder_home_to_limit_switch()` is an empty stub. | Correct for an encoder wired to the STM32, but **the encoder is now wired to the ODrive**, so this path is obsolete as the primary source. Kept as a compile-time fallback (`-DSTEERING_SOURCE=ENCODER`). |
| `steering_pot.c/h` | Potentiometer fallback on PA1 (ADC1_IN2): starts one conversion per tick, collects it on the next (non-blocking), EMA filter α=1/8, deadband, end clamps. | Fine. |
| `buttons_mcp23017.c/h` | MCP23017 on I2C1 (PB6/PB7, 0x20) with interrupt-driven HAL I2C (`HAL_I2C_Mem_*_IT`), own `I2C1_EV/ER_IRQHandler`s, state machine IODIR→GPPU→continuous GPIOA/B reads, 10 ms transfer timeout, 250 ms retry, red LED LD10 blinks while absent, active-low inversion, mapping GPA0-7 → bits 0-7, GPB0/1 → bits 8/9. Reconfigures I2C1 to 400 kHz from SYSCLK. | Non-blocking and mostly correct. Debounce counted *samples*, not milliseconds. Logic was tied to HAL, so not host-testable; no simulation hook. Refactored (logic → `mcp23017_core.c`, debounce → `debounce.c`), API kept. |
| `hid_report.c/h` | TIM6 1 kHz tick (register-level, IRQ sets a flag). `hid_report_poll()` once per tick: update steering source + buttons, build `JoystickReport_t`, send it if the CUSTOM_HID endpoint is idle. Heartbeat LED LD3, USB-configured LED LD7. `_Static_assert`s guard descriptor size 42, bInterval 1 and report size 8. `SIMULATE_INPUTS`: sine on X, walking buttons. | Non-blocking. Contains the compile error above. Extended for the ODrive source, set-centre button and pure report packing. |
| `main.c` (modified, USER CODE only) | PA12 driven low 100 ms (`HAL_Delay`) before clock init to force USB re-enumeration; generated MX init of GPIO, ADC1, I2C1, SPI1, USB; then `hid_report_init()`; loop calls `hid_report_poll()`. | The only `HAL_Delay` is the one-shot PA12 trick *before* the loop. **Nothing blocking in the main loop.** No USART1 init. |
| `usbd_custom_hid_if.c` | Report descriptor (USER CODE 0): Generic Desktop / Joystick, X, Y, Z 16-bit 0..65535 (LOGICAL_MAXIMUM as a 4-byte item — correct), 16 buttons, no Report ID, 42 bytes. | Correct; verified byte count = 42. |
| `usbd_custom_hid_if.h` | `JoystickReport_t` packed struct {x, y, z, buttons} = 8 bytes. | Fine. |
| `usbd_conf.h` | `USBD_CUSTOM_HID_REPORT_DESC_SIZE 42U`, `CUSTOM_HID_FS_BINTERVAL 0x1U` (1 ms), EP IN/OUT size 64. | These are generated values; the `.ioc` must carry them too (see `IOC_TODO.md`). |
| `usbd_customhid.h` (middleware) | Default `USBD_CUSTOM_HID_REPORT_DESC_SIZE` changed to 42 (inside `#ifndef`, so `usbd_conf.h` wins anyway). | Harmless. |

## Answers to the specific questions
- **Does `encoder.c` read a quadrature encoder on the STM32 through a timer?** Yes — TIM3 hardware encoder mode on PC6/PC7. It does *not* talk to the ODrive. With the current wiring (encoder → ODrive) the steering angle must come from the ODrive over UART instead.
- **Anything blocking in the main loop?** No. ADC, I2C and USB are all started/checked without waiting. The only `HAL_Delay(100)` is the PA12 re-enumeration pulse at start-up.
- **HID descriptor / report / polling:** Joystick, X/Y/Z uint16 0..65535 + 16 buttons, 8-byte input report without Report ID, 42-byte descriptor, interrupt IN endpoint bInterval = 1 ms (`CUSTOM_HID_FS_BINTERVAL = 1`). VID/PID 0x0483/0x5752 (matches `stm_32_wheel.py`). This already met the Phase 2 step 1 requirements; only the packing was made a tested pure function.
- **USART1:** not enabled anywhere (no handle, HAL UART module disabled in `stm32f3xx_hal_conf.h`, absent from the `.ioc`). PA9/PA10 are not used by any generated code.

## Pin check (against generated `main.h` / `MX_GPIO_Init`)
Used: PA0 (B1), PA1 (pot, only in POT build), PA5-PA7 (SPI1 gyro), PA11/PA12 (USB), PA13/PA14 (SWD), PB6/PB7 (I2C1), PC6/PC7 (TIM3, only in ENCODER build), PC14/PC15 & PF0/PF1 (oscillators), PE2-PE5 (MEMS/CS), PE8-PE15 (LEDs).
New: **PA9 = USART1_TX (AF7), PA10 = USART1_RX (AF7, pull-up)** — free.
