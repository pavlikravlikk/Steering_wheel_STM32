/**
 * @file    axis_shape.h
 * @brief   Optional steering-axis shaping (pure, host-tested): a centre
 *          deadzone and a cubic response curve. OFF by default
 *          (AXIS_SHAPING_ENABLE = 0); with the defaults it is the identity.
 *
 *   n   = (x - 32768) / 32768                  (-1..1)
 *   dz  : |n| <= d -> 0, else (|n| - d)/(1 - d) (continuous, full range kept)
 *   out = (1 - k) * n + k * n^3                (k = 0 linear .. 1 cubic)
 */
#ifndef AXIS_SHAPE_H
#define AXIS_SHAPE_H

#include <stdint.h>

#ifndef AXIS_SHAPING_ENABLE
#define AXIS_SHAPING_ENABLE   0
#endif
#ifndef AXIS_DEADZONE         /* fraction of half range, 0..0.5          */
#define AXIS_DEADZONE         0.0f
#endif
#ifndef AXIS_CURVE_K          /* 0..1                                    */
#define AXIS_CURVE_K          0.0f
#endif

uint16_t axis_shape(uint16_t x, float deadzone, float curve_k);

#endif /* AXIS_SHAPE_H */
