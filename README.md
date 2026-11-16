# STM32F3DISCOVERY USB Steering Wheel Controller

Native USB HID joystick on an **STM32F3DISCOVERY** (STM32F303VCT6): **3 axes + 16 button
bits**, 1 ms report rate. Optional, separately built **force feedback** (USB HID PID, MKS
ODrive Mini + hoverboard hub motor), **off by default**. The repository contains the
CubeMX/CMake firmware, host tools (all Python standard library) and a visualizer / demo game.

**Start here:**

| What | Where |
|---|---|
| The day the hardware arrives (ordered steps, tools) | `docs/HARDWARE_BRINGUP.md` |
| The ONE file with all hardware values | `config/hardware_profile.toml` -> `python3 tools/gen_config.py` |
| Everything checked at once (tests, 4 builds, scans, mutations) | `bash tools/check_all.sh` |
| What is still unknown / blocked | `make status` -> `docs/STATUS.md` |
| Force feedback: safety case and bring-up | `docs/FFB_SAFETY.md` |
| Module overview, adding pedals later | `docs/ARCHITECTURE.md` |
| Decisions made without the owner | `docs/DECISIONS.md` |

| Item | Source | Status |
| :--- | :--- | :--- |
| X – steering | position read **from the ODrive over USART1** (PA9/PA10, read-only `f 0` at 100 Hz; encoder LPD3806 is wired to the ODrive) – default. Fallbacks: encoder on STM32 TIM3, potentiometer on PA1 | implemented |
| Y – throttle | pedals (future) | declared, `HID_AXIS_Y_NEUTRAL` (0) |
| Z – brake | pedals (future) | declared, `HID_AXIS_Z_NEUTRAL` (0) |
| Buttons 1–10 | MCP23017 I/O expander on I2C1 (0x20) | implemented (bits 0..9; bits 10..15 reserved, `0`) |

> **Status / safety (2026-11-16):** the **default** firmware never commands the motor. The
> ODrive driver can only transmit `f <axis>` / `r <path>` (enforced by an allowlist, tests and a
> scan of the binary). Force feedback exists only in a separate build
> (`-DWHEEL_FFB=ON`), cannot arm unless additionally built with `-DWHEEL_FFB_ARM_ALLOWED=1`,
> and has **never run on the real motor** - read `docs/FFB_SAFETY.md` first.
> Host tests: `make test` (C: `make -C tests`, Python: `tools/tests`).

### Host tools (`tools/`, Python 3.11+ standard library only, never need sudo)

| Tool | Purpose |
|---|---|
| `gen_config.py` | profile -> `Core/Inc/wheel_config_generated.h` + `tools/generated/odrive_setup.py` (dry run by default) + `odrive_calibrate.py` |
| `odrive_status.py` | read-only ODrive "doctor": decoded errors, startup flags, profile differences, FFB pre-arm verdict (`--fixture` for saved dumps) |
| `hid_probe.py` | live view of the HID reports (`--selftest`, `--file`, `--status` for the FFB status report) |
| `measure_counts.py` | hand measurement -> counts per turn, belt ratio, `cpr` |
| `project_status.py` | `make status`: TBD list, blockers, TODO(TBD) markers, test results |
| `check_all.sh`, `mutation_test.sh` | full host verification |
| `99-steeringwheel.rules` | udev rule (hidraw + input) for VID 0483 / PID 5752 |

### Force feedback build (optional)
```bash
cmake --preset Debug -DWHEEL_FFB=ON                             # HID PID, can NOT arm
cmake --preset Debug -DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=1   # can arm - see docs/FFB_SAFETY.md
```
The FFB build reports Report ID 0x01 + the same 8 bytes (9 bytes), plus HID PID reports.
Arm: hold B1 + both paddles 3 s; disarm: press B1. Status LEDs LD6/LD8/LD4.

---

## HID report

No Report ID, 8 bytes, little-endian, packed (`JoystickReport_t`):

| Bytes | Field | Range |
| :--- | :--- | :--- |
| 0–1 | X (steering) | `uint16`, 0..65535 (centre = 32768) |
| 2–3 | Y (throttle) | `uint16`, 0..65535 (always 0 for now) |
| 4–5 | Z (brake) | `uint16`, 0..65535 (always 0 for now) |
| 6–7 | Buttons | bit 0 = button 1 … bit 15 = button 16 |

Logical maximum 65535 is encoded as a 4-byte item (`0x27 FF FF 00 00`); the 2-byte form
`0x26 FF FF` would be read as −1. USB identifiers: VID `0x0483`, PID `0x5752`.
Endpoint polling interval: 1 ms (`CUSTOM_HID_FS_BINTERVAL = 1`).

---

## Hardware setup and wiring

### USB connectors
- **USB ST-LINK (CN1, top edge):** flashing, debugging, power.
- **USB USER (CN5, bottom edge):** the MCU USB peripheral (PA11/PA12). This is the port the
  OS sees as the game controller.

### 1. Steering encoder – LPD3806-600BM (TIM3, hardware quadrature decoder)

| Encoder wire | Board pin | Notes |
| :--- | :--- | :--- |
| Red (V+) | `5V` (supply is 5–24 V) | encoder supply only |
| Black (GND) | `GND` | common ground |
| Green (A) | **PC6** (TIM3_CH1, AF2) | internal pull-up enabled |
| White (B) | **PC7** (TIM3_CH2, AF2) | internal pull-up enabled |

(Wire colours are typical for this encoder – check yours; if the wheel turns the wrong way,
swap A/B or set `ENCODER_INVERT_DIRECTION 1`.)

- The outputs are probably **open-collector**. The MCU enables its internal pull-ups
  (~40 kΩ, weak), but **external 3.3 kΩ–4.7 kΩ pull-ups from PC6 and PC7 to 3V are
  recommended** for clean edges and noise immunity. Pulling up to **3.3 V (not 5 V)**
  keeps the pins within their rating even with a 5 V encoder supply.
- 600 pulses/rev × 4 (x4 decoding) = **2400 counts per encoder revolution**.
- TIM3 is 16-bit. The firmware reads `CNT` every 1 ms and accumulates the signed `int16`
  delta into an `int32`. The position at power-up is the centre (zero at boot).
- Axis mapping: `X = 32768 + counts × 65535 / ENCODER_COUNTS_FULL_RANGE`, clamped to
  0..65535. Defaults: 2400 counts/rev, 540° lock-to-lock → 3600 counts for the full range
  (±1800 around centre).
- All tunables are in [`Core/Inc/wheel_config.h`](steeringwheel_firmware/Core/Inc/wheel_config.h):
  `ENCODER_COUNTS_PER_WHEEL_REV`, `WHEEL_LOCK_TO_LOCK_DEG`, `ENCODER_INVERT_DIRECTION`.
  Belt ratio is assumed 1:1; if you add a belt reduction, change the counts-per-wheel-rev value.
- `encoder_home_to_limit_switch()` is a stub for future homing.

**Pin check (PC6/PC7)** – *please verify, see "What was not verified"*:
PC6 = TIM3_CH1 and PC7 = TIM3_CH2 on AF2 according to my knowledge of the F303 alternate
function table, and neither is used by the board's LEDs, MEMS, USB or SWD. I could **not**
fetch the datasheet / UM1570 in this environment to confirm the table, the pins' 5 V
tolerance, or the board schematic. Because the pull-ups go to 3.3 V, a 5 V open-collector
encoder is safe regardless; **do not connect a push-pull 5 V encoder output directly**
until you have confirmed that PC6/PC7 are "FT" (5 V tolerant) in the datasheet pin table –
otherwise use a divider/level shifter.

### 2. Buttons – MCP23017 on I2C1

| MCP23017 pin | Board pin | Notes |
| :--- | :--- | :--- |
| VDD | `3V` | use **3.3 V** (matches the MCU I/O level) |
| VSS | `GND` | |
| SCL | **PB6** (I2C1_SCL) | shared with the on-board LSM303 |
| SDA | **PB7** (I2C1_SDA) | shared with the on-board LSM303 |
| A0, A1, A2 | `GND` | address **0x20** |
| /RESET | `3V` | must be tied high (floating = random resets) |
| INTA/INTB | not connected | polled every 1 ms |

- Bus: 400 kHz. 4.7 kΩ pull-ups on SDA/SCL to 3.3 V are required. The Discovery board
  probably already has pull-ups for the LSM303 on this bus (not confirmed here); the
  internal 40 kΩ pull-ups are **not** enough for 400 kHz.
- Address conflict check: the on-board LSM303DLHC answers at 0x19 (accelerometer) and
  0x1E (magnetometer) by my recollection of its datasheet (not re-verified here); the
  MCP23017 range is 0x20–0x27, so **0x20 does not conflict**.
- Both ports are inputs with internal pull-ups (GPPU). A button connects the pin to GND:
  pressed = 0, inverted in the report. Both ports are read in **one** I2C transaction
  (sequential read from GPIOA) every 1 ms, interrupt driven. Debounce: 5 ms.
- If the MCP23017 does not answer (or a transfer gets stuck), the firmware never hangs: it
  reports buttons = 0, blinks the **red LED LD10 (PE13)** and retries every 250 ms.

Button map:

| MCP23017 pin | Button | HID bit | Function |
| :--- | :--- | :--- | :--- |
| GPA0 | 1 | 0 | |
| GPA1 | 2 | 1 | |
| GPA2 | 3 | 2 | |
| GPA3 | 4 | 3 | |
| GPA4 | 5 | 4 | |
| GPA5 | 6 | 5 | |
| GPA6 | 7 | 6 | |
| GPA7 | 8 | 7 | |
| GPB0 | 9 | 8 | left paddle |
| GPB1 | 10 | 9 | right paddle |
| GPB2..GPB7 | – | – | unused (ignored) |

### 3. Potentiometer fallback (optional)

Select with `-DSTEERING_SOURCE=POT` (default is `ENCODER`). Wiring: ends to `3V` and `GND`,
wiper to **PA1** (ADC1_IN2). The 12-bit value is scaled to 16 bit. Conversions are started
and collected by the 1 kHz tick (no blocking ADC polling); filtering is an exponential
moving average (α = 1/8 per ms), a ~4-count deadband and clamping at both ends.

### Pins deliberately left untouched
PA0 (user button), PA5–PA7 (gyro SPI), PE0–PE5 (MEMS), PE8–PE15 (LEDs), PA11/PA12 (USB),
PA13/PA14 (SWD). Newly used: **PA9/PA10 (USART1 → ODrive GPIO2/GPIO1)**, PC6/PC7 (TIM3, only
in the `ENCODER` build), PB6/PB7 (I2C1, already configured by CubeMX),
PE13 / LD10 (fault LED), LD3 / LD7 keep their old meaning (heartbeat / USB configured).

---

## Firmware architecture

```
main loop (never blocks)
  ├─ every pass : odrive_link_process()  drain USART1 RX ring -> parse "pos vel" lines
  ├─ 100 Hz     : odrive_link_request()  send "f 0\n" (read-only allowlist)
  └─ TIM6 1 kHz flag ─► hid_report_poll()
                           ├─ X: odrive_link_axis16() / encoder / pot (compile-time)
                           ├─ buttons_update()  (MCP23017 core, I2C1 interrupts, 5 ms debounce)
                           ├─ B1 held 1 s -> set steering centre
                           ├─ hid_pack_report(): 8 bytes, Y/Z = neutral
                           └─ send only if USB class state == CUSTOM_HID_IDLE
```

Pure, host-tested modules (no HAL includes): `hid_pack`, `odrive_ascii`, `odrive_link`,
`steering_map`, `debounce`, `mcp23017_core`, `centering_law` (the latter is **not** linked
into the firmware). Hardware glue: `odrive_uart.c` (register-level USART1),
`buttons_mcp23017.c` (HAL I2C IT), `hid_report.c`, `encoder.c`, `steering_pot.c`.

| File | Purpose |
| :--- | :--- |
| `Core/Inc/wheel_config.h` | all tunables and build-time switches |
| `Core/Src/odrive_uart.c` | USART1 PA9/PA10 115200 8N1, RX/TX interrupts, ring buffer, read-only TX gate |
| `Core/Src/odrive_ascii.c` | ODrive ASCII client: strict parser, timeouts, stale flag, command allowlist |
| `Core/Src/odrive_link.c` | ODrive position → HID X, power-up centre, set-centre, hold-on-stale |
| `Core/Src/steering_map.c` | turns → 0..65535 (lock-to-lock, belt ratio, invert, clamping) |
| `Core/Src/encoder.c` | (fallback) TIM3 x4 encoder on the STM32, axis scaling, homing stub |
| `Core/Src/mcp23017_core.c` | MCP23017 state machine, mapping, simulation injection (pure) |
| `Core/Src/debounce.c` | time-based debounce, long-press detector (pure) |
| `Core/Src/buttons_mcp23017.c` | HAL/IRQ glue for the MCP23017 core + fault LED |
| `Core/Src/steering_pot.c` | non-blocking potentiometer fallback |
| `Core/Src/hid_report.c` | 1 kHz tick (TIM6), report building, USB send, simulation |
| `Core/Src/main.c` | stays short: calls `hid_report_init()` / `hid_report_poll()` |

Kept as before: the PA12 start-up re-enumeration trick, and the clock configuration
(HSE bypass, PLL ×6, 48 MHz for USB). `HAL_Delay(10)` and the blocking ADC polling are gone.

The modules program TIM3/TIM6 through CMSIS registers and own their interrupt handlers
(`TIM6_DAC_IRQHandler`, `I2C1_EV_IRQHandler`, `I2C1_ER_IRQHandler`), so no CubeMX-generated
code had to be edited outside `USER CODE` blocks – except the two USB constants listed below
(`usbd_conf.h`, plus the `#ifndef`-guarded default descriptor size in the middleware header
`usbd_customhid.h`, kept consistent at 42).

### No-hardware mode

```bash
cmake --preset Debug -DSIMULATE_INPUTS=ON   # X = 4 s sine, buttons 1..10 pressed one by one
```

With `SIMULATE_INPUTS` nothing is read from the encoder, pot or MCP23017 (and the fault LED
stays off), so the HID path can be verified with nothing wired. Y and Z stay 0.

---

## Changes needed in the `.ioc` (CubeMX GUI) – to be done manually

> The complete, current list (including the new **USART1** for the ODrive) is in
> `docs/IOC_TODO.md`.

The project builds and runs without touching the `.ioc`, but two values in generated files
were edited by hand and **CubeMX will overwrite them on the next code generation** unless you
update the `.ioc` accordingly. (`hid_report.c` contains `_Static_assert`s that fail the build
if they are reverted, so a regeneration cannot silently break the descriptor.)

1. **USB_DEVICE → Custom HID → report descriptor size = `42`** (was 20) →
   `USBD_CUSTOM_HID_REPORT_DESC_SIZE` in `USB_DEVICE/Target/usbd_conf.h`
   (the descriptor itself is in a `USER CODE` block of `usbd_custom_hid_if.c`).
2. **USB_DEVICE → Custom HID → FS polling interval (binterval) = `1`** (was 5) →
   `CUSTOM_HID_FS_BINTERVAL`. If your CubeMX version does not expose it, re-apply the value in
   `usbd_conf.h` after each generation.
3. **I2C1 → Fast Mode, 400 kHz** and **RCC → I2C1 clock source = SYSCLK**. The code
   reconfigures I2C1 at start-up (`HAL_I2C_DeInit` / clock mux / `Timing = 0x10930F20` /
   `HAL_I2C_Init`), so the generated `MX_I2C1_Init()` (100 kHz on HSI, `0x00201D2B`) is simply
   superseded. If you change it in CubeMX, use the timing value it computes (or keep
   `0x10930F20`, derived by hand for 48 MHz, PRESC=1, SCLDEL=9, SDADEL=3, SCLH=0x0F, SCLL=0x20).
4. **I2C1 NVIC:** leave the I2C1 event/error interrupts *unticked*. If you tick them,
   CubeMX generates `I2C1_EV_IRQHandler` / `I2C1_ER_IRQHandler` in `stm32f3xx_it.c` and the
   build fails with duplicate symbols – then delete the two handlers from
   `buttons_mcp23017.c`.
5. *(Documentation only)* Label **PC6 = ENC_A (TIM3_CH1, pull-up)** and **PC7 = ENC_B
   (TIM3_CH2, pull-up)**. If you additionally enable **TIM3 → Combined Channels → Encoder
   Mode (TI1 and TI2)**, counter period 65535, input filter 8, or **TIM6** (1 kHz) in CubeMX,
   CubeMX will generate its own `MX_TIM3_Init()` / `MX_TIM6_Init()` and TIM6 interrupt handler.
   Do not use both: either keep `encoder.c` / `hid_report.c` as the owners (recommended) or
   remove their init code and handlers.

---

## Signal processing of the potentiometer path (fallback only)

1. 12-bit ADC sample every 1 ms (calibrated at start-up with `HAL_ADCEx_Calibration_Start`).
2. Scaled to 16 bit: `v16 = raw × 65535 / 4095`.
3. Exponential moving average, α = 1/8, in Q8 fixed point (integer, no float).
4. Deadband of 64 (≈ 4 ADC counts): small changes keep the previous output.
5. Clamping: `≤ 8 ADC counts → 0`, `≥ 4087 → 65535`.

---

## Building

### Prerequisites (Ubuntu / Debian / WSL)
```bash
sudo apt update
sudo apt install -y gcc-arm-none-eabi libnewlib-arm-none-eabi ninja-build cmake openocd
```

### Build
```bash
cd steeringwheel_firmware
cmake --preset Debug
cmake --build --preset Debug
```

Options (all default to the real hardware):

| Option | Meaning |
| :--- | :--- |
| `-DSTEERING_SOURCE=ODRIVE` (default) / `ENCODER` / `POT` | steering input (an existing build dir keeps its cached value – pass it explicitly once) |
| `-DSIMULATE_INPUTS=ON` | simulated X sine + walking buttons |
| macros in `wheel_config.h` | counts per rev, lock-to-lock, invert, debounce, … |

> **Note – stale `build/` directory.** The repository currently contains a tracked
> `steeringwheel_firmware/build/Debug/` created on Windows (its `CMakeCache.txt` points to
> `d:/home/.../steeringwheel_firmware`). On Linux `cmake --preset Debug` then fails with
> *"CMakeCache.txt directory is different…"*. It is a build artefact: delete it once
> (`rm -rf steeringwheel_firmware/build` and, if you like, `git rm -r --cached
> steeringwheel_firmware/build`). A root `.gitignore` with `build/` was added so it is not
> committed again. The files were **not** deleted automatically.

---

## Flashing (OpenOCD)

Connect the **USB ST-LINK** port (CN1):

```bash
cd steeringwheel_firmware
openocd -f board/stm32f3discovery.cfg \
        -c "program build/Debug/steeringwheel_firmware.elf verify reset exit"
```

Then connect the **USB USER** port (CN5) to the computer.

(Windows/WSL alternative: `STM32_Programmer_CLI.exe -c port=SWD -w build/Debug/steeringwheel_firmware.elf -v -rst`.)

Board LEDs: **LD3** (red, PE9) blinks 1 Hz = main loop alive; **LD7** (green, PE11) on = USB
configured; **LD10** (red, PE13) blinking fast = MCP23017 not responding; **LD5** (orange,
PE10) on = no/stale ODrive position (X is held); **LD9** (blue, PE12) toggles = new centre
accepted (hold the blue user button **B1** for 1 s).

---

## Verifying on Linux

### 1. Descriptor – `lsusb`
```bash
lsusb -d 0483:5752
sudo lsusb -v -d 0483:5752 | less
```
Check: `bInterfaceClass 3 Human Interface Device`, endpoint `EP 1 IN`, **`bInterval 1`**,
HID descriptor `wDescriptorLength 42`, and under *Report Descriptor* items equivalent to
`Usage Page (Generic Desktop) / Usage (Joystick) / Usage X, Y, Z / Logical Maximum 65535 /
Report Size 16 / Report Count 3 / Input` followed by `Usage Page (Button) / Usage Minimum 1 /
Usage Maximum 16 / Report Size 1 / Report Count 16 / Input`.
(If `Report Descriptor` shows `(unavailable)`, run it with `sudo` and/or unbind the
interface: `echo -n 3-4:1.0 | sudo tee /sys/bus/usb/drivers/usbhid/unbind` – adjust the bus id –
or use `sudo usbhid-dump -d 0483:5752`.)

### 2. Axes and buttons – `evtest` / `jstest`
```bash
sudo apt install -y evtest joystick
sudo evtest                       # pick "STMicroelectronics Steering Wheel Controller"
jstest --event /dev/input/js0     # no root needed (device is world-readable); jstest-gtk also works
```
Expected: `ABS_X` 0..65535 moves with the wheel (≈32768 at power-up position), `ABS_Y`/`ABS_Z`
stay at 0 (these are "pedals"), and ten buttons (`BTN_TRIGGER`, `BTN_THUMB`, … on `evtest`;
buttons 0–9 in `jstest`) toggle when the MCP23017 inputs go low. With `SIMULATE_INPUTS=ON`
you should see X sweep sinusoidally (4 s period) and buttons 1…10 pulse one after another.
Without root, add yourself to the `input` group (`sudo usermod -aG input $USER`, log out/in)
to use `evtest`.

### 3. Visualizer / game – `stm_32_wheel.py`
```bash
python3 -m venv .venv && . .venv/bin/activate
pip install pygame-ce        # `pygame` has no wheel for new Python versions; pygame-ce is a drop-in
pip install hidapi           # optional on Linux (see below)
python stm_32_wheel.py
```
- On **Linux** the script reads `/dev/hidrawN` directly, so `hidapi` is optional. (The
  `hidapi` wheel from PyPI uses libusb, which reports an empty product string and cannot open
  a device that the kernel's HID driver owns – that is why the script does not rely on it.)
- `/dev/hidraw*` is `root`-only by default, so you need a **udev rule**:
  ```bash
  echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5752", MODE="0666"' \
    | sudo tee /etc/udev/rules.d/99-steeringwheel.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  then unplug/re-plug the USB USER cable. (`MODE="0666"` makes it world-accessible; use
  `TAG+="uaccess"` instead for per-logged-in-user access.)
- The window shows the connected device name, raw `X`/`Y`/`Z` (0..65535), the steering bar and
  the **ten button indicators** (1..10). If the header says *"Demo mode"* the board was not
  found: use ←/→ or A/D to steer and keys `1`..`9`, `0` to emulate buttons 1..10.
- `MAX_ANGLE` (display only) is 270° per side; `INVERT` flips the display direction.
- The visualizer accepts the 8-byte report and the FFB build's 9-byte report (ID 0x01); it
  shows X, the reserved Y/Z bars and all 16 buttons (11..16 dimmed: not wired).
- The ready-made udev rule is `tools/99-steeringwheel.rules` (uses `TAG+="uaccess"`).

---

## What could not be verified without the hardware (and in this environment)

Nothing below has been run on the real board:

- **Firmware build:** since 2026-11-15 every configuration (default, `SIMULATE_INPUTS`,
  `WHEEL_FFB=ON` with and without `WHEEL_FFB_ARM_ALLOWED`) is compiled and linked with
  `arm-none-eabi-gcc` 14.2 by `tools/check_all.sh`, with zero warnings in our sources. It has
  not been flashed or run on the board in these sessions.
- **Force feedback** (everything in `Core/Src/ffb`): host tests, a simulated ODrive and a
  descriptor self-check only; never enumerated by Linux, never connected to the motor. See
  `docs/FFB_SAFETY.md` section 9.
- **Datasheet / schematic facts**: TIM3 on PC6/PC7 = AF2, that PC6/PC7 are free on the board
  and 5 V tolerant, the board's I2C pull-ups and the LSM303 addresses are from memory – the
  datasheet and UM1570 could not be fetched. Check them before wiring a 5 V push-pull encoder.
- **I2C timing `0x10930F20`** was derived by hand from RM0316 for I2C1CLK = 48 MHz (≈ 385 kHz
  expected); measure SCL on a scope or compare with CubeMX's timing tool.
- **Encoder behaviour**: direction, 2400 counts/rev, input filter value, noise immunity, lost
  counts at high speed – need the real encoder.
- **MCP23017 behaviour on the real bus**: shared-bus operation with the LSM303, the
  fault-LED/recovery path with a physically disconnected chip, and the 5 ms debounce feel.
- **USB on the host**: enumeration of the new 42-byte descriptor, 1 ms polling and the
  `lsusb -v` / `evtest` / `jstest` results. The descriptor was only checked by byte count and
  by decoding its items (3×16 bit + 16×1 bit = 64 bits = 8 bytes = `sizeof(JoystickReport_t)`).
- **1 kHz timing under load**: TIM6 tick + I2C interrupts + USB IRQs sharing the CPU, the
  worst-case jitter, and whether 1 report/ms is accepted by every host.
- **`SIMULATE_INPUTS` build** and the potentiometer fallback build/behaviour.
- `stm_32_wheel.py` was tested only headless with a fake 8-byte device (parsing and drawing),
  not against the real firmware.
