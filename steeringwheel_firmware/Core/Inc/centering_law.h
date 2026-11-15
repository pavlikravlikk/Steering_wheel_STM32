/**
 * @file    centering_law.h
 * @brief   Gentle centring spring, PURE MATH ONLY (host-tested).
 *
 *   torque = clamp(-Kp*(pos-centre) - Kd*vel, +-max_amps)
 *   with a deadband on (pos-centre) and a slew-rate limiter on the output.
 *
 * !!! NOT WIRED TO THE ODRIVE. !!!
 * This file is deliberately NOT part of the firmware build (see
 * CMakeLists.txt / docs/DECISIONS.md). There is no code path that sends a
 * torque, state or any other write command to the ODrive. Connecting this
 * requires: the belt installed, the encoder calibrated, tightened ODrive
 * limits, the ODrive watchdog enabled and fed, an e-stop, and the owner's
 * explicit approval.
 */
#ifndef CENTERING_LAW_H
#define CENTERING_LAW_H

#include <stdbool.h>

/* Safe-side compile-time defaults. */
#define CENTERING_HARD_MAX_AMPS        1.0f   /* absolute cap, cannot be raised by config */
#define CENTERING_DEFAULT_MAX_AMPS     1.0f
#define CENTERING_DEFAULT_KP           0.5f   /* A per wheel turn          */
#define CENTERING_DEFAULT_KD           0.05f  /* A per (wheel turn / s)    */
#define CENTERING_DEFAULT_DEADBAND     0.005f /* wheel turns (~1.8 deg)    */
#define CENTERING_DEFAULT_SLEW         5.0f   /* A per second              */

typedef struct
{
  float kp;              /* A / turn            (>= 0) */
  float kd;              /* A / (turn/s)        (>= 0) */
  float max_amps;        /* clamped to CENTERING_HARD_MAX_AMPS */
  float deadband_turns;  /* >= 0 */
  float slew_amps_per_s; /* > 0 */
} centering_cfg_t;

typedef struct
{
  float last_out;        /* A */
} centering_state_t;

void centering_default_cfg(centering_cfg_t *cfg);
void centering_reset(centering_state_t *st);

/** Fail-safe wrapper: returns 0 A (and resets the slew state) when
 *  @p input_valid is false, any input is NaN/inf, dt <= 0, or the config is
 *  invalid. Otherwise the slew-limited, clamped spring/damper torque. */
float centering_compute(const centering_cfg_t *cfg, centering_state_t *st,
                        float pos_turns, float centre_turns, float vel_turns_s,
                        float dt_s, bool input_valid);

#endif /* CENTERING_LAW_H */
