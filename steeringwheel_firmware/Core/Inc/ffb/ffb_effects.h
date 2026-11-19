/**
 * @file    ffb_effects.h
 * @brief   HID PID effect engine (layer B, pure, host-tested).
 *
 * Effect slots 1..FFB_MAX_EFFECTS (allocated by Create New Effect, released
 * by Block Free), start/stop/solo with loop count, duration and start delay,
 * per-effect gain, envelope, device gain, and the effect types constant,
 * ramp, square, sine, triangle, sawtooth up/down, spring, damper, inertia,
 * friction - on ONE axis (the steering axis, HID X).
 *
 * ffb_fx_force() returns a normalised force in -1..1 in the HID X frame;
 * ffb_service.c scales it to FFB_MAX_AMPS and feeds it into the layer A
 * pipeline (clamp, slew limit, safety state machine). Reset / stop-all /
 * disable-actuators / pause give 0.
 */
#ifndef FFB_EFFECTS_H
#define FFB_EFFECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "ffb_hid_desc.h"

typedef struct
{
  int16_t  cp_offset;        /* -10000..10000                              */
  int16_t  pos_coeff;        /* -10000..10000                              */
  int16_t  neg_coeff;
  uint16_t pos_sat;          /* 0..10000                                   */
  uint16_t neg_sat;
  uint16_t deadband;         /* 0..10000                                   */
} ffb_cond_t;

typedef struct
{
  bool       allocated;
  uint8_t    type;           /* FFB_ET_*                                   */
  uint16_t   duration_ms;    /* FFB_TIME_INFINITE = forever                */
  uint16_t   start_delay_ms;
  uint8_t    gain;           /* 0..255                                     */
  bool       dir_enable;
  uint16_t   direction;      /* 0..35999 (0.01 deg)                        */
  uint16_t   attack_level, fade_level, attack_ms, fade_ms;
  ffb_cond_t cond[2];        /* parameter block offset 0 (X) and 1         */
  uint16_t   magnitude;      /* periodic                                   */
  int16_t    offset;
  uint16_t   phase;          /* 0..35999                                   */
  uint16_t   period_ms;
  int16_t    constant;       /* constant force magnitude                   */
  int16_t    ramp_start, ramp_end;
  bool       playing;
  uint32_t   start_ms;
  uint16_t   loops;          /* plays in total; 0xFFFF = infinite          */
} ffb_fx_t;

/** Gain per kind of game effect, applied before the effects are summed (1 = unchanged). Texture (gravel, rumble strips, engine)
 *  arrives as weak periodic effects and needs a big gain; forces that depend on the wheel position or speed (spring, damper,
 *  friction, inertia) form a closed loop with the game and must stay low, or the wheel starts to swing. */
typedef struct
{
  float periodic;        /* sine, square, triangle, saw                          */
  float constant;        /* constant, ramp (cornering, impacts)                  */
  float condition;       /* spring, damper, inertia, friction                    */
  float periodic_cap;    /* the periodic part alone is limited to +-this (normalised, 0..1) */
  float texture_hz;         /* above this frequency the constant force is texture (gravel carried by the constant force); 0 = off */
  float texture_gain;
  float texture_deadband;   /* normalised */
  float texture_cap;        /* normalised, the texture part alone is limited to +-this */
  float constant_deadband;  /* the constant / ramp part below +-this is ignored, above it the rest is rescaled (normalised, 0..1) */
  float constant_cap;       /* the constant / ramp part alone is limited to +-this (normalised, 0..1): crashes, skids */
  float constant_filter_hz; /* low-pass on the constant / ramp part (0 = off): the game changes that force in steps */
  float direction_sign;  /* +1, or -1 = the direction of constant / ramp / periodic effects is mirrored (conditions are not) */
} ffb_fx_gains_t;

typedef struct
{
  ffb_fx_t fx[FFB_MAX_EFFECTS];
  ffb_fx_gains_t gains;      /* not touched by ffb_fx_reset (a game's Device Reset must not change the tuning) */
  uint8_t  device_gain;      /* 0..255                                     */
  bool     actuators_enabled;
  bool     paused;
  /* result of the last Create New Effect (Block Load report) */
  uint8_t  bl_index;
  uint8_t  bl_status;        /* FFB_BL_*                                   */
  float    constant_filt;     /* filtered constant / ramp sum                */
  float    constant_filt2;    /* texture split, first low-pass               */
  float    constant_filt3;    /* texture split, second low-pass (2nd order high-pass) */
  bool     have_constant_filt;
  /* inertia needs acceleration */
  float    last_vel;
  uint32_t last_ms;
  bool     have_vel;
  /* statistics */
  uint32_t created, freed, rejected, clipped, ignored;
} ffb_fx_engine_t;

void ffb_fx_init(ffb_fx_engine_t *e);

/** Set the per-kind gains (values that are not finite or negative become 1; the cap is limited to 0..1, 0 or less = 1). */
void ffb_fx_set_gains(ffb_fx_engine_t *e, float periodic, float constant, float condition, float periodic_cap);

/** Mirror the direction of the effects that carry one (constant, ramp, periodic): for a host whose left/right convention is
 *  the opposite of ours (a constant force that pushes the wheel AWAY from the centre where it should pull back). Spring, damper,
 *  friction and inertia depend on the wheel's own position or speed and are never mirrored. */
void ffb_fx_set_direction_invert(ffb_fx_engine_t *e, bool invert);

/** Low-pass (Hz) on the game's constant force and ramp only (texture and conditions stay as they are). 0 or less = off. */
void ffb_fx_set_constant_filter(ffb_fx_engine_t *e, float hz);

/** Limit of the constant force and ramp alone (a crash, a skid, the self-aligning force), normalised 0..1; 0 or >= 1 = no limit.
 *  A full-scale crash force on a light rotor throws the wheel for almost two turns; this keeps an impact a jolt. */
void ffb_fx_set_constant_cap(ffb_fx_engine_t *e, float cap);

/** Dead band of the constant force and ramp (normalised 0..1; 0 = off). The game sends small, restless corrections (road, camber,
 *  tyre noise) all the time: on a straight road they were felt as a constant jerking. A force below the band is dropped; above it
 *  the rest is rescaled so that a full-scale force is still full scale (no step at the edge of the band). */
void ffb_fx_set_constant_deadband(ffb_fx_engine_t *e, float db);

/** Many games carry the texture of the road (gravel, kerbs) as a fast wobble of the CONSTANT force. The slow part (low-pass
 *  ffb_fx_set_constant_filter, dead band, cap) is the push of the car; the part above @p hz is the texture: it gets its own
 *  dead band, gain and cap (all normalised to the full scale except the gain). The band between the two corner frequencies
 *  (the restless corrections that jerked the wheel on a straight road) stays removed. hz or gain <= 0 = off. */
void ffb_fx_set_constant_texture(ffb_fx_engine_t *e, float hz, float gain, float deadband, float cap);

/** PID Device Reset: stop and free everything, defaults back. */
void ffb_fx_reset(ffb_fx_engine_t *e);

/** Create New Effect: lowest free slot. Sets bl_index/bl_status and returns
 *  the block index (0 = full / invalid type). */
uint8_t ffb_fx_create(ffb_fx_engine_t *e, uint8_t type);

void ffb_fx_free(ffb_fx_engine_t *e, uint8_t index);

/** The slot if allocated, else NULL (all setters ignore invalid indexes). */
ffb_fx_t *ffb_fx_get(ffb_fx_engine_t *e, uint8_t index);

void ffb_fx_operation(ffb_fx_engine_t *e, uint8_t index, uint8_t op, uint8_t loop_count,
                      uint32_t now_ms);
void ffb_fx_device_control(ffb_fx_engine_t *e, uint8_t cmd);
void ffb_fx_set_device_gain(ffb_fx_engine_t *e, uint8_t gain);

uint16_t ffb_fx_ram_available(const ffb_fx_engine_t *e);
bool ffb_fx_any_playing(const ffb_fx_engine_t *e);

/** Sum of all playing effects, normalised -1..1 (HID X frame).
 *  @p pos -1..1 (wheel position over half the lock range),
 *  @p vel -1..1 (wheel speed over the overspeed limit). */
float ffb_fx_force(ffb_fx_engine_t *e, uint32_t now_ms, float pos, float vel, bool *active);

/** Value of one effect at time @p now (exposed for tests). */
float ffb_fx_eval(ffb_fx_engine_t *e, ffb_fx_t *fx, uint32_t now_ms, float pos, float vel,
                  float accel);

/** sin(2*pi*phase) for phase 0..1, max error about 0.1 %, no libm. */
float ffb_sin_turns(float phase);

#endif /* FFB_EFFECTS_H */
