/**
 * @file    ffb_torque.h
 * @brief   Device-side torque pipeline (pure, host-tested):
 *
 *     layer A effects (spring, damper, friction, inertia, soft end stops)
 *   + layer B game force (ffb_effects, already in amps)
 *   -> clamp to FFB_MAX_AMPS -> slew-rate limit -> amps * torque_constant
 *   -> value for axis0.controller.input_torque
 *
 * Sign convention: positive amps = positive ODrive torque = the direction in
 * which pos_estimate increases (the motor frame). The wheel angle is
 * measured in the same frame; HID inversion is applied only to game forces.
 */
#ifndef FFB_TORQUE_H
#define FFB_TORQUE_H

#include <stdbool.h>
#include <stdint.h>

#define FFB_SPRING_CURVE_N 6U
#define FFB_SPRING_CURVE_STEP_DEG 90.0f

typedef struct
{
  float spring_kp;            /* A per wheel turn (used only when spring_curve_on is false) */
  bool  spring_curve_on;      /* centring force from the curve below instead of the straight line */
  float spring_curve[FFB_SPRING_CURVE_N]; /* A at 90, 180, 270, 360, 450, 540 deg from the centre; linear in between,
                                 from 0 at the centre, and held at the last value beyond */
  float damper_kd;            /* A per wheel turn/s                        */
  float friction_amps;        /* A                                         */
  float friction_vel_eps;     /* turns/s for full friction                 */
  float inertia_k;            /* A per wheel turn/s^2 (placeholder)        */
  float half_range_turns;     /* lock-to-lock / 2, in wheel turns          */
  float endstop_width_turns;  /* wall build-up distance                    */
  float endstop_amps;         /* wall force at full width                  */
  float endstop_damping;      /* A per wheel turn/s inside the wall        */
  float max_amps;             /* total clamp (<= FFB_HARD_CAP_AMPS)        */
  float slew_amps_per_s;
  float regen_power_w;        /* mechanical power the motor may take OUT of the wheel (0 = no limit)  */
  float kt_nominal;           /* Nm per A (profile), only for that power estimate                     */
} ffb_torque_cfg_t;

typedef struct
{
  float last_amps;            /* output of the slew limiter                */
  float last_vel;
  bool  have_vel;
} ffb_torque_state_t;

/** Wheel kinematics in the motor frame. */
typedef struct
{
  float angle_turns;          /* wheel turns from centre                   */
  float vel_turns_s;          /* wheel turns/s                             */
} ffb_axis_t;

void ffb_torque_default_cfg(ffb_torque_cfg_t *cfg);
void ffb_torque_reset(ffb_torque_state_t *st);

float ffb_fx_spring(float kp, float angle_turns);
/** Centring force from a curve: amps[i] at (i+1) * FFB_SPRING_CURVE_STEP_DEG degrees, linear in between and from 0 at the centre,
 *  held at the last value beyond. Restoring (opposite sign to the angle), symmetric. */
float ffb_fx_spring_curve(const float amps[FFB_SPRING_CURVE_N], float angle_turns);
float ffb_fx_damper(float kd, float vel_turns_s);
float ffb_fx_friction(float amps, float vel_eps, float vel_turns_s);
/** Soft end stop: 0 inside +-half_range, then a smoothstep wall that reaches
 *  endstop_amps after endstop_width, plus damping while inside the wall. */
float ffb_fx_endstop(const ffb_torque_cfg_t *cfg, const ffb_axis_t *ax);

/** Sum of the layer A effects (amps, motor frame).
 *  @p spring_enabled false while the game plays effects (if configured). */
float ffb_layer_a(const ffb_torque_cfg_t *cfg, ffb_torque_state_t *st, const ffb_axis_t *ax,
                  float dt_s, bool spring_enabled);

float ffb_clamp(float v, float limit);

/** Regeneration power limiter. When the commanded current opposes the wheel's motion the motor brakes the wheel and the energy
 *  goes into the supply (it cannot take it: the voltage rises, 24 -> 28 V on 2026-11-18). The braking power is
 *  kt * |amps| * 2*pi*|speed|; this limits |amps| so that it stays at or below @p power_w. A force that pushes WITH the motion,
 *  or a slow wheel, is not limited. power_w <= 0 or kt <= 0 disables it. */
float ffb_regen_limit(float amps, float vel_turns_s, float kt, float power_w);

/** Scale (0..1) that fades the commanded current out while the supply voltage rises (regeneration: the wheel is forced
 *  against the motor and the energy goes into the bus). 1 below @p start_v, 0 at and above @p end_v, linear between.
 *  start_v <= 0 disables it (always 1). A voltage that is not finite gives 0 (fail safe). */
float ffb_vbus_derate(float vbus, float start_v, float end_v);

/** Move from @p prev towards @p target by at most rate*dt. */
float ffb_slew(float prev, float target, float rate_per_s, float dt_s);

/** amps -> controller torque units using the torque_constant READ from the
 *  ODrive. false (and *out = 0) if kt is outside [kt_min, kt_max] or not finite. */
bool ffb_amps_to_torque(float amps, float torque_constant, float kt_min, float kt_max, float *out);

/** Full pipeline. @p host_amps = game force (amps, motor frame).
 *  Returns false if any input is not finite (the caller must fault);
 *  then *amps_out = 0. Otherwise *amps_out is clamped and slew-limited. */
bool ffb_torque_step(const ffb_torque_cfg_t *cfg, ffb_torque_state_t *st, const ffb_axis_t *ax,
                     float host_amps, bool spring_enabled, float dt_s, float *amps_out);

#endif /* FFB_TORQUE_H */
