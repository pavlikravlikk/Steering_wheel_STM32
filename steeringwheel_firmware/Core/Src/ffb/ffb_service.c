/**
 * @file    ffb_service.c
 * @brief   FFB control cycle (pure logic + injected I/O, host-tested).
 */
#include "ffb_service.h"

#include <math.h>      /* NAN only (no libm call) */
#include <string.h>

#include "ffb_cmd.h"
#include "ffb_config.h"

#define BG_MAX_AGE_MS 3000U

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

static float clamp1(float v)
{
  return (v > 1.0f) ? 1.0f : ((v < -1.0f) ? -1.0f : v);
}

void ffb_service_default_cfg(ffb_service_cfg_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->axis = ODRIVE_AXIS;
  cfg->arm_allowed = (WHEEL_FFB_ARM_ALLOWED != 0);
  cfg->simulation = (SIMULATE_INPUTS != 0);
  cfg->invert = (ODRIVE_INVERT_DIRECTION != 0);
  cfg->invert_host = (FFB_INVERT_HOST_FORCE != 0);
  cfg->host_gain = FFB_HOST_GAIN;
  cfg->verify_closed_loop = (FFB_VERIFY_CLOSED_LOOP != 0);
  cfg->spring_only_without_host = (FFB_SPRING_ONLY_WITHOUT_HOST != 0);
  cfg->profile_safety_tbd = PROFILE_SAFETY_TBD_COUNT;
  cfg->require_measured = (FFB_REQUIRE_MEASURED_PROFILE != 0);
  cfg->period_ms = FFB_COMMAND_PERIOD_MS;
  cfg->stale_ms = FFB_STALE_MS;
  cfg->arm_hold_ms = FFB_ARM_HOLD_MS;
  cfg->arm_b1_only = (FFB_ARM_B1_ONLY != 0);
  cfg->vbus_derate_start_v = FFB_VBUS_DERATE_START_V;
  cfg->vbus_derate_end_v = FFB_VBUS_DERATE_END_V;
}

void ffb_service_init(ffb_service_t *s, const ffb_service_cfg_t *cfg, const ffb_io_t *io,
                      uint32_t now_ms)
{
  ffb_sm_cfg_t smc;

  memset(s, 0, sizeof(*s));
  s->cfg = *cfg;
  s->io = io;
  ffb_sm_default_cfg(&smc, cfg->arm_allowed && !cfg->simulation);
  ffb_sm_init(&s->sm, &smc, now_ms);
  ffb_pc_init(&s->pc, cfg->axis, cfg->verify_closed_loop);
  ffb_limits_default(&s->lim);
  ffb_torque_default_cfg(&s->tcfg);
  ffb_torque_reset(&s->tst);
  ffb_gesture_init(&s->gesture, cfg->arm_hold_ms);
  ffb_b1seq_init(&s->b1seq, cfg->arm_hold_ms);
  s->bg_reason = FFB_REASON_NOT_CHECKED_YET;
  s->actuators_enabled = true;
  s->derate_scale = 1.0f;
  s->last_cycle_ms = now_ms;
  s->rate_window_ms = now_ms;
}

void ffb_service_buttons(ffb_service_t *s, bool b1, uint16_t buttons, uint32_t now_ms)
{
  const bool pl = (buttons & (1U << FFB_PADDLE_LEFT_BIT)) != 0U;
  const bool pr = (buttons & (1U << FFB_PADDLE_RIGHT_BIT)) != 0U;

  s->paddles_held = pl || pr;
  switch (ffb_gesture_update(&s->gesture, b1, pl, pr, now_ms))
  {
    case FFB_GESTURE_ARM:    s->arm_event = true;    break;
    case FFB_GESTURE_DISARM: s->disarm_event = true; break;
    default: break;
  }
  if (s->cfg.arm_b1_only && ffb_b1seq_update(&s->b1seq, b1, now_ms))
  {
    s->arm_event = true;         /* tap, tap, hold B1 (bench without paddles) */
  }
}

static ffb_pc_context_t context(const ffb_service_t *s, bool position_fresh, float angle_turns)
{
  ffb_pc_context_t c;
  c.arm_allowed_build = s->cfg.arm_allowed;
  c.simulation_build = s->cfg.simulation;
  c.profile_safety_tbd = s->cfg.profile_safety_tbd;
  c.require_measured = s->cfg.require_measured;
  c.position_fresh = position_fresh;
  c.wheel_in_range = position_fresh && (s->tcfg.half_range_turns > 0.0f)
                  && (((angle_turns >= 0.0f) ? angle_turns : -angle_turns) <= s->tcfg.half_range_turns);
  return c;
}

static bool any_failed(const ffb_pc_table_t *t)
{
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    if (t->v[i].failed && ((i != (unsigned)FFB_PC_CURRENT_STATE) || t->use_current_state))
    {
      return true;
    }
  }
  return false;
}

static bool send(ffb_service_t *s, const char *cmd, size_t n, bool urgent)
{
  bool ok = (n > 0U) && (s->io->send_write != NULL) && s->io->send_write(cmd, urgent);
  if (ok)
  {
    s->writes_sent++;
  }
  return ok;
}

/* Transmit the decisions. Returns false if any write could not be queued. */
static bool transmit(ffb_service_t *s, const ffb_sm_out_t *out)
{
  char cmd[FFB_CMD_MAX];
  bool ok = true;
  bool urgent = out->urgent;

  if (out->send_torque)
  {
    float torque = 0.0f;
    const float kt = s->pc.v[FFB_PC_TORQUE_CONSTANT].value;
    if (out->torque_amps != 0.0f)
    {
      if (!s->pc.v[FFB_PC_TORQUE_CONSTANT].valid
          || !ffb_amps_to_torque(out->torque_amps, kt, s->lim.kt_min, s->lim.kt_max, &torque))
      {
        s->torque_invalid = true;   /* -> TORQUE_INVALID fault next cycle */
        torque = 0.0f;
      }
    }
    ok = send(s, cmd, ffb_fmt_torque(cmd, sizeof(cmd), s->cfg.axis, torque), urgent) && ok;
    urgent = false;               /* only the first write flushes the queue */
  }
  if (out->send_closed_loop)
  {
    ok = send(s, cmd, ffb_fmt_state(cmd, sizeof(cmd), s->cfg.axis, 8), urgent) && ok;
    urgent = false;
  }
  if (out->send_idle)
  {
    ok = send(s, cmd, ffb_fmt_state(cmd, sizeof(cmd), s->cfg.axis, 1), urgent) && ok;
    urgent = false;
  }
  if (out->feed_watchdog)
  {
    ok = send(s, cmd, ffb_fmt_feed(cmd, sizeof(cmd), s->cfg.axis), urgent) && ok;
  }
  return ok;
}

static void collect_read(ffb_service_t *s, uint32_t now_ms)
{
  float v = 0.0f;
  odr_read_state_t st;

  if (!s->read_pending)
  {
    return;
  }
  st = s->io->read_poll(&v);
  if ((st == ODR_READ_DONE) || (st == ODR_READ_FAILED))
  {
    s->read_pending = false;
    if (s->discard_read)
    {
      s->discard_read = false;     /* started before the precheck: ignore */
    }
    else if (st == ODR_READ_DONE)
    {
      ffb_pc_store(&s->pc, s->pending_item, v, now_ms);
    }
    else if ((s->sm.state == FFB_STATE_ARMING) || (s->sm.state == FFB_STATE_RUNNING))
    {
      /* While running, one lost/garbled reply must not stop the wheel: the
       * old value simply ages. Persistent failure still faults
       * (READ_FAILED after FFB_MONITOR_MAX_AGE_MS, UART_TIMEOUT after
       * FFB_MAX_CONSEC_TIMEOUTS). */
      s->monitor_read_failures++;
    }
    else
    {
      ffb_pc_store_failed(&s->pc, s->pending_item);   /* precheck: READ_ERROR */
    }
  }
  else if (st == ODR_READ_IDLE)
  {
    s->read_pending = false;       /* lost (should not happen) */
  }
}

static void schedule_read(ffb_service_t *s)
{
  ffb_pc_item_t item;
  bool due;

  if (s->read_pending)
  {
    return;
  }
  switch (s->sm.state)
  {
    case FFB_STATE_PRECHECK:
      due = (s->cycle_count % FFB_PRECHECK_READ_EVERY) == 0U;   /* the other cycles refresh the position */
      item = ffb_pc_next(&s->pc);
      break;
    case FFB_STATE_ARMING:
      due = (s->cycle_count % FFB_PRECHECK_READ_EVERY) == 0U;
      item = ffb_pc_next_monitor(&s->pc);
      break;
    case FFB_STATE_RUNNING:
      due = (s->cycle_count % FFB_MONITOR_EVERY) == 0U;
      item = ffb_pc_next_monitor(&s->pc);
      break;
    case FFB_STATE_DISARMED:
    case FFB_STATE_FAULT:
    default:
      due = (s->cycle_count % FFB_DISARMED_READ_EVERY) == 0U;
      item = due ? ffb_pc_next(&s->pc) : FFB_PC_AXIS_ERROR;
      break;
  }
  if (due && s->io->queue_read(ffb_pc_path(&s->pc, item)))
  {
    s->read_pending = true;
    s->pending_item = item;
  }
}

static bool closed_loop_confirmed(const ffb_service_t *s)
{
  const ffb_pc_item_t item = s->cfg.verify_closed_loop ? FFB_PC_CURRENT_STATE : FFB_PC_AXIS_ERROR;
  const ffb_pc_value_t *v = &s->pc.v[item];
  const float want = s->cfg.verify_closed_loop ? 8.0f : 0.0f;

  /* must have been read after the closed-loop request went out */
  return v->valid && ((int32_t)(v->read_ms - s->arming_start_ms) > 0) && (v->value == want);
}

void ffb_service_cycle(ffb_service_t *s, uint32_t now_ms)
{
  ffb_sm_in_t in;
  ffb_sm_out_t out;
  const ffb_state_t before = s->sm.state;
  const uint32_t t0 = (s->io->micros != NULL) ? s->io->micros() : 0U;
  float angle = 0.0f;
  float vel = 0.0f;
  uint32_t age = 0xFFFFFFFFU;
  bool have;
  bool fresh;
  float amps = 0.0f;
  uint32_t n;

  ffb_sm_in_defaults(&in, now_ms);
  in.cycle_overrun = s->started && ((now_ms - s->last_cycle_ms) > FFB_MAX_CYCLE_GAP_MS);
  s->started = true;
  s->last_cycle_ms = now_ms;
  s->cycle_count++;

  if (s->cfg.simulation)
  {
    /* Synthetic steering: never talk to the ODrive, never arm. */
    s->bg_reason = FFB_REASON_SIMULATION_BUILD;
    s->arm_event = s->disarm_event = false;
    return;
  }

  /* 1. results of the last read, reply statistics */
  collect_read(s, now_ms);
  n = s->io->reply_timeouts();
  if (n != s->timeouts_seen)
  {
    s->consec_timeouts += n - s->timeouts_seen;
    s->timeouts_seen = n;
  }
  n = s->io->replies_ok();
  if (n != s->replies_seen)
  {
    s->consec_timeouts = 0U;
    s->replies_seen = n;
  }

  /* 2. position */
  have = s->io->wheel(now_ms, &angle, &vel, &age);
  fresh = have && (age <= s->cfg.stale_ms) && finite_f(angle) && finite_f(vel);

  /* 3. torque pipeline (only meaningful while RUNNING) */
  if (fresh && (s->sm.state == FFB_STATE_RUNNING))
  {
    ffb_axis_t ax;
    float host = 0.0f;
    bool active = false;
    const float sign = s->cfg.invert ? -1.0f : 1.0f;            /* motor -> HID X */
    const float hsign = sign * (s->cfg.invert_host ? -1.0f : 1.0f);
    ax.angle_turns = angle;
    ax.vel_turns_s = vel;
    if ((s->io->host_force != NULL) && s->actuators_enabled)
    {
      const float half = (s->tcfg.half_range_turns > 0.0f) ? s->tcfg.half_range_turns : 1.0f;
      host = s->io->host_force(now_ms, clamp1(sign * angle / half),
                               clamp1(sign * vel / FFB_OVERSPEED_TURNS_S), &active);
      /* The game scales its force for a strong wheel; host_gain amplifies it here and the clamp keeps max_amps as the
       * cap (NaN stays NaN and is caught later as TORQUE_INVALID). */
      host = finite_f(host) ? clamp1(host * s->cfg.host_gain) * FFB_MAX_AMPS * hsign : host;
    }
    s->host_active = active;
    if (!ffb_torque_step(&s->tcfg, &s->tst, &ax, host,
                         !(s->cfg.spring_only_without_host && active),
                         (float)s->cfg.period_ms / 1000.0f, &amps))
    {
      amps = NAN;                  /* invalid input -> TORQUE_INVALID fault */
    }
    /* Regeneration guard: the supply voltage rises when the wheel is forced against the motor (bench 2026-11-17: 24 -> 28 V in
     * 0.3 s at 6 A). Fade the force out before the VBUS_RANGE fault, so the wheel gets lighter instead of going limp. */
    s->derate_scale = 1.0f;
    if (s->pc.v[FFB_PC_VBUS].valid && finite_f(amps))
    {
      s->derate_scale = ffb_vbus_derate(s->pc.v[FFB_PC_VBUS].value, s->cfg.vbus_derate_start_v, s->cfg.vbus_derate_end_v);
      amps *= s->derate_scale;
    }
  }
  else
  {
    s->derate_scale = 1.0f;
    ffb_torque_reset(&s->tst);     /* RUNNING always starts from 0 A */
    s->host_active = false;
  }

  /* 4. state machine inputs */
  in.arm_request = s->arm_event;
  in.disarm_request = s->disarm_event;
  s->arm_event = s->disarm_event = false;
  in.position_fresh = fresh;
  in.wheel_vel_turns_s = fresh ? vel : 0.0f;
  in.consec_timeouts = s->consec_timeouts;
  in.consec_tx_failures = s->consec_tx_failures;
  {
    const ffb_pc_context_t ctx = context(s, fresh, angle);
    in.precheck_reason = ffb_precheck_evaluate(&s->pc, &s->lim, &ctx, now_ms, FFB_PRECHECK_MAX_AGE_MS);
    in.precheck_complete = (s->sm.state == FFB_STATE_PRECHECK)
                        && (ffb_pc_complete(&s->pc, now_ms, FFB_PRECHECK_MAX_AGE_MS) || any_failed(&s->pc));
    s->bg_reason = ffb_precheck_evaluate(&s->pc, &s->lim, &ctx, now_ms, BG_MAX_AGE_MS);
  }
  if (s->sm.state == FFB_STATE_ARMING)
  {
    in.monitor_fault = ffb_monitor_evaluate(&s->pc, &s->lim, now_ms, FFB_MONITOR_MAX_AGE_MS, false);
    in.closed_loop_confirmed = closed_loop_confirmed(s);
  }
  else if (s->sm.state == FFB_STATE_RUNNING)
  {
    in.monitor_fault = ffb_monitor_evaluate(&s->pc, &s->lim, now_ms, FFB_MONITOR_MAX_AGE_MS,
                                            s->cfg.verify_closed_loop);
  }
  in.torque_amps = (s->torque_invalid || !finite_f(amps)) ? NAN : amps;
  s->torque_invalid = false;

  /* 5. decide */
  ffb_sm_step(&s->sm, &in, &out);
  s->last_amps = out.send_torque ? out.torque_amps : 0.0f;
  if (out.start_precheck)
  {
    ffb_pc_invalidate(&s->pc);
    s->discard_read = s->read_pending;
  }
  if ((s->sm.state == FFB_STATE_ARMING) && (before != FFB_STATE_ARMING))
  {
    s->arming_start_ms = now_ms;
    ffb_torque_reset(&s->tst);
  }

  /* 6. next read, then transmit. A stop goes out before the request. */
  schedule_read(s);
  {
    bool ok;
    if (out.urgent)
    {
      ok = transmit(s, &out);
      s->io->link_request(now_ms);
    }
    else
    {
      s->io->link_request(now_ms);
      ok = transmit(s, &out);
    }
    if ((s->sm.state == FFB_STATE_ARMING) || (s->sm.state == FFB_STATE_RUNNING))
    {
      s->consec_tx_failures = ok ? 0U : (s->consec_tx_failures + 1U);
    }
    else
    {
      s->consec_tx_failures = 0U;
    }
  }

  /* 7. statistics */
  s->rate_window_cycles++;
  if ((now_ms - s->rate_window_ms) >= 1000U)
  {
    s->rate_hz = (s->rate_window_cycles * 1000U) / (now_ms - s->rate_window_ms);
    s->rate_window_cycles = 0U;
    s->rate_window_ms = now_ms;
  }
  if (s->io->micros != NULL)
  {
    /* The board's micros() is CYCCNT / 72 and wraps about every 60 s (not at 2^32): a cycle that straddles the wrap
     * would give a huge difference (bench 2026-11-17: loop_max stuck at 65535). Such a sample is dropped. */
    const uint32_t t1 = s->io->micros();
    if (t1 >= t0)
    {
      const uint32_t dt = t1 - t0;
      if (dt > s->cycle_us_max)
      {
        s->cycle_us_max = dt;
      }
    }
  }
}

void ffb_service_poll(ffb_service_t *s, uint32_t now_ms)
{
  if (!s->started || ((now_ms - s->last_cycle_ms) >= s->cfg.period_ms))
  {
    ffb_service_cycle(s, now_ms);
  }
}

bool ffb_service_centre_allowed(const ffb_service_t *s, uint32_t now_ms)
{
  return (s->sm.state == FFB_STATE_DISARMED) && !s->paddles_held && !ffb_b1seq_pending(&s->b1seq)
      && ((now_ms - s->sm.last_disarm_ms) >= FFB_CENTRE_LOCKOUT_MS);
}

void ffb_service_set_actuators(ffb_service_t *s, bool enabled)
{
  s->actuators_enabled = enabled;
}

uint8_t ffb_service_leds(const ffb_service_t *s, uint32_t now_ms)
{
  const ffb_reason_t r = (s->sm.state == FFB_STATE_DISARMED) ? s->bg_reason : s->sm.reason;
  return ffb_led_pattern(s->sm.state, r, s->sm.fault, s->bg_reason == FFB_REASON_OK,
                         ffb_gesture_active(&s->gesture) || ffb_b1seq_pending(&s->b1seq), now_ms);
}

static void put16(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

void ffb_service_status_report(const ffb_service_t *s, uint8_t out[FFB_STATUS_REPORT_LEN])
{
  uint8_t flags = 0U;
  int32_t ma = (int32_t)(s->last_amps * 1000.0f);
  const ffb_pc_value_t *vb = &s->pc.v[FFB_PC_VBUS];
  uint32_t dv = (vb->valid && (vb->value > 0.0f) && (vb->value < 6000.0f)) ? (uint32_t)(vb->value * 10.0f) : 0U;

  memset(out, 0, FFB_STATUS_REPORT_LEN);
  if (s->cfg.arm_allowed)               flags |= FFB_FLAG_ARM_ALLOWED_BUILD;
  if (s->bg_reason == FFB_REASON_OK)    flags |= FFB_FLAG_PRECHECK_OK;
  if (s->cfg.profile_safety_tbd == 0U)  flags |= FFB_FLAG_PROFILE_MEASURED;
  if (s->host_active)                   flags |= FFB_FLAG_HOST_EFFECTS;
  if (s->actuators_enabled)             flags |= FFB_FLAG_ACTUATORS_ENABLED;
  if (s->derate_scale < 0.999f)         flags |= FFB_FLAG_VBUS_DERATING;
  if (ffb_gesture_active(&s->gesture) || ffb_b1seq_pending(&s->b1seq))  flags |= FFB_FLAG_GESTURE_ACTIVE;
  if (ma > 32767)  ma = 32767;
  if (ma < -32768) ma = -32768;
  out[0] = FFB_STATUS_REPORT_ID;
  out[1] = 1U;                                          /* layout version */
  out[2] = (uint8_t)s->sm.state;
  out[3] = (uint8_t)((s->sm.state == FFB_STATE_DISARMED) ? s->bg_reason : s->sm.reason);
  out[4] = (uint8_t)s->sm.fault;
  out[5] = flags;
  put16(&out[6], (uint32_t)(uint16_t)(int16_t)ma);
  put16(&out[8], s->rate_hz > 0xFFFFU ? 0xFFFFU : s->rate_hz);
  put16(&out[10], s->cycle_us_max > 0xFFFFU ? 0xFFFFU : s->cycle_us_max);
  put16(&out[12], s->timeouts_seen > 0xFFFFU ? 0xFFFFU : s->timeouts_seen);
  put16(&out[14], dv > 0xFFFFU ? 0xFFFFU : dv);
}
