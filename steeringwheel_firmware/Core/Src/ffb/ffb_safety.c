/**
 * @file    ffb_safety.c
 * @brief   FFB safety state machine. Pure: no HAL, no I/O, no globals.
 */
#include "ffb_safety.h"

#include <string.h>

#include "ffb_config.h"

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

static float absf(float v)
{
  return (v < 0.0f) ? -v : v;
}

void ffb_sm_default_cfg(ffb_sm_cfg_t *cfg, bool arm_allowed)
{
  cfg->arm_allowed = arm_allowed;
  cfg->feed_when_disarmed = (FFB_FEED_WATCHDOG_WHEN_DISARMED != 0);
  cfg->overspeed_turns_s = FFB_OVERSPEED_TURNS_S;
  cfg->precheck_timeout_ms = FFB_PRECHECK_TIMEOUT_MS;
  cfg->arming_timeout_ms = FFB_ARMING_TIMEOUT_MS;
  cfg->arming_min_ms = FFB_ARMING_MIN_MS;
  cfg->fault_resend_ms = FFB_FAULT_RESEND_MS;
  cfg->disarm_resend_count = FFB_DISARM_RESEND_COUNT;
  cfg->max_consec_timeouts = FFB_MAX_CONSEC_TIMEOUTS;
  cfg->max_tx_failures = FFB_MAX_TX_FAILURES;
}

void ffb_sm_init(ffb_sm_t *sm, const ffb_sm_cfg_t *cfg, uint32_t now_ms)
{
  memset(sm, 0, sizeof(*sm));
  sm->cfg = *cfg;
  sm->state = FFB_STATE_DISARMED;
  sm->reason = cfg->arm_allowed ? FFB_REASON_NOT_CHECKED_YET : FFB_REASON_ARM_NOT_ALLOWED;
  sm->fault = FFB_FAULT_NONE;
  sm->state_since_ms = now_ms;
  sm->last_disarm_ms = now_ms;
}

void ffb_sm_in_defaults(ffb_sm_in_t *in, uint32_t now_ms)
{
  memset(in, 0, sizeof(*in));
  in->now_ms = now_ms;
  in->precheck_reason = FFB_REASON_NOT_CHECKED_YET;
  in->monitor_fault = FFB_FAULT_NONE;
}

static void enter(ffb_sm_t *sm, ffb_state_t s, uint32_t now_ms)
{
  sm->state = s;
  sm->state_since_ms = now_ms;
  sm->transitions++;
}

/* Stop the motor now: zero torque, then IDLE, ahead of anything queued. */
static void stop_now(ffb_sm_out_t *out)
{
  out->urgent = true;
  out->send_torque = true;
  out->torque_amps = 0.0f;
  out->send_idle = true;
  out->send_closed_loop = false;
  out->feed_watchdog = false;
}

static void to_fault(ffb_sm_t *sm, ffb_fault_t f, uint32_t now_ms, ffb_sm_out_t *out)
{
  sm->fault = (f == FFB_FAULT_NONE) ? FFB_FAULT_INTERNAL : f;
  enter(sm, FFB_STATE_FAULT, now_ms);
  sm->last_resend_ms = now_ms;
  stop_now(out);
}

static void to_disarmed_stopping(ffb_sm_t *sm, uint32_t now_ms, ffb_sm_out_t *out)
{
  enter(sm, FFB_STATE_DISARMED, now_ms);
  sm->idle_resends_left = sm->cfg.disarm_resend_count;
  sm->last_resend_ms = now_ms;
  sm->last_disarm_ms = now_ms;
  stop_now(out);
}

/* Faults common to ARMING and RUNNING. Returns NONE if all is well. */
static ffb_fault_t active_faults(const ffb_sm_t *sm, const ffb_sm_in_t *in)
{
  if (in->cycle_overrun)
  {
    return FFB_FAULT_LOOP_OVERRUN;
  }
  if (!in->position_fresh)
  {
    return FFB_FAULT_POSITION_STALE;
  }
  if (in->monitor_fault != FFB_FAULT_NONE)
  {
    return in->monitor_fault;
  }
  if (in->consec_timeouts >= sm->cfg.max_consec_timeouts)
  {
    return FFB_FAULT_UART_TIMEOUT;
  }
  if (in->consec_tx_failures >= sm->cfg.max_tx_failures)
  {
    return FFB_FAULT_WATCHDOG_FEED_FAILED;
  }
  if (!finite_f(in->wheel_vel_turns_s) || (absf(in->wheel_vel_turns_s) > sm->cfg.overspeed_turns_s))
  {
    return FFB_FAULT_OVERSPEED;
  }
  return FFB_FAULT_NONE;
}

void ffb_sm_step(ffb_sm_t *sm, const ffb_sm_in_t *in, ffb_sm_out_t *out)
{
  const uint32_t now = in->now_ms;
  const uint32_t in_state = now - sm->state_since_ms;
  ffb_fault_t f;

  memset(out, 0, sizeof(*out));

  switch (sm->state)
  {
    case FFB_STATE_DISARMED:
      /* A few extra IDLE commands right after a disarm (robust against a
       * lost byte); otherwise nothing is written. */
      if ((sm->idle_resends_left > 0U) && ((now - sm->last_resend_ms) >= 100U))
      {
        sm->idle_resends_left--;
        sm->last_resend_ms = now;
        out->send_torque = true;
        out->torque_amps = 0.0f;
        out->send_idle = true;
      }
      else if (sm->cfg.feed_when_disarmed)
      {
        out->feed_watchdog = true;
      }
      if (in->arm_request)
      {
        if (!sm->cfg.arm_allowed)
        {
          sm->reason = FFB_REASON_ARM_NOT_ALLOWED;
        }
        else
        {
          sm->idle_resends_left = 0U;
          enter(sm, FFB_STATE_PRECHECK, now);
          out->start_precheck = true;
        }
      }
      break;

    case FFB_STATE_PRECHECK:            /* read-only */
      if (in->disarm_request)
      {
        enter(sm, FFB_STATE_DISARMED, now);
        sm->last_disarm_ms = now;
        break;
      }
      if (in->precheck_complete)
      {
        sm->reason = in->precheck_reason;
        if ((in->precheck_reason == FFB_REASON_OK) && sm->cfg.arm_allowed && in->position_fresh)
        {
          enter(sm, FFB_STATE_ARMING, now);
          out->send_torque = true;      /* input_torque 0 FIRST ...          */
          out->torque_amps = 0.0f;
          out->send_closed_loop = true; /* ... then requested_state 8        */
          out->feed_watchdog = true;
        }
        else
        {
          if ((in->precheck_reason == FFB_REASON_OK) && !in->position_fresh)
          {
            sm->reason = FFB_REASON_POSITION_STALE;
          }
          enter(sm, FFB_STATE_DISARMED, now);
        }
        break;
      }
      if (in_state >= sm->cfg.precheck_timeout_ms)
      {
        sm->reason = FFB_REASON_READ_TIMEOUT;
        enter(sm, FFB_STATE_DISARMED, now);
      }
      break;

    case FFB_STATE_ARMING:
      if (in->disarm_request)
      {
        to_disarmed_stopping(sm, now, out);
        break;
      }
      f = active_faults(sm, in);
      if (f != FFB_FAULT_NONE)
      {
        to_fault(sm, f, now, out);
        break;
      }
      out->send_torque = true;          /* keep zero torque while arming     */
      out->torque_amps = 0.0f;
      out->feed_watchdog = true;
      if (in->closed_loop_confirmed && (in_state >= sm->cfg.arming_min_ms))
      {
        enter(sm, FFB_STATE_RUNNING, now);
      }
      else if (in_state >= sm->cfg.arming_timeout_ms)
      {
        to_fault(sm, FFB_FAULT_ARMING_TIMEOUT, now, out);
      }
      break;

    case FFB_STATE_RUNNING:
      if (in->disarm_request)
      {
        to_disarmed_stopping(sm, now, out);
        break;
      }
      f = active_faults(sm, in);
      if ((f == FFB_FAULT_NONE) && !finite_f(in->torque_amps))
      {
        f = FFB_FAULT_TORQUE_INVALID;
      }
      if ((f == FFB_FAULT_NONE) && (absf(in->torque_amps) > FFB_MAX_AMPS * 1.0001f))
      {
        f = FFB_FAULT_TORQUE_INVALID;   /* the pipeline must have clamped it */
      }
      if (f != FFB_FAULT_NONE)
      {
        to_fault(sm, f, now, out);
        break;
      }
      out->send_torque = true;
      out->torque_amps = in->torque_amps;
      out->feed_watchdog = true;
      break;

    case FFB_STATE_FAULT:
    default:
      if (sm->state != FFB_STATE_FAULT)
      {
        to_fault(sm, FFB_FAULT_INTERNAL, now, out);   /* corrupted state */
        break;
      }
      if ((now - sm->last_resend_ms) >= sm->cfg.fault_resend_ms)
      {
        sm->last_resend_ms = now;
        out->send_torque = true;
        out->torque_amps = 0.0f;
        out->send_idle = true;
      }
      break;
  }
}

const char *ffb_state_name(ffb_state_t s)
{
  static const char *const names[FFB_STATE_COUNT] = {
    "DISARMED", "PRECHECK", "ARMING", "RUNNING", "FAULT"
  };
  return ((unsigned)s < (unsigned)FFB_STATE_COUNT) ? names[s] : "?";
}

const char *ffb_reason_name(ffb_reason_t r)
{
  static const char *const names[FFB_REASON_COUNT] = {
    "OK", "ARM_NOT_ALLOWED", "PROFILE_TBD", "SIMULATION_BUILD", "POSITION_STALE",
    "READ_TIMEOUT", "WATCHDOG_DISABLED", "WATCHDOG_TIMEOUT_RANGE", "CONTROL_MODE",
    "CURRENT_LIM_HIGH", "VEL_LIMIT_HIGH", "MOTOR_NOT_CALIBRATED", "ENCODER_NOT_READY",
    "AXIS_ERROR", "MOTOR_ERROR", "ENCODER_ERROR", "VBUS_RANGE", "TORQUE_CONSTANT_RANGE",
    "NOT_CHECKED_YET", "READ_ERROR", "NOT_IDLE"
  };
  return ((unsigned)r < (unsigned)FFB_REASON_COUNT) ? names[r] : "?";
}

const char *ffb_fault_name(ffb_fault_t f)
{
  static const char *const names[FFB_FAULT_COUNT] = {
    "NONE", "POSITION_STALE", "ODRIVE_ERROR", "UART_TIMEOUT", "VBUS_RANGE",
    "WATCHDOG_FEED_FAILED", "TORQUE_INVALID", "OVERSPEED", "ARMING_TIMEOUT",
    "LOOP_OVERRUN", "READ_FAILED", "INTERNAL"
  };
  return ((unsigned)f < (unsigned)FFB_FAULT_COUNT) ? names[f] : "?";
}
