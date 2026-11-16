#!/usr/bin/env bash
# Run every host check and build every firmware configuration. No hardware.
#
#   bash tools/check_all.sh              # from the repo root
#   CHECK_BUILD_DIR=/tmp/x bash tools/check_all.sh
#
# Prints a pass/fail table; exit code 0 only if everything passed.
# Never flashes, never opens a device.
set -u
cd "$(dirname "$0")/.." || exit 2
REPO="$(pwd)"
BUILD_ROOT="${CHECK_BUILD_DIR:-$REPO/build/check_all}"
FW="$REPO/steeringwheel_firmware"
mkdir -p "$BUILD_ROOT"

declare -a NAMES RESULTS
record() { NAMES+=("$1"); RESULTS+=("$2"); }

run_step() {  # name, command...
  local name="$1"; shift
  local log="$BUILD_ROOT/$(echo "$name" | tr ' /' '__').log"
  if "$@" >"$log" 2>&1; then
    record "$name" PASS
  else
    record "$name" "FAIL (see $log)"
  fi
}

# --- 1. generated files up to date -----------------------------------------
run_step "profile: gen_config --check" python3 tools/gen_config.py --check

# --- 2. host C tests ---------------------------------------------------------
run_step "host C tests (make -C tests)" make -C tests

# --- 3. host Python tests ----------------------------------------------------
run_step "host Python tests" python3 -m unittest discover -s tools/tests -t tools -q

# --- 3b. mutation tests: every disabled safety check must make a test fail ---
run_step "mutation tests (17 safety mutations killed)" bash tools/mutation_test.sh

# --- 4. firmware configurations ---------------------------------------------
# name | cmake options
CONFIGS=(
  "default|"
  "simulate|-DSIMULATE_INPUTS=ON"
  "ffb_noarm|-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=0"
  "ffb_arm_compile_only|-DWHEEL_FFB=ON -DWHEEL_FFB_ARM_ALLOWED=1"
  "extras_on_compile_only|-DWHEEL_EXTRA_DEFINES=IWDG_ENABLE=1;RESET_REASON_LED=1;AXIS_SHAPING_ENABLE=1;AXIS_DEADZONE=0.02f;AXIS_CURVE_K=0.3f;SETTINGS_FLASH_ENABLE=1;ODRIVE_RECENTRE_AFTER_STALE_MS=1000U"
)
# Tokens that only an ODrive WRITE path can contain.
WRITE_TOKENS=("input_torque" "requested_state" "save_configuration" "w axis" "u 0")

build_config() {  # name, options
  local name="$1" opts="$2" dir="$BUILD_ROOT/fw_$1"
  # shellcheck disable=SC2086
  cmake -S "$FW" -B "$dir" -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
        -DCMAKE_BUILD_TYPE=Debug -DSTEERING_SOURCE=ODRIVE -DSIMULATE_INPUTS=OFF \
        -DWHEEL_FFB=OFF -DWHEEL_FFB_ARM_ALLOWED=0 -DWHEEL_EXTRA_DEFINES= $opts >"$dir.configure.log" 2>&1 || return 1
  cmake --build "$dir" --clean-first >"$dir.build.log" 2>&1 || return 1
  if grep -E "warning:" "$dir.build.log" | grep -v -E "Drivers/|Middlewares/" | grep -q .; then
    echo "warnings in $name" >&2; return 1
  fi
  arm-none-eabi-objcopy -O binary "$dir/steeringwheel_firmware.elf" "$dir/fw.bin" || return 1
  arm-none-eabi-size "$dir/steeringwheel_firmware.elf" | tail -1 > "$dir.size"
}

scan_bin() {  # dir, expect(absent|present)
  local bin="$1/fw.bin" expect="$2" found=0 t
  for t in "${WRITE_TOKENS[@]}"; do
    if grep -a -F -q "$t" "$bin"; then found=1; echo "token '$t' found in $bin"; fi
  done
  if [ "$expect" = absent ]; then [ $found -eq 0 ]; else [ $found -eq 1 ]; fi
}

if command -v arm-none-eabi-gcc >/dev/null && command -v ninja >/dev/null; then
  for entry in "${CONFIGS[@]}"; do
    name="${entry%%|*}"; opts="${entry#*|}"
    if build_config "$name" "$opts"; then
      record "build $name (0 warnings)" "PASS  $(awk '{print "text="$1" data="$2" bss="$3}' "$BUILD_ROOT/fw_$name.size")"
      case "$name" in
        default|simulate|extras_on_compile_only) run_step "binary scan $name: no write tokens" scan_bin "$BUILD_ROOT/fw_$name" absent ;;
        *)                run_step "binary scan $name: tokens present (scanner works)" scan_bin "$BUILD_ROOT/fw_$name" present ;;
      esac
    else
      record "build $name (0 warnings)" "FAIL (see $BUILD_ROOT/fw_$name.*.log)"
    fi
  done
else
  record "firmware builds" "SKIPPED (arm-none-eabi-gcc / ninja not found)"
fi

# --- 5. source scan outside ffb/ (also covered by tests/test_safety.c) ------
src_scan() {
  ! grep -R -n -E 'input_torque|requested_state|save_configuration|"w |"u ' \
      "$FW/Core/Src" "$FW/Core/Inc" "$FW/USB_DEVICE" \
      --include='*.c' --include='*.h' --exclude-dir=ffb
}
run_step "source scan: write tokens only in ffb/" src_scan

# --- table ------------------------------------------------------------------
echo
printf '%-58s %s\n' "CHECK" "RESULT"
printf '%-58s %s\n' "-----" "------"
fail=0
for i in "${!NAMES[@]}"; do
  printf '%-58s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
  case "${RESULTS[$i]}" in PASS*) ;; SKIPPED*) ;; *) fail=1 ;; esac
done
echo
if [ $fail -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "SOME CHECKS FAILED"; fi
exit $fail
