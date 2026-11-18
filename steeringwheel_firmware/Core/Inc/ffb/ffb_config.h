/**
 * @file    ffb_config.h
 * @brief   Force-feedback configuration. Used ONLY by Core/Src/ffb (compiled
 *          with -DWHEEL_FFB=ON). Profile values come from
 *          wheel_config_generated.h (config/hardware_profile.toml).
 *
 * See docs/FFB_SAFETY.md before changing anything here.
 */
#ifndef FFB_CONFIG_H
#define FFB_CONFIG_H

#include "wheel_config.h"

/* ------------------------------------------------------------------------- */
/* Hard limits (deliberately NOT in the profile: raising them is a code      */
/* change that must be reviewed together with docs/FFB_SAFETY.md).           */
/* ------------------------------------------------------------------------- */
/* Absolute ceiling of the commanded current, whatever the profile says. */
/* 2026-11-18 (owner decision): 1.0 -> 3.0 -> 6.0 A (about 3 Nm). The wheel is still DISMOUNTED; the conditions in docs/FFB_SAFETY.md
 * section 5 (mechanical stop, hardware e-stop, OVERSPEED test) are NOT met yet: do not mount the wheel at this value. */
#define FFB_HARD_CAP_AMPS               6.0f

#define FFB_MAX_AMPS \
  ((FFB_PROFILE_MAX_AMPS < FFB_HARD_CAP_AMPS) ? FFB_PROFILE_MAX_AMPS : FFB_HARD_CAP_AMPS)

/* Plausible range of axis0.motor.config.torque_constant (Nm/A). Outside it
 * the precheck refuses: the amps -> torque conversion would be meaningless. */
#ifndef FFB_TORQUE_CONSTANT_MIN
#define FFB_TORQUE_CONSTANT_MIN         0.001f
#endif
#ifndef FFB_TORQUE_CONSTANT_MAX
#define FFB_TORQUE_CONSTANT_MAX         2.0f
#endif

/* Largest |input_torque| the command allowlist accepts (torque units). */
#define FFB_TORQUE_CMD_ABS_MAX          (FFB_HARD_CAP_AMPS * FFB_TORQUE_CONSTANT_MAX)

/* ------------------------------------------------------------------------- */
/* Gates                                                                     */
/* ------------------------------------------------------------------------- */
/* 1 = refuse to arm while any safety-relevant profile entry is still TBD
 * (PROFILE_SAFETY_TBD_COUNT from the generated header). */
#ifndef FFB_REQUIRE_MEASURED_PROFILE
#define FFB_REQUIRE_MEASURED_PROFILE    1
#endif

/* 1 = also feed the ODrive watchdog ("u 0") while DISARMED. Default 0:
 * DISARMED sends nothing. Needed only if the ODrive checks its watchdog
 * while IDLE (docs/HARDWARE_BRINGUP.md step F3). TODO(TBD): decide on bench. */
#ifndef FFB_FEED_WATCHDOG_WHEN_DISARMED
#define FFB_FEED_WATCHDOG_WHEN_DISARMED 0
#endif

/* 1 = read axis0.current_state (exists in docs/odrive_dump.txt, int32):
 * the precheck requires 1 (IDLE), ARMING requires 8 (CLOSED_LOOP_CONTROL)
 * before RUNNING, and RUNNING monitors it. */
#ifndef FFB_VERIFY_CLOSED_LOOP
#define FFB_VERIFY_CLOSED_LOOP          1
#endif

/* 1 = disable the built-in centring spring while the game plays effects. */
#ifndef FFB_SPRING_ONLY_WITHOUT_HOST
#define FFB_SPRING_ONLY_WITHOUT_HOST    1
#endif

/* 1 = flip the sign of game forces (HID PID). TODO(TBD): set with fftest. */
#ifndef FFB_INVERT_HOST_FORCE
#define FFB_INVERT_HOST_FORCE           0
#endif

/* ------------------------------------------------------------------------- */
/* Timing                                                                    */
/* ------------------------------------------------------------------------- */
/* The control cycle = the ODrive command cycle (FFB_COMMAND_PERIOD_MS, from
 * the profile, 5 ms = 200 Hz). At 115200 baud one cycle needs 48 bytes
 * (torque write + watchdog feed + position request) = 4.2 ms, so 1 kHz is
 * impossible on this link; tools/gen_config.py refuses a period that would
 * load the UART above 90 %. */
#define FFB_PRECHECK_TIMEOUT_MS         2000U  /* all precheck reads done within */
#define FFB_ARMING_TIMEOUT_MS           1000U  /* closed loop confirmed within   */
#define FFB_ARMING_MIN_MS               100U   /* zero torque at least this long */
#define FFB_FAULT_RESEND_MS             500U   /* re-send torque 0 + IDLE        */
#define FFB_DISARM_RESEND_COUNT         3U     /* IDLE re-sent after a disarm    */
#define FFB_MAX_CYCLE_GAP_MS            25U    /* main loop stalled -> fault     */
#define FFB_MAX_CONSEC_TIMEOUTS         8U     /* ODrive reply timeouts -> fault */
#define FFB_MAX_TX_FAILURES             3U     /* writes not queued -> fault     */
#define FFB_MONITOR_EVERY               3U     /* RUNNING: 1 read per N cycles   */
#define FFB_MONITOR_MAX_AGE_MS          400U   /* monitored value older -> fault */
#define FFB_DISARMED_READ_EVERY         20U    /* DISARMED: 1 read per N cycles  */
/* PRECHECK / ARMING: 1 read per N cycles. Must be >= 2: a queued read REPLACES the "f" position request of that
 * cycle (one request at a time), so reading every cycle starves the position, which then goes stale
 * (FFB_STALE_MS) and the arm ends in POSITION_STALE (found on the bench 2026-11-17). */
#define FFB_PRECHECK_READ_EVERY         2U
#define FFB_PRECHECK_MAX_AGE_MS         1500U  /* precheck values younger than   */
#define FFB_CENTRE_LOCKOUT_MS           2000U  /* no set-centre right after disarm*/

/* 1 = two push buttons PD0 (left) and PD1 (right), each between the pin and GND (internal pull-up, active low),
 * count as the left / right paddle of the arm gesture. Lets the FFB arm without an MCP23017 board. The pins
 * are read only for the gesture, they are not HID buttons. 0 = only the MCP23017 paddles (HID 9 / 10). */
#ifndef FFB_ARM_PINS
#define FFB_ARM_PINS                    0   /* opt-in: -DWHEEL_EXTRA_DEFINES=FFB_ARM_PINS=1 */
#endif

/* 1 = BENCH WITHOUT PADDLE BUTTONS: the board button B1 alone can arm, with a deliberate sequence: two short
 * taps (each <= FFB_B1SEQ_TAP_MAX_MS, gaps <= FFB_B1SEQ_GAP_MAX_MS) and then HOLD B1 for FFB_ARM_HOLD_MS.
 * Weaker than B1 + both paddles (one button, on the controller board): use it only for the first tests with the
 * steering wheel DISMOUNTED, go back to 0 (default) before the wheel is mounted. */
#ifndef FFB_ARM_B1_ONLY
#define FFB_ARM_B1_ONLY                 0
#endif
#define FFB_B1SEQ_TAPS                  2U
#define FFB_B1SEQ_TAP_MAX_MS            600U
#define FFB_B1SEQ_GAP_MAX_MS            1500U

/* Button bits (HID buttons 9 and 10 = left / right paddle). */
#define FFB_PADDLE_LEFT_BIT             8U
#define FFB_PADDLE_RIGHT_BIT            9U

/* Friction: full force above this wheel speed (turns/s), linear below. */
#define FFB_FRICTION_VEL_EPS            0.05f

_Static_assert(FFB_HARD_CAP_AMPS <= 6.0f, "FFB hard cap above 6 A needs a reviewed safety case");
_Static_assert(FFB_COMMAND_PERIOD_MS >= 1U, "command period");
_Static_assert(FFB_STALE_MS >= 3U * FFB_COMMAND_PERIOD_MS, "stale window too short");
_Static_assert(FFB_PRECHECK_READ_EVERY >= 2U, "reads every cycle starve the position request");

#endif /* FFB_CONFIG_H */
