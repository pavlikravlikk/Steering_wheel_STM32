/**
 * @file    steering_map.c
 * @brief   Pure turns -> HID axis mapping. Must not include any HAL header.
 */
#include "steering_map.h"

static bool is_finite_f(float v)
{
  /* NaN fails every comparison; inf exceeds FLT_MAX. */
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

void steering_map_init(steering_map_t *m, float lock_to_lock_deg,
                       float turns_per_wheel_rev, bool invert)
{
  m->lock_to_lock_deg = lock_to_lock_deg;
  m->turns_per_wheel_rev = turns_per_wheel_rev;
  m->invert = invert;
  m->centred = false;
  m->centre_turns = 0.0f;
}

bool steering_map_set_centre(steering_map_t *m, float pos_turns)
{
  if (!is_finite_f(pos_turns))
  {
    return false;
  }
  m->centre_turns = pos_turns;
  m->centred = true;
  return true;
}

float steering_map_angle_deg(const steering_map_t *m, float pos_turns)
{
  float deg;

  if (!m->centred || !is_finite_f(pos_turns) || !(m->turns_per_wheel_rev > 0.0f))
  {
    return 0.0f;
  }
  deg = ((pos_turns - m->centre_turns) / m->turns_per_wheel_rev) * 360.0f;
  return m->invert ? -deg : deg;
}

uint16_t steering_map_axis16(const steering_map_t *m, float pos_turns)
{
  float half;
  float frac;
  float x;

  if (!m->centred || !is_finite_f(pos_turns) || !(m->lock_to_lock_deg > 0.0f)
      || !(m->turns_per_wheel_rev > 0.0f))
  {
    return 32768U;
  }
  half = m->lock_to_lock_deg * 0.5f;
  frac = steering_map_angle_deg(m, pos_turns) / half;
  if (frac <= -1.0f)
  {
    return 0U;
  }
  if (frac >= 1.0f)
  {
    return 65535U;
  }
  x = 32768.0f + (frac * 32768.0f) + 0.5f;   /* round to nearest */
  if (x >= 65535.0f)
  {
    return 65535U;
  }
  if (x <= 0.0f)
  {
    return 0U;
  }
  return (uint16_t)x;
}
