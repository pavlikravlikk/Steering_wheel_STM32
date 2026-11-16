#!/usr/bin/env bash
# Mutation testing of the safety-relevant code: each mutation disables ONE
# check in a scratch copy of the sources; the host tests must then FAIL.
# A mutation that survives (tests still pass) means a check is not covered.
#
#   bash tools/mutation_test.sh            (from the repo root)
#   MUT_DIR=/tmp/x bash tools/mutation_test.sh
#
# Never touches the real sources: everything happens in copies under
# $MUT_DIR (default build/mutation, git-ignored).
set -u
cd "$(dirname "$0")/.." || exit 2
REPO="$(pwd)"
ROOT="${MUT_DIR:-$REPO/build/mutation}"
mkdir -p "$ROOT"
SRC="steeringwheel_firmware/Core/Src"

# file | literal text to replace | replacement | description
MUTATIONS=(
  "$SRC/ffb/ffb_safety.c|(absf(in->wheel_vel_turns_s) > sm->cfg.overspeed_turns_s)|(0)|no overspeed fault"
  "$SRC/ffb/ffb_safety.c|if (!sm->cfg.arm_allowed)|if (0)|arming without WHEEL_FFB_ARM_ALLOWED"
  "$SRC/ffb/ffb_safety.c|  if (!in->position_fresh)|  if (0)|no stale-position fault"
  "$SRC/ffb/ffb_safety.c|in->consec_timeouts >= sm->cfg.max_consec_timeouts|0|no UART timeout fault"
  "$SRC/ffb/ffb_safety.c|stop_now(out);\n}\n\nstatic void to_disarmed|(void)out;\n}\n\nstatic void to_disarmed|fault does not stop the motor"
  "$SRC/ffb/ffb_precheck.c|return FFB_REASON_CURRENT_LIM_HIGH;|return FFB_REASON_OK;|current_lim not checked"
  "$SRC/ffb/ffb_precheck.c|return FFB_REASON_WATCHDOG_DISABLED;|return FFB_REASON_OK;|watchdog not required"
  "$SRC/ffb/ffb_precheck.c|ctx->require_measured \&\& (ctx->profile_safety_tbd > 0U)|0|TBD profile accepted"
  "$SRC/ffb/ffb_cmd.c|(state != STATE_IDLE) \&\& (state != STATE_CLOSED_LOOP)|0|any requested_state allowed"
  "$SRC/ffb/ffb_cmd.c|return ((float)scaled / 10000.0f) <= (FFB_TORQUE_CMD_ABS_MAX + 0.00005f);|return true;|torque magnitude not limited in allowlist"
  "$SRC/ffb/ffb_torque.c|limit = (cfg->max_amps < FFB_HARD_CAP_AMPS) ? cfg->max_amps : FFB_HARD_CAP_AMPS;|limit = cfg->max_amps;|hard current cap removed"
  "$SRC/ffb/ffb_torque.c|target = ffb_slew(st->last_amps, target, cfg->slew_amps_per_s, dt_s);|(void)0;|slew limiter removed"
  "$SRC/ffb/ffb_ui.c|g->released_seen = false;      /* must see|g->released_seen = true;      /* must see|gesture arms without prior release"
  "$SRC/ffb/ffb_effects.c|    sum = clampf(sum, -1.0f, 1.0f);|    (void)0;|total game force not limited"
  "$SRC/ffb/ffb_pid.c|  if (len < need)|  if (0)|truncated reports accepted"
  "$SRC/ffb/ffb_service.c|s->consec_tx_failures = ok ? 0U : (s->consec_tx_failures + 1U);|s->consec_tx_failures = 0U;|refused writes ignored"
  "$SRC/odrive_ascii.c|  if (!odrive_cmd_is_readonly(r->read_cmd))|  if (0)|read queue not allowlisted"
)

pass=0; fail=0; i=0
printf '%-48s %s\n' "MUTATION" "RESULT"
for m in "${MUTATIONS[@]}"; do
  i=$((i + 1))
  IFS='|' read -r file from to desc <<<"$m"
  dir="$ROOT/m$i"
  mkdir -p "$dir"
  cp -a steeringwheel_firmware tests "$dir"/ 2>/dev/null
  if ! python3 - "$dir/$file" "$from" "$to" <<'EOF'
import sys
path, a, b = sys.argv[1], sys.argv[2].replace("\\n", "\n").replace("\\&", "&"), sys.argv[3].replace("\\n", "\n").replace("\\&", "&")
s = open(path).read()
if a not in s:
    sys.exit(1)
open(path, "w").write(s.replace(a, b, 1))
EOF
  then
    printf '%-48s %s\n' "$desc" "ERROR: pattern not found (update tools/mutation_test.sh)"
    fail=$((fail + 1)); continue
  fi
  if make -C "$dir/tests" >"$dir/log" 2>&1; then
    printf '%-48s %s\n' "$desc" "SURVIVED  <-- tests did not notice"
    fail=$((fail + 1))
  else
    printf '%-48s %s\n' "$desc" "killed"
    pass=$((pass + 1))
  fi
done
echo
echo "$pass killed, $fail survived/errors"
[ "$fail" -eq 0 ]
