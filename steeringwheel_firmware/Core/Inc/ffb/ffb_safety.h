/**
 * @file    ffb_safety.h
 * @brief   FFB safety state machine (pure, host-tested, no I/O).
 *
 *   DISARMED --arm gesture--> PRECHECK --all checks OK--> ARMING --closed loop
 *   confirmed--> RUNNING.  Any anomaly in ARMING/RUNNING --> FAULT (latched
 *   until power cycle). A failed precheck returns to DISARMED with a reason.
 *
 * The state machine only DECIDES; ffb_service.c turns the decisions into
 * ODrive commands. DISARMED and PRECHECK never request a write (except the
 * IDLE commands right after leaving ARMING/RUNNING, and the optional
 * watchdog feed FFB_FEED_WATCHDOG_WHEN_DISARMED).
 *
 * The enum values are mirrored in tools/hid_probe.py (unit-tested).
 */
#ifndef FFB_SAFETY_H
#define FFB_SAFETY_H

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
  FFB_STATE_DISARMED = 0,
  FFB_STATE_PRECHECK = 1,
  FFB_STATE_ARMING = 2,
  FFB_STATE_RUNNING = 3,
  FFB_STATE_FAULT = 4,
  FFB_STATE_COUNT
} ffb_state_t;

/* Why the wheel is not armed (DISARMED). Also the LED blink count. */
typedef enum
{
  FFB_REASON_OK = 0,
  FFB_REASON_ARM_NOT_ALLOWED = 1,       /* built with WHEEL_FFB_ARM_ALLOWED=0 */
  FFB_REASON_PROFILE_TBD = 2,           /* safety profile entries still TBD   */
  FFB_REASON_SIMULATION_BUILD = 3,      /* SIMULATE_INPUTS build              */
  FFB_REASON_POSITION_STALE = 4,        /* no fresh position from the ODrive  */
  FFB_REASON_READ_TIMEOUT = 5,          /* precheck reads not complete        */
  FFB_REASON_WATCHDOG_DISABLED = 6,
  FFB_REASON_WATCHDOG_TIMEOUT_RANGE = 7,
  FFB_REASON_CONTROL_MODE = 8,          /* not TORQUE_CONTROL                 */
  FFB_REASON_CURRENT_LIM_HIGH = 9,
  FFB_REASON_VEL_LIMIT_HIGH = 10,
  FFB_REASON_MOTOR_NOT_CALIBRATED = 11,
  FFB_REASON_ENCODER_NOT_READY = 12,
  FFB_REASON_AXIS_ERROR = 13,
  FFB_REASON_MOTOR_ERROR = 14,
  FFB_REASON_ENCODER_ERROR = 15,
  FFB_REASON_VBUS_RANGE = 16,
  FFB_REASON_TORQUE_CONSTANT_RANGE = 17,
  FFB_REASON_NOT_CHECKED_YET = 18,
  FFB_REASON_READ_ERROR = 19,           /* a property read was refused        */
  FFB_REASON_COUNT
} ffb_reason_t;

/* Why FFB stopped (FAULT, latched). Also the LED blink count. */
typedef enum
{
  FFB_FAULT_NONE = 0,
  FFB_FAULT_POSITION_STALE = 1,
  FFB_FAULT_ODRIVE_ERROR = 2,           /* error register set / left closed loop */
  FFB_FAULT_UART_TIMEOUT = 3,
  FFB_FAULT_VBUS_RANGE = 4,
  FFB_FAULT_WATCHDOG_FEED_FAILED = 5,   /* writes could not be queued           */
  FFB_FAULT_TORQUE_INVALID = 6,
  FFB_FAULT_OVERSPEED = 7,
  FFB_FAULT_ARMING_TIMEOUT = 8,
  FFB_FAULT_LOOP_OVERRUN = 9,
  FFB_FAULT_READ_FAILED = 10,           /* monitoring read failed / too old     */
  FFB_FAULT_INTERNAL = 11,
  FFB_FAULT_COUNT
} ffb_fault_t;

typedef struct
{
  bool     arm_allowed;          /* WHEEL_FFB_ARM_ALLOWED                     */
  bool     feed_when_disarmed;   /* FFB_FEED_WATCHDOG_WHEN_DISARMED           */
  float    overspeed_turns_s;    /* wheel turns/s                             */
  uint32_t precheck_timeout_ms;
  uint32_t arming_timeout_ms;
  uint32_t arming_min_ms;
  uint32_t fault_resend_ms;
  uint32_t disarm_resend_count;
  uint32_t max_consec_timeouts;
  uint32_t max_tx_failures;
} ffb_sm_cfg_t;

/* Inputs for one control cycle. */
typedef struct
{
  uint32_t     now_ms;
  bool         arm_request;          /* arm gesture completed (event)          */
  bool         disarm_request;       /* user disarm (event)                    */
  bool         precheck_complete;    /* all precheck values read since start   */
  ffb_reason_t precheck_reason;      /* result incl. context checks            */
  bool         position_fresh;       /* ODrive position younger than stale_ms  */
  float        wheel_vel_turns_s;
  ffb_fault_t  monitor_fault;        /* from monitored reads (NONE = ok)       */
  bool         closed_loop_confirmed;/* ARMING: ODrive reports closed loop     */
  float        torque_amps;          /* pipeline output (NaN/inf = invalid)    */
  uint32_t     consec_timeouts;      /* consecutive ODrive reply timeouts      */
  uint32_t     consec_tx_failures;   /* consecutive refused/unqueued writes    */
  bool         cycle_overrun;        /* control cycle late (main loop stalled) */
} ffb_sm_in_t;

/* Decisions for this cycle, in transmission order. */
typedef struct
{
  bool  urgent;            /* drop queued writes first (fault / disarm)      */
  bool  send_torque;       /* "input_torque <torque_amps converted>"         */
  float torque_amps;       /* 0 except in RUNNING                            */
  bool  send_closed_loop;  /* "requested_state 8"                            */
  bool  send_idle;         /* "requested_state 1"                            */
  bool  feed_watchdog;     /* "u <axis>"                                     */
  bool  start_precheck;    /* glue: invalidate the table and read it all     */
} ffb_sm_out_t;

typedef struct
{
  ffb_sm_cfg_t cfg;
  ffb_state_t  state;
  ffb_reason_t reason;          /* last precheck result (DISARMED)          */
  ffb_fault_t  fault;           /* latched fault                            */
  uint32_t     state_since_ms;
  uint32_t     last_resend_ms;
  uint32_t     idle_resends_left;
  uint32_t     last_disarm_ms;
  uint32_t     transitions;     /* statistics                               */
} ffb_sm_t;

void ffb_sm_default_cfg(ffb_sm_cfg_t *cfg, bool arm_allowed);
void ffb_sm_init(ffb_sm_t *sm, const ffb_sm_cfg_t *cfg, uint32_t now_ms);

/** One control cycle. Fills *out (always fully initialised). */
void ffb_sm_step(ffb_sm_t *sm, const ffb_sm_in_t *in, ffb_sm_out_t *out);

/** A neutral input (everything "not happening"), for callers and tests. */
void ffb_sm_in_defaults(ffb_sm_in_t *in, uint32_t now_ms);

const char *ffb_state_name(ffb_state_t s);
const char *ffb_reason_name(ffb_reason_t r);
const char *ffb_fault_name(ffb_fault_t f);

#endif /* FFB_SAFETY_H */
