/**
 * @file    ffb_effects.c
 * @brief   HID PID effect engine. Pure (no HAL, no libm).
 */
#include "ffb_effects.h"

#include <string.h>

#define MAG       ((float)FFB_SCALE_MAG)
#define LOOPS_INF 0xFFFFU
/* Inertia: acceleration that gives full scale (normalised vel units per s). */
#define ACCEL_FULL_SCALE  10.0f
/* Friction: full force above this normalised speed. */
#define FRICTION_EPS      0.02f

static float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static float absf(float v)
{
  return (v < 0.0f) ? -v : v;
}

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

float ffb_sin_turns(float phase)
{
  /* parabola approximation of sin on -pi..pi, refined (same as hid_report.c) */
  float x;
  float y;
  phase -= (float)(int32_t)phase;          /* 0..1 (also for negatives)  */
  if (phase < 0.0f)
  {
    phase += 1.0f;
  }
  x = (phase * 6.2831853f) - 3.1415927f;   /* -pi..pi                     */
  y = (1.2732395f * x) - (0.4052847f * x * absf(x));
  y = 0.225f * ((y * absf(y)) - y) + y;
  return -y;                               /* sin(x + pi) = -sin(x)       */
}

static void fx_defaults(ffb_fx_t *f)
{
  memset(f, 0, sizeof(*f));
  f->duration_ms = FFB_TIME_INFINITE;
  f->gain = 255U;
  f->dir_enable = true;
  f->direction = 9000U;                    /* 90 deg: full +X (see FFB_SAFETY) */
  for (unsigned k = 0U; k < 2U; k++)
  {
    f->cond[k].pos_sat = (uint16_t)FFB_SCALE_MAG;
    f->cond[k].neg_sat = (uint16_t)FFB_SCALE_MAG;
  }
  f->loops = 1U;
}

void ffb_fx_init(ffb_fx_engine_t *e)
{
  memset(e, 0, sizeof(*e));
  ffb_fx_reset(e);
}

void ffb_fx_reset(ffb_fx_engine_t *e)
{
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    fx_defaults(&e->fx[i]);
  }
  e->device_gain = 255U;
  e->actuators_enabled = true;
  e->paused = false;
  e->bl_index = 0U;
  e->bl_status = FFB_BL_PENDING;
  e->have_vel = false;
}

static bool index_ok(uint8_t index)
{
  return (index >= 1U) && (index <= FFB_MAX_EFFECTS);
}

ffb_fx_t *ffb_fx_get(ffb_fx_engine_t *e, uint8_t index)
{
  if (!index_ok(index) || !e->fx[index - 1U].allocated)
  {
    return NULL;
  }
  return &e->fx[index - 1U];
}

uint8_t ffb_fx_create(ffb_fx_engine_t *e, uint8_t type)
{
  if ((type < 1U) || (type > FFB_ET_COUNT))
  {
    e->bl_index = 0U;
    e->bl_status = FFB_BL_ERROR;
    e->rejected++;
    return 0U;
  }
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    if (!e->fx[i].allocated)
    {
      fx_defaults(&e->fx[i]);
      e->fx[i].allocated = true;
      e->fx[i].type = type;
      e->bl_index = (uint8_t)(i + 1U);    /* lowest first: index 1 = logical min,
                                             so Linux does not assume a built-in
                                             autocentre spring */
      e->bl_status = FFB_BL_SUCCESS;
      e->created++;
      return e->bl_index;
    }
  }
  e->bl_index = 0U;
  e->bl_status = FFB_BL_FULL;
  e->rejected++;
  return 0U;
}

void ffb_fx_free(ffb_fx_engine_t *e, uint8_t index)
{
  if (ffb_fx_get(e, index) != NULL)
  {
    fx_defaults(&e->fx[index - 1U]);       /* also stops it */
    e->freed++;
  }
  else
  {
    e->ignored++;
  }
}

static void stop_all(ffb_fx_engine_t *e)
{
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    e->fx[i].playing = false;
  }
}

void ffb_fx_operation(ffb_fx_engine_t *e, uint8_t index, uint8_t op, uint8_t loop_count,
                      uint32_t now_ms)
{
  ffb_fx_t *f = ffb_fx_get(e, index);

  if (f == NULL)
  {
    e->ignored++;
    return;
  }
  switch (op)
  {
    case FFB_OP_START_SOLO:
      stop_all(e);
      /* fall through */
    case FFB_OP_START:
      f->playing = true;
      f->start_ms = now_ms;
      f->loops = (loop_count == 255U) ? LOOPS_INF : ((loop_count == 0U) ? 1U : loop_count);
      break;
    case FFB_OP_STOP:
      f->playing = false;
      break;
    default:
      e->ignored++;
      break;
  }
}

void ffb_fx_device_control(ffb_fx_engine_t *e, uint8_t cmd)
{
  switch (cmd)
  {
    case FFB_DC_ENABLE_ACTUATORS:  e->actuators_enabled = true;  break;
    case FFB_DC_DISABLE_ACTUATORS: e->actuators_enabled = false; break;
    case FFB_DC_STOP_ALL:          stop_all(e);                  break;
    case FFB_DC_RESET:             ffb_fx_reset(e);              break;
    case FFB_DC_PAUSE:             e->paused = true;             break;
    case FFB_DC_CONTINUE:          e->paused = false;            break;
    default:                       e->ignored++;                 break;
  }
}

void ffb_fx_set_device_gain(ffb_fx_engine_t *e, uint8_t gain)
{
  e->device_gain = gain;
}

uint16_t ffb_fx_ram_available(const ffb_fx_engine_t *e)
{
  uint16_t n = 0U;
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    if (!e->fx[i].allocated)
    {
      n++;
    }
  }
  return (uint16_t)(n * (uint16_t)sizeof(ffb_fx_t));
}

bool ffb_fx_any_playing(const ffb_fx_engine_t *e)
{
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    if (e->fx[i].allocated && e->fx[i].playing)
    {
      return true;
    }
  }
  return false;
}

/* Envelope applied to a magnitude (>= 0) at loop time t. */
static float envelope(const ffb_fx_t *f, float mag, uint32_t t)
{
  if ((f->attack_ms > 0U) && (t < f->attack_ms))
  {
    const float a = (float)f->attack_level;
    return a + ((mag - a) * (float)t / (float)f->attack_ms);
  }
  if ((f->duration_ms != FFB_TIME_INFINITE) && (f->fade_ms > 0U) && (f->duration_ms > f->fade_ms)
      && (t > (uint32_t)(f->duration_ms - f->fade_ms)))
  {
    const float fl = (float)f->fade_level;
    const uint32_t left = (t < f->duration_ms) ? (f->duration_ms - t) : 0U;
    return fl + ((mag - fl) * (float)left / (float)f->fade_ms);
  }
  return mag;
}

static float condition(const ffb_cond_t *c, float metric)
{
  const float x = metric - ((float)c->cp_offset / MAG);
  const float db = (float)c->deadband / MAG;
  float out = 0.0f;

  if (x > db)
  {
    out = -((float)c->pos_coeff / MAG) * (x - db);
  }
  else if (x < -db)
  {
    out = -((float)c->neg_coeff / MAG) * (x + db);
  }
  /* saturation: positive side limits the force for x > 0 and vice versa */
  if (x >= 0.0f)
  {
    out = clampf(out, -(float)c->pos_sat / MAG, (float)c->pos_sat / MAG);
  }
  else
  {
    out = clampf(out, -(float)c->neg_sat / MAG, (float)c->neg_sat / MAG);
  }
  return out;
}

static float friction(const ffb_cond_t *c, float vel)
{
  const float k = (vel >= 0.0f) ? ((float)c->pos_coeff / MAG) : ((float)c->neg_coeff / MAG);
  const float sat = ((vel >= 0.0f) ? (float)c->pos_sat : (float)c->neg_sat) / MAG;
  return clampf(-k * clampf(vel / FRICTION_EPS, -1.0f, 1.0f), -sat, sat);
}

static float waveform(uint8_t type, float ph)
{
  switch (type)
  {
    case FFB_ET_SQUARE:   return (ph < 0.5f) ? 1.0f : -1.0f;
    case FFB_ET_SINE:     return ffb_sin_turns(ph);
    case FFB_ET_TRIANGLE: return (ph < 0.25f) ? (4.0f * ph)
                               : ((ph < 0.75f) ? (2.0f - (4.0f * ph)) : ((4.0f * ph) - 4.0f));
    case FFB_ET_SAW_UP:   return -1.0f + (2.0f * ph);
    case FFB_ET_SAW_DOWN: return 1.0f - (2.0f * ph);
    default:              return 0.0f;
  }
}

float ffb_fx_eval(ffb_fx_engine_t *e, ffb_fx_t *f, uint32_t now_ms, float pos, float vel, float accel)
{
  uint32_t t;
  uint32_t elapsed;
  float out = 0.0f;
  const bool timed = (f->duration_ms != FFB_TIME_INFINITE) && (f->duration_ms > 0U);
  (void)e;

  if (!f->allocated || !f->playing)
  {
    return 0.0f;
  }
  elapsed = now_ms - f->start_ms;
  if (elapsed < f->start_delay_ms)
  {
    return 0.0f;                           /* waiting for the start delay */
  }
  t = elapsed - f->start_delay_ms;
  if (timed)
  {
    const uint32_t d = f->duration_ms;
    if ((f->loops != LOOPS_INF) && (t >= d * (uint32_t)f->loops))
    {
      f->playing = false;                  /* all loops done */
      return 0.0f;
    }
    t %= d;
  }

  switch (f->type)
  {
    case FFB_ET_CONSTANT:
    {
      const float m = absf((float)f->constant);
      const float s = (f->constant < 0) ? -1.0f : 1.0f;
      out = s * envelope(f, m, t) / MAG;
      break;
    }
    case FFB_ET_RAMP:
    {
      const float a = (float)f->ramp_start;
      const float b = (float)f->ramp_end;
      const float v = timed ? (a + ((b - a) * (float)t / (float)f->duration_ms)) : a;
      const float m = (absf(a) > absf(b)) ? absf(a) : absf(b);
      out = (m > 0.0f) ? (v * envelope(f, m, t) / m) / MAG : 0.0f;
      break;
    }
    case FFB_ET_SQUARE:
    case FFB_ET_SINE:
    case FFB_ET_TRIANGLE:
    case FFB_ET_SAW_UP:
    case FFB_ET_SAW_DOWN:
    {
      float ph = (float)f->phase / (float)(FFB_DIR_MAX + 1U);
      if (f->period_ms > 0U)
      {
        ph += (float)(t % f->period_ms) / (float)f->period_ms;
      }
      ph -= (float)(int32_t)ph;
      out = ((float)f->offset + (envelope(f, (float)f->magnitude, t) * waveform(f->type, ph))) / MAG;
      break;
    }
    case FFB_ET_SPRING:   return condition(&f->cond[0], pos) * ((float)f->gain / 255.0f);
    case FFB_ET_DAMPER:   return condition(&f->cond[0], vel) * ((float)f->gain / 255.0f);
    case FFB_ET_INERTIA:  return condition(&f->cond[0], accel) * ((float)f->gain / 255.0f);
    case FFB_ET_FRICTION: return friction(&f->cond[0], vel) * ((float)f->gain / 255.0f);
    default:
      return 0.0f;
  }
  /* direction: X component of the force vector (0 deg = +Y, 90 deg = +X) */
  if (f->dir_enable)
  {
    out *= ffb_sin_turns((float)f->direction / (float)(FFB_DIR_MAX + 1U));
  }
  return clampf(out, -1.0f, 1.0f) * ((float)f->gain / 255.0f);
}

float ffb_fx_force(ffb_fx_engine_t *e, uint32_t now_ms, float pos, float vel, bool *active)
{
  float sum = 0.0f;
  float accel = 0.0f;
  bool any = false;

  if (!finite_f(pos) || !finite_f(vel))
  {
    pos = 0.0f;
    vel = 0.0f;
  }
  if (e->have_vel && (now_ms != e->last_ms))
  {
    accel = clampf(((vel - e->last_vel) * 1000.0f / (float)(now_ms - e->last_ms)) / ACCEL_FULL_SCALE,
                   -1.0f, 1.0f);
  }
  e->last_vel = vel;
  e->last_ms = now_ms;
  e->have_vel = true;

  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    ffb_fx_t *f = &e->fx[i];
    if (f->allocated && f->playing)
    {
      sum += ffb_fx_eval(e, f, now_ms, pos, vel, accel);
      any = any || f->playing;
    }
  }
  if (!e->actuators_enabled || e->paused)
  {
    *active = false;
    return 0.0f;
  }
  sum *= (float)e->device_gain / 255.0f;
  if ((sum > 1.0f) || (sum < -1.0f))
  {
    e->clipped++;                          /* total force limited to full scale */
    sum = clampf(sum, -1.0f, 1.0f);
  }
  *active = any;
  return sum;
}
