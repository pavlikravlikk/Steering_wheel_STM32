/**
 * @file    steering_map.h
 * @brief   Pure mapping of a position in turns to the HID X axis (host-tested).
 *
 * axis = 32768 + 32768 * (wheel_deg / (lock_to_lock_deg / 2)), clamped to
 * 0..65535, where wheel_deg = (pos - centre) / turns_per_wheel_rev * 360.
 */
#ifndef STEERING_MAP_H
#define STEERING_MAP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
  float lock_to_lock_deg;      /* e.g. 540                              */
  float turns_per_wheel_rev;   /* sensor turns per wheel turn (gears)   */
  bool  invert;
  /* state */
  bool  centred;               /* centre captured                       */
  float centre_turns;
} steering_map_t;

void steering_map_init(steering_map_t *m, float lock_to_lock_deg,
                       float turns_per_wheel_rev, bool invert);

/** Make @p pos_turns the new centre. Refused (returns false) for a NaN/inf
 *  position. */
bool steering_map_set_centre(steering_map_t *m, float pos_turns);

/** Wheel angle relative to centre in degrees (sign includes inversion). */
float steering_map_angle_deg(const steering_map_t *m, float pos_turns);

/** HID X value. Returns 32768 until a centre has been captured, or if the
 *  configuration/position is invalid. */
uint16_t steering_map_axis16(const steering_map_t *m, float pos_turns);

#endif /* STEERING_MAP_H */
