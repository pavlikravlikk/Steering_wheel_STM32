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
- On a USART1 framing/noise/overrun error the ISR pushes a 0x00 marker into the RX ring so the parser discards the damaged line instead of mis-parsing it. — robustness — `odrive_uart.c`.
- MCP23017 first probe now happens immediately after boot (previously after 250 ms). — faster start, same retry period afterwards — `mcp_core_init()`.
- CMake cache default for `STEERING_SOURCE` changed to `ODRIVE`; an existing build directory keeps its cached value, so pass `-DSTEERING_SOURCE=ODRIVE` once. — CMake semantics — `docs/BENCH_CHECKLIST.md` step 0.
- `tests/test_safety.c` scans all firmware sources for ODrive write tokens (`requested_state`, `input_torque`, `.startup_`, `"w `, `"c `, ...) and fails if `centering_law.c` appears in the firmware CMake sources (mutation-tested). — guard against accidental motor-command paths — edit the test only together with an approved design change.
- Tests live in repo-root `tests/` (plain `gcc`, `make -C tests`). — single command — `tests/README.md`.

---

# Session 2 (unattended, 2026-11-16)

## Starting state
- `git status`: branch `wip-local`, "up to date with 'origin/wip-local'", clean. HEAD `1b70668`.
  64 host tests pass; default ARM build 0 warnings. Default binary saved as baseline.
- The teammates' zip was only *listed* this time (not extracted, nothing executed, nothing read
  beyond the file list); the PID/FFB code here is written from the USB PID spec and the
  requirements of the Linux driver.

## Step 1: profile and tools
- **One profile file** `config/hardware_profile.toml`, every entry `{value, status, how_to_measure}`,
  placeholders = last night's values, all `TBD` (a few pure design choices are `assumed`). — task — edit the file.
- **cpr = counts per MOTOR turn** = `4 * PPR * teeth_housing / teeth_pulley` (what the ODrive needs for
  commutation, since it believes the encoder sits on the motor). Therefore ODrive "turns" = motor turns and
  `ODRIVE_TURNS_PER_WHEEL_REV = motor.motor_turns_per_wheel_rev` (1.0 for direct drive). — physics of the
  ODrive commutation — `gen_config.derive()`.
- `wheel_config.h` includes `wheel_config_generated.h`; every generated macro is `#ifndef`-guarded, the old
  `#ifndef` defaults stay as fallbacks. **The default binary is byte-identical** to the end of last night
  (checked with `cmp` against the saved baseline). — hard rule 1 — n/a.
- CMake regenerates the header when the profile changes (Python 3.11+ found), otherwise uses the committed
  file; an invalid profile fails the build. — "change the profile and rebuild is enough" — `CMakeLists.txt`.
- The generated header lists no ODrive property names (the TBD list is in `make status` instead), because
  `tests/test_safety.c` forbids those names in firmware sources outside `ffb/`. — keep the guard strict — n/a.
- Gentle-profile values chosen (all `TBD`): `current_lim` 4 A, `calibration_current` 3 A, `vel_limit`
  2 turns/s, over/under-voltage trip 30/18 V, `dc_max_positive_current` 6 A, `dc_max_negative_current`
  **-0.01 A** (0.5.x default; a lab supply cannot sink regen current, so regen trips and disarms rather than
  pumping the bus), watchdog 0.25 s enabled, `control_mode` = torque (1), `input_mode` = passthrough (1),
  `enable_torque_mode_vel_limit` = true. — conservative — profile.
- `odrive_setup.py` also writes `startup_closed_loop_control = False` and
  `startup_encoder_offset_calibration = False` (already False; writing False is defensive). It never touches
  `requested_state`, never saves without `--save`, refuses unless IDLE and vbus in the FFB window, verifies by
  read-back and **skips properties that do not exist** on the unnumbered build. — task — `gen_config.py`.
- `odrive_calibrate.py`: preflight (IDLE, vbus, no errors, limits/cpr equal to the profile), typed confirmation
  `I AM CLEAR`, MOTOR_CALIBRATION only if needed, then ENCODER_OFFSET_CALIBRATION, Ctrl+C -> IDLE. With
  `--save` it sets only `motor.config.pre_calibrated` (encoder pre-calibration is not meaningful without an
  index pulse in 0.5.x, as far as I know: TODO(TBD) confirm). — task — generator.
- ODrive enums/error tables from the **0.5.6 documentation** (`tools/odrive_enums.py`), plus the legacy 0.5.1
  axis bits labelled as such. Licence: documentation facts, own code. — unnumbered firmware — verify against
  `odrivetool`'s output.
- `tools/odrive_status.py` accepts flat (`a.b = v`) and odrivetool tree dumps; `--live` uses only `getattr`.
  `docs/odrive_dump.txt` did not exist, so the test fixtures in `tools/tests/fixtures/` are **synthetic**,
  built from the facts in the task, and labelled so. — no hardware — replace with the real dump in H2.
- `tools/hid_probe.py` defines the FFB status feature report layout (ID 0x40, 16 bytes) that the firmware
  implements; a unit test parses `Core/Inc/ffb/ffb_safety.h` to keep the enum tables in sync. — single
  definition checked both ways — n/a.
- Python tests: `python3 -m unittest discover -s tools/tests -t tools` (stdlib only). `make test` runs C+Python.
- `stm_32_wheel.py`: shows Y/Z bars and 16 buttons; accepts the 8-byte report and the FFB build's 9-byte
  report (ID 0x01); other report IDs are ignored. Its parser is unit-tested without pygame (AST extraction).

## Step 2: force feedback (all behind `WHEEL_FFB`, default OFF)
- **Layout:** everything that can write lives in `Core/Src/ffb` + `Core/Inc/ffb`, listed in CMake only in
  `WHEEL_FFB_SOURCES`, added only under `if(WHEEL_FFB)`. Outside `ffb/`, every FFB reference is inside
  `#if WHEEL_FFB` (checked by `tests/test_safety.c`). — hard rule 1 — n/a.
- **Default binary:** byte-identical after Step 1. Step 2 changed shared files (`odrive_ascii.c` gained the one-shot
  `r <path>` read; `odrive_uart.c`/`odrive_link.c`/`hid_report.c`/`main.c` gained `#if WHEEL_FFB` blocks), so the
  default binary is no longer byte-identical, but its behaviour is: no read is ever queued in the default build
  (the queue API is `#if WHEEL_FFB` in `odrive_link`), the TX path of the default build is the old code verbatim
  (`#if !WHEEL_FFB`), single-number lines still count as parse errors (new test), and the binary scan finds no
  write token. — necessary to share the parser — n/a.
- **Reads for the precheck** reuse the existing client: one request outstanding at a time; a reply with one
  number = the read, two numbers = position. — unambiguous without changing the protocol — `odrive_ascii.c`.
- **Writes**: three kinds only (`input_torque`, `requested_state 1|8`, `u 0`), allowlisted in `ffb_cmd.c` and
  re-checked in `odrive_uart_send_ffb()`. Torque formatted `[-]d.dddd` by own code (no printf). — hard rule 3.
- **UART queue (FFB build only):** 160-byte line queue; an urgent stop drops queued lines that have not started
  but finishes the line on the wire (the ODrive never sees a truncated command). — `odrive_uart.c`.
- **Control cycle = ODrive command cycle = 5 ms (200 Hz)**, not 1 kHz: 48 bytes/cycle at 115200 baud is 83 %
  of the link. Order per cycle: request first (reply overlaps the writes), then torque, then feed. — measured by
  arithmetic — `ffb.command_period_ms`, `uart.baud`.
- **States:** DISARMED -> PRECHECK -> ARMING -> RUNNING, FAULT latched until power cycle; user disarm (B1) goes
  to DISARMED (not latched) and re-sends IDLE 3x. FAULT re-sends torque 0 + IDLE every 500 ms. — task — `ffb_safety.c`.
- **DISARMED sends nothing** (only background READS for the status display, one per 100 ms) unless
  `FFB_FEED_WATCHDOG_WHEN_DISARMED=1` (default 0, TODO(TBD) after bench step F3). — task — `ffb_config.h`.
- **Arm gesture:** B1 + both paddles 3 s, only after all three were released; B1 alone = disarm. Set-centre (B1 held
  1 s) is allowed only while DISARMED, with no paddle held, and not within 2 s after a disarm (a centre jump while
  running would jerk the motor). — no accidental arming, no accidental re-centre — `ffb_ui.c`, `hid_report.c`.
- **Profile gate:** FFB refuses to arm while any safety-section profile entry is TBD (`FFB_REQUIRE_MEASURED_PROFILE=1`).
  — "do not invent facts" applied to the motor — `-DFFB_REQUIRE_MEASURED_PROFILE=0` only for deliberate tests.
- **Closed-loop confirmation:** ARMING reads `axis0.current_state` (`FFB_VERIFY_CLOSED_LOOP=1`). This path is not in
  the owner's "known to exist" list (TODO(TBD)); if it does not exist the read fails and arming ends in
  ARMING_TIMEOUT - fail safe. — `ffb_config.h`.
- **Lost replies while running:** one failed read does not fault; the value ages (READ_FAILED after 400 ms) and
  consecutive timeouts give UART_TIMEOUT after 8. — robustness without losing coverage — `ffb_service.c`.
- **Hard cap** `FFB_HARD_CAP_AMPS = 1.0` in code (static assert), profile `ffb.max_amps` can only lower it. —
  task — code change + review to raise.
- **End stops:** smoothstep wall over `ffb.endstop_width_deg` beyond +-lock/2, damping only while moving outward.
  Centring spring disabled while game effects play (`FFB_SPRING_ONLY_WITHOUT_HOST=1`). — `ffb_torque.c`.
- **HID PID descriptor (FFB build only):** own design from the PID 1.0 usage table + Linux hid-pidff requirements
  (read with WebFetch from torvalds/linux master `drivers/hid/usbhid/hid-pidff.c`, GPL-2.0-or-later: requirements
  only, no code copied). Report IDs: 0x01 joystick, 0x02 PID state, 0x11-0x1A outputs, 0x21-0x23 features,
  0x40 vendor status. 16 effect slots, allocated lowest-first so Linux's autocentre probe gets index = logical
  minimum and does not assume a built-in spring. Device Managed Pool = 1. — `ffb_hid_desc.c`.
- **Game force direction:** X component = sin(direction), 90 deg = +X; `FFB_INVERT_HOST_FORCE` TODO(TBD) with fftest. —
  convention, unverified — `ffb_config.h`.
- **Duration 0 and 0xFFFF = forever; loop count 0 = once, 255 = forever.** — Linux sends all ones for infinite — `ffb_pid.c`.
- **USB plumbing:** a separate class driver `Core/Src/ffb/usbd_ffb_hid.c` instead of patching the CustomHID middleware
  (which forwards only 2 OUT bytes, has no GET_REPORT, and stores the report-descriptor length in one byte). Same
  endpoints/PMA as `usbd_conf.c`. Installed by swapping the class pointer right after `MX_USB_DEVICE_Init()`;
  **no USBD_Stop/Start** because on the F3 the HAL stop powers the USB cell down and start does not undo it.
  **No generated file and no middleware file was edited.** — rule 6 — `ffb_board_init()`.
- **USB ISR vs main loop:** OUT/SET_REPORT data is copied into a 16-slot queue in the ISR and applied in the main loop;
  GET_REPORT answers from snapshots. Create New Effect marks Block Load "pending" immediately so the host never
  reads a stale success. — no shared mutable engine state — `ffb_pid.c`.
- **Host gone:** USB not configured / suspended, actuators disabled, reset, stop-all, class DeInit -> no game force.
- **Mutation tests** `tools/mutation_test.sh` (17 mutations, all killed) are part of `tools/check_all.sh`.
