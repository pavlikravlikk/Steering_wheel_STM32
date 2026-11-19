/**
 * @file    centering_law.c
 * @brief   Pure centring-spring math. NOT linked into the firmware; no I/O.
 */
#include "centering_law.h"

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

static float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

void centering_default_cfg(centering_cfg_t *cfg)
{
  cfg->kp = CENTERING_DEFAULT_KP;
  cfg->kd = CENTERING_DEFAULT_KD;
  cfg->max_amps = CENTERING_DEFAULT_MAX_AMPS;
  cfg->deadband_turns = CENTERING_DEFAULT_DEADBAND;
  cfg->slew_amps_per_s = CENTERING_DEFAULT_SLEW;
}

void centering_reset(centering_state_t *st)
{
  st->last_out = 0.0f;
}

static bool cfg_valid(const centering_cfg_t *c)
{
  return finite_f(c->kp) && (c->kp >= 0.0f)
      && finite_f(c->kd) && (c->kd >= 0.0f)
      && finite_f(c->max_amps) && (c->max_amps >= 0.0f)
      && finite_f(c->deadband_turns) && (c->deadband_turns >= 0.0f)
      && finite_f(c->slew_amps_per_s) && (c->slew_amps_per_s > 0.0f);
}

float centering_compute(const centering_cfg_t *cfg, centering_state_t *st,
                        float pos_turns, float centre_turns, float vel_turns_s,
                        float dt_s, bool input_valid)
{
  float err;
  float target;
  float max_step;
  float limit;

  if (!input_valid || (cfg == 0) || !cfg_valid(cfg) || !finite_f(pos_turns)
      || !finite_f(centre_turns) || !finite_f(vel_turns_s) || !finite_f(dt_s)
      || !(dt_s > 0.0f))
  {
    centering_reset(st);   /* fail-safe: zero now, not slewed */
    return 0.0f;
  }

  /* Deadband, continuous at its edges. */
  err = pos_turns - centre_turns;
  if (err > cfg->deadband_turns)
  {
    err -= cfg->deadband_turns;
  }
  else if (err < -cfg->deadband_turns)
  {
    err += cfg->deadband_turns;
  }
  else
  {
    err = 0.0f;
  }

  limit = (cfg->max_amps < CENTERING_HARD_MAX_AMPS) ? cfg->max_amps : CENTERING_HARD_MAX_AMPS;
  target = clampf((-cfg->kp * err) - (cfg->kd * vel_turns_s), -limit, limit);

  /* Slew-rate limit relative to the previous output. */
  max_step = cfg->slew_amps_per_s * dt_s;
  target = clampf(target, st->last_out - max_step, st->last_out + max_step);
  target = clampf(target, -limit, limit);

  st->last_out = target;
  return target;
}
