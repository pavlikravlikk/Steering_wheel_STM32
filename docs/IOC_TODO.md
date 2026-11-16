# What the `.ioc` needs (to be mirrored by a teammate in CubeMX)

The firmware builds and runs **without** touching the `.ioc`: everything below is already done
in code. These entries only keep a future CubeMX regeneration from undoing or conflicting with
it. The `.ioc` was not edited and no code was regenerated in this session.

## 1. USART1 → ODrive (NEW)
| Setting | Value |
|---|---|
| Connectivity → USART1 → Mode | **Asynchronous** |
| Pins | **PA9 = USART1_TX**, **PA10 = USART1_RX** (AF7) — free on the Discovery board |
| Baud rate / word / parity / stop | **115200**, 8 bits, None, 1 |
| Hardware flow control | None |
| Over-sampling | 16 |
| Advanced features | none (no TX/RX swap, no inversion) |
| GPIO settings → PA10 | **Pull-up** (keeps RX idle-high when the ODrive is unplugged) |
| GPIO settings → PA9 | No pull, push-pull, low speed |
| RCC → USART1 clock mux | **PCLK2** (48 MHz; `.ioc` already shows `USART1Freq_Value=48000000`) |
| NVIC → USART1 global interrupt | **leave UNTICKED** (see below) |

Why the NVIC box must stay unticked: `Core/Src/odrive_uart.c` owns `USART1_IRQHandler`
(register-level RX/TX interrupts + ring buffer). If CubeMX also generates one in
`stm32f3xx_it.c`, the link fails with a duplicate symbol.

What CubeMX will additionally generate once USART1 is enabled, and why it is harmless:
`UART_HandleTypeDef huart1`, `MX_USART1_UART_Init()` (called before `USER CODE 2`),
`HAL_UART_MspInit()` in `stm32f3xx_hal_msp.c`, and `HAL_UART_MODULE_ENABLED` in
`stm32f3xx_hal_conf.h`. `odrive_uart_init()` runs later (from `hid_report_init()` in
`USER CODE 2`), resets USART1 and reconfigures it identically, so the result is the same.
Option for later: port `odrive_uart.c` onto `huart1` (`HAL_UART_Receive_IT` 1 byte +
`HAL_UART_Transmit_IT`) and let CubeMX own the IRQ — keep `odrive_uart_send_readonly()` and
its allowlist check as the only TX entry point.

Label suggestion: PA9 `ODRIVE_TX` (→ ODrive GPIO2/RX), PA10 `ODRIVE_RX` (← ODrive GPIO1/TX).

## 2. USB (already required before this session — unchanged)
1. USB_DEVICE → Custom HID → **report descriptor size = 42** (`USBD_CUSTOM_HID_REPORT_DESC_SIZE`).
2. USB_DEVICE → Custom HID → **FS polling interval (bInterval) = 1** (`CUSTOM_HID_FS_BINTERVAL`).
   If your CubeMX version does not expose it, re-apply it in `usbd_conf.h` after generating.
   `hid_report.c` has `_Static_assert`s and `tests/test_hid_descriptor.c` checks both values,
   so a silent revert fails the build/tests.

## 3. I2C1 (unchanged)
3. I2C1 → **Fast mode 400 kHz**, RCC → **I2C1 clock = SYSCLK** (code reconfigures it at run
   time with `Timing = 0x10930F20` anyway).
4. NVIC → I2C1 event / error interrupts: **leave UNTICKED** (`buttons_mcp23017.c` owns
   `I2C1_EV_IRQHandler` / `I2C1_ER_IRQHandler`).

## 4. Timers (documentation only, unchanged)
5. **TIM6** is configured register-level in `hid_report.c` (1 kHz tick, owns
   `TIM6_DAC_IRQHandler`) — do not enable TIM6 or its interrupt in CubeMX.
6. **TIM3 on PC6/PC7** is only used by the fallback build `-DSTEERING_SOURCE=ENCODER`
   (encoder wired to the STM32). Optionally label PC6 `ENC_A`, PC7 `ENC_B`; do not enable TIM3
   in CubeMX.

## 5. Labels (documentation only)
7. PA0 / B1: **"SET_CENTRE"** — hold 1 s to make the current wheel position the centre.
8. LEDs: LD3 heartbeat, LD7 USB configured, **LD5 (PE10, orange) = ODrive position missing/stale**,
   **LD9 (PE12, blue) toggles = centre accepted**, LD10 blinking = MCP23017 not responding.

## 6. Force feedback build (session 2) - nothing to change in CubeMX
- **No generated file and no middleware file was edited for FFB.** The FFB build (`-DWHEEL_FFB=ON`) uses its own
  USB class driver `Core/Src/ffb/usbd_ffb_hid.c` (same endpoints 0x81/0x01, 64 bytes, the PMA addresses already
  in `usbd_conf.c`), installed from `ffb_board_init()` in `main.c` USER CODE 2.
- Files touched only inside USER CODE blocks: `Core/Src/main.c` (Includes, PV, 2, WHILE).
- `usbd_conf.h` keeps `USBD_CUSTOM_HID_REPORT_DESC_SIZE 42` and `USBD_CUSTOMHID_OUTREPORT_BUF_SIZE 2` - correct for
  the default build; the FFB class does not use them.
- LEDs LD4 (PE8), LD6 (PE15), LD8 (PE14) are used by the FFB build for status; they are already GPIO outputs in
  the generated `MX_GPIO_Init()`. Labels suggestion: LD6 `FFB_READY`, LD8 `FFB_CODE`, LD4 `FFB_FAULT`.
- The DWT cycle counter is enabled in `ffb_board_init()` (loop-time statistics); nothing in CubeMX.
