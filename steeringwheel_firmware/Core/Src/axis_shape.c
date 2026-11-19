/**
 * @file    axis_shape.c
 * @brief   Steering deadzone + cubic curve. Pure, no libm.
 */
#include "axis_shape.h"

uint16_t axis_shape(uint16_t x, float deadzone, float curve_k)
{
  /* the positive half spans 32767 counts, the negative 32768 */
  const float half = (x >= 32768U) ? 32767.0f : 32768.0f;
  float n = ((float)x - 32768.0f) / half;
  float a;
  float out;

  if (!(deadzone >= 0.0f) || (deadzone > 0.5f)) deadzone = 0.0f;   /* invalid -> off */
  if (!(curve_k >= 0.0f) || (curve_k > 1.0f))   curve_k = 0.0f;
  if ((deadzone == 0.0f) && (curve_k == 0.0f))
  {
    return x;                                   /* exact identity */
  }
  a = (n < 0.0f) ? -n : n;
  a = (a <= deadzone) ? 0.0f : ((a - deadzone) / (1.0f - deadzone));
  if (a > 1.0f) a = 1.0f;
  a = ((1.0f - curve_k) * a) + (curve_k * a * a * a);
  out = 32768.0f + (((n < 0.0f) ? -a : a) * half);
  if (out < 0.0f) out = 0.0f;
  if (out > 65535.0f) out = 65535.0f;
  return (uint16_t)(out + 0.5f > 65535.0f ? 65535.0f : out + 0.5f);
}
