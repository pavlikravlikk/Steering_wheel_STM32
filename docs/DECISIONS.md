# Decisions log (unattended session, 2026-11-15)

One line per decision: **what** — why — how to change it.

## Starting state (recorded before the first change)
- `git status`: branch `wip-local`, "nothing to commit, working tree clean". HEAD = `50cc0c3 WIP: local work before enabling USART1`.
- `git stash list`: `stash@{0}: On main: before cubemx` — left untouched (never applied, dropped or popped).
- Branches: `main`, `wip-local` (current), `origin/main`. All my commits are local, on `wip-local`, prefix `wip:`; nothing pushed.

## Process
- Teammates' zip extracted to a scratch dir outside the repo (`/tmp/claude-1000/.../scratchpad/teammates`); nothing executed, no file copied. — rule — n/a.
- ARM build directory is outside the repo (scratch). `build/` is git-ignored anyway. — keep repo clean — use `cmake --preset Debug` as usual.

## Firmware
- Fixed `TIM6->EGF` -> `TIM6->EGR` in `hid_report.c`: the committed tree did not compile. — obvious typo — n/a.
- USART1 is NOT enabled in the generated code (no `huart1`, `HAL_UART_MODULE_ENABLED` off, nothing in the `.ioc`). It is brought up register-level in new files `odrive_uart.c/h` (PA9/PA10 AF7, 115200 8N1, RX+TX interrupts, ring buffers), so no generated file, no `stm32f3xx_hal_conf.h` and no `.ioc` change is needed. — rules forbid editing `.ioc`/regenerating — when the `.ioc` gets USART1 (see `docs/IOC_TODO.md`), either keep these files (then leave the USART1 IRQ in CubeMX *disabled* so there is no duplicate `USART1_IRQHandler`) or port `odrive_uart.c` onto `huart1`.
- USART1 kernel clock forced to PCLK2 (48 MHz), BRR computed at runtime from `HAL_RCC_GetPCLK2Freq()`. — matches `.ioc` value `USART1Freq_Value=48000000` — change in `odrive_uart_init()`.
- ODrive driver is read-only by construction: the only TX path is `odrive_uart_send_readonly()`, which rejects any string that is not `f <axis>\n` or `r <path>\n` (`odrive_cmd_is_readonly()`, unit-tested against `w`, `c`, `u`, `sc`, `ss`, `sb`, `se`, `sr`, `p`, `v`, `t`, `q`, ...). — hard rule — do not relax without an explicit decision by the owner.
- Position request is `f 0\n` (reply `"<pos> <vel>\r\n"`), polled at 100 Hz (`ODRIVE_POLL_PERIOD_MS = 10`). — one reply carries both position and velocity, easy to parse strictly — change `ODRIVE_POLL_PERIOD_MS` (5 = 200 Hz) in `wheel_config.h`.
- Own strict float parser (no `strtof`): rejects `nan`, `inf`, trailing junk, empty fields. — `strtof` accepts too much and pulls in a large newlib chunk — `odrive_ascii.c`.
- Reply timeout 8 ms (`ODRIVE_REPLY_TIMEOUT_MS`), stale after 100 ms without a valid line (`ODRIVE_STALE_MS`). When stale, X holds the last value (LED LD5 orange indicates "ODrive link not OK"). Before the first valid sample X = 32768 (centre). — safe & simple — `wheel_config.h`.
- Lines equal to our own command (`f 0`) are treated as echo and ignored without counting an error; any other non-numeric line (e.g. ODrive's `invalid command format`) counts as a parse error. — ODrive does not normally echo, but a USB-serial adaptor or misconfigured board might — `odr_parse_feedback_line()`.
- New steering source `STEERING_SOURCE_ODRIVE` is the default. `POT` (PA1) and the old `ENCODER` (STM32 TIM3 on PC6/PC7) stay selectable at compile time (`-DSTEERING_SOURCE=POT|ENCODER|ODRIVE`). — encoder is now wired to the ODrive, not the STM32 — CMake cache variable.
- `ODRIVE_TURNS_PER_WHEEL_REV = 1.0f` TODO(TBD): ODrive "turns" are encoder turns as seen through the placeholder `cpr`; the belt ratio is unknown. — no belt yet — set it after the belt is fitted (= encoder turns per one wheel turn, as reported by `pos_estimate`).
- `WHEEL_LOCK_TO_LOCK_DEG = 540` TODO(TBD), `ODRIVE_INVERT_DIRECTION = 0` TODO(TBD). — task default — `wheel_config.h`.
- Centre = first valid ODrive position after power-up. "Set centre" is bound to the Discovery **blue user button B1 (PA0)** held for 1 s (`SET_CENTRE_HOLD_MS`), debounced 5 ms. — B1 exists today, does not consume one of the 10 game buttons and cannot be hit by accident while driving — change in `hid_report.c` (`centre_button_task`). Refused (no-op) while ODrive data is missing/stale.
- HID Y (throttle) and Z (brake) report `HID_AXIS_Y_NEUTRAL` / `HID_AXIS_Z_NEUTRAL`, both default 0 (pedal released). — matches the existing behaviour and the Python visualiser — `wheel_config.h`.
- HID report is now packed byte-by-byte by `hid_pack_report()` (explicit little-endian) instead of sending the packed struct; buttons masked to `BUTTON_COUNT` bits. — host-testable, endianness explicit — `hid_pack.c`.
- MCP23017 logic split into pure `mcp23017_core.c` (state machine, mockable async I2C interface, simulation injection) + `debounce.c`; `buttons_mcp23017.c` keeps its public API and only contains HAL/IRQ glue. — testability without the board — n/a.
- Debounce is now time-based (a change must persist `BUTTON_DEBOUNCE_MS` = 5 ms of wall time) instead of counting samples. — the old version counted completed I2C reads, so a slow bus would lengthen it — `debounce.c`.
- MCP23017 inversion done in software by default (`MCP23017_USE_IPOL = 0`); `1` makes the chip invert via IPOLA/IPOLB instead. — fewer writes, same result — `wheel_config.h`.
- On any I2C error/timeout the buttons read as released, and the driver retries after 250 ms. Never blocks. — fail released, not stuck pressed — `MCP23017_RETRY_MS`.
- In `SIMULATE_INPUTS=1` builds the walking-button pattern is injected through `mcp_core_sim_set()` so it also exercises the debounce path. — covers untested hardware — n/a.
- Main loop scheduler lives in `main.c` USER CODE blocks and uses `HAL_GetTick()`; `hid_report_poll()` keeps its own TIM6 1 kHz tick for HID send + button scan; ODrive RX is drained every loop iteration and a request goes out every 10 ms. The 100 ms `HAL_Delay` for the PA12 re-enumeration trick stays (it runs once before the loop). — non-blocking loop — `main.c`.
- `centering_law.c` (gentle spring) is compiled ONLY into the host tests, not into the firmware, and has no caller. Defaults: `MAX_AMPS` 1.0 A (hard cap, a config above it is clamped), Kp 0.5 A/turn, Kd 0.05 A/(turn/s), deadband 0.005 turn, slew 5 A/s. — task rule — **wiring it to the ODrive requires: the belt installed, encoder calibrated (`cpr`, offset), tightened ODrive limits (`current_lim`, `vel_limit`, `torque_lim`), the ODrive watchdog enabled and fed, a hardware e-stop, and the owner's explicit approval.** Until then no code path may send torque/state.
- The teammates' 1078-byte PID force-feedback descriptor was NOT ported. — out of scope, large and unverified, and its main.c arms the motor automatically — revisit when FFB is approved.
- Tests live in repo-root `tests/` (plain `gcc`, `make -C tests`). — single command — `tests/README.md`.
