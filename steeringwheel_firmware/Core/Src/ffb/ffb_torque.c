/**
 * @file    ffb_torque.c
 * @brief   Torque pipeline (pure). Based on the centring law of
 *          centering_law.c (deadband removed: the end stops and the spring
 *          are continuous, so no deadband is needed).
 */
#include "ffb_torque.h"

#include "ffb_config.h"

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

static float absf(float v)
{
  return (v < 0.0f) ? -v : v;
}

void ffb_torque_default_cfg(ffb_torque_cfg_t *cfg)
{
  cfg->spring_kp = FFB_SPRING_KP;
  cfg->damper_kd = FFB_DAMPER_KD;
  cfg->friction_amps = FFB_FRICTION_AMPS;
  cfg->friction_vel_eps = FFB_FRICTION_VEL_EPS;
  cfg->inertia_k = FFB_INERTIA_KI;
  cfg->half_range_turns = ((float)WHEEL_LOCK_TO_LOCK_DEG / 2.0f) / 360.0f;
  cfg->endstop_width_turns = FFB_ENDSTOP_WIDTH_DEG / 360.0f;
  cfg->endstop_amps = FFB_ENDSTOP_AMPS;
  cfg->endstop_damping = FFB_ENDSTOP_DAMPING;
  cfg->max_amps = FFB_MAX_AMPS;
  cfg->slew_amps_per_s = FFB_SLEW_AMPS_PER_S;
}

void ffb_torque_reset(ffb_torque_state_t *st)
{
  st->last_amps = 0.0f;
  st->last_vel = 0.0f;
  st->have_vel = false;
}

float ffb_clamp(float v, float limit)
{
  if (!(limit >= 0.0f))
  {
    return 0.0f;
  }
  return (v > limit) ? limit : ((v < -limit) ? -limit : v);
}

float ffb_vbus_derate(float vbus, float start_v, float end_v)
{
  if (!(start_v > 0.0f))
  {
    return 1.0f;                       /* disabled */
  }
  if (!finite_f(vbus) || !(end_v > start_v))
  {
    return 0.0f;
  }
  if (vbus <= start_v)
  {
    return 1.0f;
  }
  if (vbus >= end_v)
  {
    return 0.0f;
  }
  return (end_v - vbus) / (end_v - start_v);
}

float ffb_fx_spring(float kp, float angle_turns)
{
  return -kp * angle_turns;
}

float ffb_fx_damper(float kd, float vel_turns_s)
{
  return -kd * vel_turns_s;
}

float ffb_fx_friction(float amps, float vel_eps, float vel_turns_s)
{
  /* Linear around zero speed (no chatter at standstill), saturated above. */
  if (!(vel_eps > 0.0f))
  {
    return 0.0f;
  }
  return -amps * ffb_clamp(vel_turns_s / vel_eps, 1.0f);
}

float ffb_fx_endstop(const ffb_torque_cfg_t *cfg, const ffb_axis_t *ax)
{
  const float a = absf(ax->angle_turns);
  const float pen = a - cfg->half_range_turns;
  const float sign = (ax->angle_turns >= 0.0f) ? 1.0f : -1.0f;
  float x;
  float wall;
  float damp = 0.0f;

  if (pen <= 0.0f)
  {
    return 0.0f;
  }
  x = (cfg->endstop_width_turns > 0.0f) ? (pen / cfg->endstop_width_turns) : 1.0f;
  if (x > 1.0f)
  {
    x = 1.0f;
  }
  wall = cfg->endstop_amps * (x * x * (3.0f - (2.0f * x)));   /* smoothstep */
  /* Damping inside the wall, only when moving further out (it must never
   * push the wheel deeper into the stop). */
  if ((ax->vel_turns_s * sign) > 0.0f)
  {
    damp = cfg->endstop_damping * ax->vel_turns_s;
  }
  return (-sign * wall) - damp;
}

float ffb_layer_a(const ffb_torque_cfg_t *cfg, ffb_torque_state_t *st, const ffb_axis_t *ax,
                  float dt_s, bool spring_enabled)
{
  float sum = 0.0f;
  float accel = 0.0f;

  if (spring_enabled)
  {
    sum += ffb_fx_spring(cfg->spring_kp, ax->angle_turns);
  }
  sum += ffb_fx_damper(cfg->damper_kd, ax->vel_turns_s);
  sum += ffb_fx_friction(cfg->friction_amps, cfg->friction_vel_eps, ax->vel_turns_s);
  if (st->have_vel && (dt_s > 0.0f))
  {
    accel = (ax->vel_turns_s - st->last_vel) / dt_s;
  }
  st->last_vel = ax->vel_turns_s;
  st->have_vel = true;
  /* Inertia compensation placeholder: helps the user accelerate the 4 kg
   * rotor. 0 by default (profile ffb.inertia_amps_per_turn_s2). */
  sum += cfg->inertia_k * accel;
  sum += ffb_fx_endstop(cfg, ax);
  return sum;
}

float ffb_slew(float prev, float target, float rate_per_s, float dt_s)
{
  const float step = rate_per_s * dt_s;

  if (!(step >= 0.0f))
  {
    return prev;
  }
  if (target > prev + step)
  {
    return prev + step;
  }
  if (target < prev - step)
  {
    return prev - step;
  }
  return target;
}

bool ffb_amps_to_torque(float amps, float torque_constant, float kt_min, float kt_max, float *out)
{
  *out = 0.0f;
  if (!finite_f(amps) || !finite_f(torque_constant) || (torque_constant < kt_min)
      || (torque_constant > kt_max))
  {
    return false;
  }
  *out = amps * torque_constant;
  return true;
}

bool ffb_torque_step(const ffb_torque_cfg_t *cfg, ffb_torque_state_t *st, const ffb_axis_t *ax,
                     float host_amps, bool spring_enabled, float dt_s, float *amps_out)
{
  float limit;
  float target;

  *amps_out = 0.0f;
  if (!finite_f(ax->angle_turns) || !finite_f(ax->vel_turns_s) || !finite_f(host_amps)
      || !finite_f(dt_s) || !(dt_s > 0.0f))
  {
    ffb_torque_reset(st);
    return false;
  }
  limit = (cfg->max_amps < FFB_HARD_CAP_AMPS) ? cfg->max_amps : FFB_HARD_CAP_AMPS;
  target = ffb_layer_a(cfg, st, ax, dt_s, spring_enabled) + host_amps;
  target = ffb_clamp(target, limit);
  target = ffb_slew(st->last_amps, target, cfg->slew_amps_per_s, dt_s);
  target = ffb_clamp(target, limit);
  st->last_amps = target;
  *amps_out = target;
  return true;
}
