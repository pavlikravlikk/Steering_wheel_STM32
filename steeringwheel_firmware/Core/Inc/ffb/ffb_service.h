/**
 * @file    ffb_service.h
 * @brief   FFB glue: one control cycle every FFB_COMMAND_PERIOD_MS.
 *
 * Per cycle: collect the last property read -> evaluate precheck/monitoring
 * -> torque pipeline -> safety state machine -> queue the next read ->
 * transmit (position request first, then the writes; for a stop the writes
 * go first, ahead of anything queued).
 *
 * All hardware access goes through ffb_io_t, so the complete flow (including
 * "no write before arm" and every fault path) is host-tested with a mock
 * ODrive (tests/test_ffb_service.c). The firmware instance lives in
 * ffb_board.c.
 */
#ifndef FFB_SERVICE_H
#define FFB_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "odrive_ascii.h"
#include "ffb_precheck.h"
#include "ffb_safety.h"
#include "ffb_torque.h"
#include "ffb_ui.h"

#define FFB_STATUS_REPORT_ID   0x40U
#define FFB_STATUS_REPORT_LEN  16U    /* incl. the Report ID (tools/hid_probe.py) */

/* status flags (byte 5) */
#define FFB_FLAG_ARM_ALLOWED_BUILD  0x01U
#define FFB_FLAG_PRECHECK_OK        0x02U
#define FFB_FLAG_PROFILE_MEASURED   0x04U
#define FFB_FLAG_HOST_EFFECTS       0x08U
#define FFB_FLAG_ACTUATORS_ENABLED  0x10U
#define FFB_FLAG_GESTURE_ACTIVE     0x20U

typedef struct
{
  /* one-shot read-only property read (odrive_link_queue_read / _read_poll) */
  bool             (*queue_read)(const char *path);
  odr_read_state_t (*read_poll)(float *value);
  /* allowlisted write (odrive_uart_send_ffb); urgent = drop queued writes */
  bool             (*send_write)(const char *cmd, bool urgent);
  /* send the next read request ("f" or the queued "r") */
  void             (*link_request)(uint32_t now_ms);
  /* wheel angle/velocity in the MOTOR frame; false if no data/centre */
  bool             (*wheel)(uint32_t now_ms, float *angle_turns, float *vel_turns_s,
                            uint32_t *age_ms);
  uint32_t         (*reply_timeouts)(void);   /* monotonic counters           */
  uint32_t         (*replies_ok)(void);
  /* game force, normalised -1..1 in the HID X frame; may be NULL */
  float            (*host_force)(uint32_t now_ms, float pos_norm, float vel_norm, bool *active);
  /* free-running microsecond counter for loop statistics; may be NULL */
  uint32_t         (*micros)(void);
} ffb_io_t;

typedef struct
{
  int      axis;
  bool     arm_allowed;        /* WHEEL_FFB_ARM_ALLOWED                     */
  bool     simulation;         /* SIMULATE_INPUTS: never touch the ODrive   */
  bool     invert;             /* ODRIVE_INVERT_DIRECTION (HID X frame)     */
  bool     invert_host;        /* FFB_INVERT_HOST_FORCE                     */
  bool     verify_closed_loop; /* FFB_VERIFY_CLOSED_LOOP                    */
  bool     spring_only_without_host;
  uint32_t profile_safety_tbd; /* PROFILE_SAFETY_TBD_COUNT                  */
  bool     require_measured;   /* FFB_REQUIRE_MEASURED_PROFILE              */
  uint32_t period_ms;          /* FFB_COMMAND_PERIOD_MS                     */
  uint32_t stale_ms;           /* FFB_STALE_MS                              */
  uint32_t arm_hold_ms;        /* FFB_ARM_HOLD_MS                           */
  bool     arm_b1_only;        /* FFB_ARM_B1_ONLY: tap, tap, hold B1 can arm */
} ffb_service_cfg_t;

typedef struct
{
  ffb_service_cfg_t  cfg;
  const ffb_io_t    *io;
  ffb_sm_t           sm;
  ffb_pc_table_t     pc;
  ffb_limits_t       lim;
  ffb_torque_cfg_t   tcfg;
  ffb_torque_state_t tst;
  ffb_gesture_t      gesture;
  ffb_b1seq_t        b1seq;            /* only used when cfg.arm_b1_only        */

  bool          arm_event;          /* latched by ffb_service_buttons()      */
  bool          disarm_event;
  bool          paddles_held;
  bool          started;
  uint32_t      last_cycle_ms;
  uint32_t      cycle_count;

  bool          read_pending;
  bool          discard_read;
  ffb_pc_item_t pending_item;

  uint32_t      timeouts_seen, replies_seen, consec_timeouts;
  uint32_t      consec_tx_failures;
  bool          torque_invalid;     /* conversion failed last cycle          */

  uint32_t      arming_start_ms;
  ffb_reason_t  bg_reason;          /* background (DISARMED) evaluation      */
  bool          host_active;
  bool          actuators_enabled;  /* HID PID device control                */
  float         last_amps;

  /* statistics */
  uint32_t      writes_sent;
  uint32_t      monitor_read_failures;
  uint32_t      rate_window_ms, rate_window_cycles, rate_hz;
  uint32_t      cycle_us_max;
} ffb_service_t;

void ffb_service_default_cfg(ffb_service_cfg_t *cfg);
void ffb_service_init(ffb_service_t *s, const ffb_service_cfg_t *cfg, const ffb_io_t *io,
                      uint32_t now_ms);

/** Every 1 ms: debounced B1 and the HID button bits (arm/disarm gesture). */
void ffb_service_buttons(ffb_service_t *s, bool b1, uint16_t buttons, uint32_t now_ms);

/** Every main-loop pass; runs a control cycle when one is due. */
void ffb_service_poll(ffb_service_t *s, uint32_t now_ms);

/** Run one control cycle now (used by ffb_service_poll and the tests). */
void ffb_service_cycle(ffb_service_t *s, uint32_t now_ms);

/** Set-centre is allowed only while DISARMED, with no paddle held and not
 *  right after a disarm (a centre jump while running would jerk the motor). */
bool ffb_service_centre_allowed(const ffb_service_t *s, uint32_t now_ms);

/** HID PID "enable/disable actuators" (layer B). Disabled = no game force. */
void ffb_service_set_actuators(ffb_service_t *s, bool enabled);

uint8_t ffb_service_leds(const ffb_service_t *s, uint32_t now_ms);

/** Vendor feature report 0x40 (16 bytes), see tools/hid_probe.py. */
void ffb_service_status_report(const ffb_service_t *s, uint8_t out[FFB_STATUS_REPORT_LEN]);

#endif /* FFB_SERVICE_H */
