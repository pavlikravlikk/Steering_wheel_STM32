/**
 * @file    ffb_pid.h
 * @brief   HID PID report parsing/dispatch (pure, host-tested) and the glue
 *          between the USB interrupt and the effect engine.
 *
 * USB interrupt context only COPIES reports into a small queue
 * (ffb_pid_rx_isr) and answers GET_REPORT from snapshots
 * (ffb_pid_get_report_isr). The main loop drains the queue into the effect
 * engine (ffb_pid_poll), so the engine is never touched by two contexts.
 *
 * Malformed input (unknown Report ID, wrong report type, too short, invalid
 * block index or effect type) is ignored and counted - never acted on.
 */
#ifndef FFB_PID_H
#define FFB_PID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ffb_effects.h"
#include "ffb_hid_desc.h"

/* HID report types (wValue high byte of GET/SET_REPORT) */
#define FFB_REPORT_INPUT    1U
#define FFB_REPORT_OUTPUT   2U
#define FFB_REPORT_FEATURE  3U

#define FFB_PID_QUEUE_LEN   16U

typedef struct
{
  uint8_t type;
  uint8_t len;
  uint8_t data[FFB_LEN_MAX];
} ffb_pid_msg_t;

typedef struct
{
  ffb_pid_msg_t     q[FFB_PID_QUEUE_LEN];
  volatile uint8_t  head;      /* written by the producer (ISR)  */
  volatile uint8_t  tail;      /* written by the consumer (main) */
  uint32_t          dropped;
} ffb_pid_queue_t;

typedef struct
{
  uint32_t accepted, ignored_short, ignored_type, ignored_id;
} ffb_pid_stats_t;

/* ---- pure functions -------------------------------------------------- */

/** Apply one OUTPUT or FEATURE report to the engine. Returns true if used. */
bool ffb_pid_dispatch(ffb_fx_engine_t *e, ffb_pid_stats_t *st, uint8_t type,
                      const uint8_t *buf, size_t len, uint32_t now_ms);

/** GET_REPORT builders; return the length written (0 if @p max too small). */
size_t ffb_pid_build_block_load(const ffb_fx_engine_t *e, uint8_t *out, size_t max);
size_t ffb_pid_build_pool(uint8_t *out, size_t max);
size_t ffb_pid_build_state(const ffb_fx_engine_t *e, bool actuator_power, uint8_t *out, size_t max);

void ffb_pid_queue_init(ffb_pid_queue_t *q);
bool ffb_pid_queue_push(ffb_pid_queue_t *q, uint8_t type, const uint8_t *buf, size_t len);
bool ffb_pid_queue_pop(ffb_pid_queue_t *q, ffb_pid_msg_t *out);

/* ---- firmware glue (one global engine) -------------------------------- */
void ffb_pid_init(void);

/** USB interrupt: an OUTPUT (interrupt OUT / SET_REPORT) or FEATURE report. */
void ffb_pid_rx_isr(uint8_t type, const uint8_t *buf, size_t len);

/** USB interrupt: GET_REPORT. Returns the length (0 = not supported). */
size_t ffb_pid_get_report_isr(uint8_t type, uint8_t id, uint8_t *out, size_t max);

/** Main loop: process queued reports, refresh the snapshots. */
void ffb_pid_poll(uint32_t now_ms, bool actuator_power, const uint8_t *status16);

/** Game force for ffb_service (normalised -1..1, HID X frame). */
float ffb_pid_force(uint32_t now_ms, float pos, float vel, bool *active);

bool ffb_pid_actuators_enabled(void);
const ffb_pid_stats_t *ffb_pid_stats(void);

#endif /* FFB_PID_H */
