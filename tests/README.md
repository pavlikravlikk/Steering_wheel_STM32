# Host unit tests

Plain `gcc`, no dependencies, one command (from the repo root):

```bash
make -C tests
```

It builds `tests/build/run_tests` (with `-Wall -Wextra -Werror` and AddressSanitizer +
UBSan) and runs it. The last line must be `N tests, 0 failed`. The exit code is non-zero on
any failure. `make -C tests clean` removes the build directory.

## What is covered
| File | Module under test |
|---|---|
| `test_hid_pack.c` | `hid_pack.c`: byte layout (little-endian), clamping, button masking |
| `test_hid_descriptor.c` | parses the **real** descriptor in `usbd_custom_hid_if.c`: size = `USBD_CUSTOM_HID_REPORT_DESC_SIZE` = 42, 3 axes, 64 input bits, no Report ID, `CUSTOM_HID_FS_BINTERVAL` = 1 |
| `test_odrive_ascii.c` | read-only command allowlist, strict number parser, line classification, partial lines, garbage, binary noise, echo, overflow, timeouts, stale, tick wrap |
| `test_odrive_link.c` | `odrive_link.c` against `mock_odrive_uart.c`: power-up centre, hold-on-stale, set-centre refusal, request rate limiting, **every transmitted string passes the allowlist**; ring buffer |
| `test_steering_map.c` | turns → HID X: centre, ends, clamping, lock-to-lock, belt ratio, inversion, re-centre, NaN/inf, monotonic |
| `test_debounce.c` | 5 ms time-based debounce (press, release, bounce, sparse samples, wrap), hold detector |
| `test_mcp23017.c` | `mcp23017_core.c` with a mock async I2C bus: init sequence, IPOL option, mapping (paddles = bits 8/9), debounce, absent chip, errors, timeouts, simulation injection |
| `test_centering_law.c` | spring/damper sign and value, 1 A hard cap, deadband, slew limit, fail-safe zero, fuzz bound |
| `test_safety.c` | scans firmware sources for ODrive write tokens; checks `centering_law.c` is not in the firmware build |

## Rules for firmware modules used here
They must not include any HAL/CMSIS header (`main.h`, `stm32f3xx_*.h`). Hardware access goes
through thin interfaces (`odrive_uart.h`, `mcp_io_t`) that the tests replace with mocks.

## Adding a test
Create `tests/test_<name>.c` with `TEST(...)` functions and a `run_<name>_tests()` that calls
`RUN(...)` for each, declare and call it in `test_main.c`, and add any new firmware source to
`FW_SRCS` in the `Makefile`.
