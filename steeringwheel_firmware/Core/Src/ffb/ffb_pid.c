/**
 * @file    ffb_pid.c
 * @brief   HID PID report parser/dispatcher, ISR queue and snapshots. Pure
 *          except for the single global instance at the end (no HAL calls).
 */
#include "ffb_pid.h"

#include "ffb_config.h"

#include <string.h>

static uint16_t le16(const uint8_t *p)
{
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static int16_t les16(const uint8_t *p)
{
  return (int16_t)le16(p);
}

static int16_t mag_s(const uint8_t *p)          /* -10000..10000, clamped */
{
  int16_t v = les16(p);
  if (v > FFB_SCALE_MAG)  v = FFB_SCALE_MAG;
  if (v < -FFB_SCALE_MAG) v = -FFB_SCALE_MAG;
  return v;
}

static uint16_t mag_u(const uint8_t *p)         /* 0..10000, clamped */
{
  uint16_t v = le16(p);
  return (v > (uint16_t)FFB_SCALE_MAG) ? (uint16_t)FFB_SCALE_MAG : v;
}

static uint16_t angle_u(const uint8_t *p)       /* 0..35999, clamped */
{
  uint16_t v = le16(p);
  return (v > FFB_DIR_MAX) ? (uint16_t)FFB_DIR_MAX : v;
}

static void put16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)(v >> 8);
}

/* Expected length and report type for each ID (0 = unknown). */
static size_t expected(uint8_t id, uint8_t *type)
{
  *type = FFB_REPORT_OUTPUT;
  switch (id)
  {
    case FFB_RID_SET_EFFECT:     return FFB_LEN_SET_EFFECT;
    case FFB_RID_SET_ENVELOPE:   return FFB_LEN_SET_ENVELOPE;
    case FFB_RID_SET_CONDITION:  return FFB_LEN_SET_CONDITION;
    case FFB_RID_SET_PERIODIC:   return FFB_LEN_SET_PERIODIC;
    case FFB_RID_SET_CONSTANT:   return FFB_LEN_SET_CONSTANT;
    case FFB_RID_SET_RAMP:       return FFB_LEN_SET_RAMP;
    case FFB_RID_EFFECT_OP:      return FFB_LEN_EFFECT_OP;
    case FFB_RID_BLOCK_FREE:     return FFB_LEN_BLOCK_FREE;
    case FFB_RID_DEVICE_CONTROL: return FFB_LEN_DEVICE_CONTROL;
    case FFB_RID_DEVICE_GAIN:    return FFB_LEN_DEVICE_GAIN;
    case FFB_RID_CREATE_EFFECT:  *type = FFB_REPORT_FEATURE; return FFB_LEN_CREATE_EFFECT;
    default:                     return 0U;
  }
}

bool ffb_pid_dispatch(ffb_fx_engine_t *e, ffb_pid_stats_t *st, uint8_t type,
                      const uint8_t *b, size_t len, uint32_t now_ms)
{
  uint8_t want_type;
  size_t need;
  ffb_fx_t *f;

  if ((b == NULL) || (len == 0U))
  {
    st->ignored_short++;
    return false;
  }
  need = expected(b[0], &want_type);
  if (need == 0U)
  {
    st->ignored_id++;
    return false;
  }
  if (type != want_type)
  {
    st->ignored_type++;
    return false;
  }
  if (len < need)
  {
    st->ignored_short++;                 /* truncated: never act on it */
    return false;
  }

  switch (b[0])
  {
    case FFB_RID_CREATE_EFFECT:
      (void)ffb_fx_create(e, b[1]);
      break;

    case FFB_RID_BLOCK_FREE:
      ffb_fx_free(e, b[1]);
      break;

    case FFB_RID_EFFECT_OP:
      ffb_fx_operation(e, b[1], b[2], b[3], now_ms);
      break;

    case FFB_RID_DEVICE_CONTROL:
      ffb_fx_device_control(e, b[1]);
      break;

    case FFB_RID_DEVICE_GAIN:
      ffb_fx_set_device_gain(e, b[1]);
      break;

    default:
      f = ffb_fx_get(e, b[1]);
      if (f == NULL)
      {
        e->ignored++;                    /* not allocated: ignore */
        return false;
      }
      switch (b[0])
      {
        case FFB_RID_SET_EFFECT:
        {
          const uint16_t dur = le16(&b[3]);
          if ((b[2] < 1U) || (b[2] > FFB_ET_COUNT))
          {
            e->ignored++;
            return false;
          }
          f->type = b[2];
          /* 0 and 0xFFFF both mean "forever" (Linux sends all ones). */
          f->duration_ms = (dur == 0U) ? FFB_TIME_INFINITE : dur;
          f->start_delay_ms = le16(&b[9]);
          f->gain = b[11];
          f->dir_enable = (b[13] & 0x02U) != 0U;
          f->direction = angle_u(&b[14]);
          break;
        }
        case FFB_RID_SET_ENVELOPE:
          f->attack_level = mag_u(&b[2]);
          f->fade_level = mag_u(&b[4]);
          f->attack_ms = le16(&b[6]);
          f->fade_ms = le16(&b[8]);
          break;
        case FFB_RID_SET_CONDITION:
        {
          const uint8_t pbo = (uint8_t)(b[2] & 0x0FU);
          ffb_cond_t *c;
          if (pbo > 1U)
          {
            e->ignored++;
            return false;
          }
          c = &f->cond[pbo];
          c->cp_offset = mag_s(&b[3]);
          c->pos_coeff = mag_s(&b[5]);
          c->neg_coeff = mag_s(&b[7]);
          c->pos_sat = mag_u(&b[9]);
          c->neg_sat = mag_u(&b[11]);
          c->deadband = mag_u(&b[13]);
          break;
        }
        case FFB_RID_SET_PERIODIC:
          f->magnitude = mag_u(&b[2]);
          f->offset = mag_s(&b[4]);
          f->phase = angle_u(&b[6]);
          f->period_ms = le16(&b[8]);
          break;
        case FFB_RID_SET_CONSTANT:
          f->constant = mag_s(&b[2]);
          break;
        case FFB_RID_SET_RAMP:
          f->ramp_start = mag_s(&b[2]);
          f->ramp_end = mag_s(&b[4]);
          break;
        default:
          return false;
      }
      break;
  }
  st->accepted++;
  return true;
}

size_t ffb_pid_build_block_load(const ffb_fx_engine_t *e, uint8_t *out, size_t max)
{
  if (max < FFB_LEN_BLOCK_LOAD)
  {
    return 0U;
  }
  out[0] = FFB_RID_BLOCK_LOAD;
  out[1] = e->bl_index;
  out[2] = e->bl_status;
  put16(&out[3], ffb_fx_ram_available(e));
  return FFB_LEN_BLOCK_LOAD;
}

size_t ffb_pid_build_pool(uint8_t *out, size_t max)
{
  if (max < FFB_LEN_POOL)
  {
    return 0U;
  }
  out[0] = FFB_RID_POOL;
  put16(&out[1], (uint16_t)(FFB_MAX_EFFECTS * sizeof(ffb_fx_t)));
  out[3] = (uint8_t)FFB_MAX_EFFECTS;         /* simultaneous effects max     */
  out[4] = 0x01U;                            /* device managed pool = 1,
                                                shared parameter blocks = 0 */
  return FFB_LEN_POOL;
}

size_t ffb_pid_build_state(const ffb_fx_engine_t *e, bool actuator_power, uint8_t *out, size_t max)
{
  uint8_t playing = 0U;

  if (max < FFB_LEN_PID_STATE)
  {
    return 0U;
  }
  for (unsigned i = 0U; i < FFB_MAX_EFFECTS; i++)
  {
    if (e->fx[i].allocated && e->fx[i].playing)
    {
      playing = (uint8_t)(0x80U | (i + 1U));
      break;
    }
  }
  out[0] = FFB_RID_PID_STATE;
  out[1] = (uint8_t)((e->paused ? 0x01U : 0U) | (e->actuators_enabled ? 0x02U : 0U)
                     | 0x04U                                  /* safety switch: ok */
                     | (actuator_power ? 0x10U : 0U));
  out[2] = playing;
  return FFB_LEN_PID_STATE;
}

void ffb_pid_queue_init(ffb_pid_queue_t *q)
{
  memset(q, 0, sizeof(*q));
}

bool ffb_pid_queue_push(ffb_pid_queue_t *q, uint8_t type, const uint8_t *buf, size_t len)
{
  const uint8_t next = (uint8_t)((q->head + 1U) % FFB_PID_QUEUE_LEN);

  if ((buf == NULL) || (len == 0U) || (next == q->tail))
  {
    q->dropped++;
    return false;
  }
  if (len > FFB_LEN_MAX)
  {
    len = FFB_LEN_MAX;                       /* longer than any report: trimmed */
  }
  q->q[q->head].type = type;
  q->q[q->head].len = (uint8_t)len;
  memcpy(q->q[q->head].data, buf, len);
  q->head = next;                            /* publish after the copy */
  return true;
}

bool ffb_pid_queue_pop(ffb_pid_queue_t *q, ffb_pid_msg_t *out)
{
  if (q->tail == q->head)
  {
    return false;
  }
  *out = q->q[q->tail];
  q->tail = (uint8_t)((q->tail + 1U) % FFB_PID_QUEUE_LEN);
  return true;
}

/* ======================================================================= */
/* Global instance (firmware glue)                                          */
/* ======================================================================= */
static ffb_fx_engine_t  s_engine;
static ffb_pid_queue_t  s_queue;
static ffb_pid_stats_t  s_stats;
static uint8_t          s_bl_snapshot[FFB_LEN_BLOCK_LOAD];
static uint8_t          s_state_snapshot[FFB_LEN_PID_STATE];
static uint8_t          s_status_snapshot[FFB_LEN_STATUS];

void ffb_pid_init(void)
{
  ffb_fx_init(&s_engine);
  /* the profile's gains per kind of effect (ffb.gain_periodic, gain_constant, gain_condition, periodic_max_amps) */
  ffb_fx_set_gains(&s_engine, FFB_GAIN_PERIODIC, FFB_GAIN_CONSTANT, FFB_GAIN_CONDITION, FFB_PERIODIC_MAX_AMPS / FFB_MAX_AMPS);
  ffb_pid_queue_init(&s_queue);
  memset(&s_stats, 0, sizeof(s_stats));
  (void)ffb_pid_build_block_load(&s_engine, s_bl_snapshot, sizeof(s_bl_snapshot));
  (void)ffb_pid_build_state(&s_engine, false, s_state_snapshot, sizeof(s_state_snapshot));
  memset(s_status_snapshot, 0, sizeof(s_status_snapshot));
  s_status_snapshot[0] = FFB_RID_STATUS;
}

void ffb_pid_rx_isr(uint8_t type, const uint8_t *buf, size_t len)
{
  const bool create = (buf != NULL) && (len >= 1U) && (buf[0] == FFB_RID_CREATE_EFFECT);

  if (create)
  {
    s_bl_snapshot[2] = FFB_BL_PENDING;       /* host must not see the old result */
  }
  if (!ffb_pid_queue_push(&s_queue, type, buf, len) && create)
  {
    s_bl_snapshot[1] = 0U;
    s_bl_snapshot[2] = FFB_BL_ERROR;         /* queue full: fail, do not hang */
  }
}

size_t ffb_pid_get_report_isr(uint8_t type, uint8_t id, uint8_t *out, size_t max)
{
  const uint8_t *src = NULL;
  size_t n = 0U;

  if ((type == FFB_REPORT_FEATURE) && (id == FFB_RID_BLOCK_LOAD))
  {
    src = s_bl_snapshot; n = sizeof(s_bl_snapshot);
  }
  else if ((type == FFB_REPORT_FEATURE) && (id == FFB_RID_POOL))
  {
    return ffb_pid_build_pool(out, max);
  }
  else if ((type == FFB_REPORT_FEATURE) && (id == FFB_RID_STATUS))
  {
    src = s_status_snapshot; n = sizeof(s_status_snapshot);
  }
  else if ((type == FFB_REPORT_INPUT) && (id == FFB_RID_PID_STATE))
  {
    src = s_state_snapshot; n = sizeof(s_state_snapshot);
  }
  if ((src == NULL) || (max < n))
  {
    return 0U;
  }
  memcpy(out, src, n);
  return n;
}

void ffb_pid_poll(uint32_t now_ms, bool actuator_power, const uint8_t *status16)
{
  ffb_pid_msg_t m;
  bool created = false;

  while (ffb_pid_queue_pop(&s_queue, &m))
  {
    (void)ffb_pid_dispatch(&s_engine, &s_stats, m.type, m.data, m.len, now_ms);
    created = created || (m.data[0] == FFB_RID_CREATE_EFFECT);
  }
  if (created)
  {
    /* The host waits for this answer before the next Create (Linux polls
     * GET_REPORT up to 60 times), so only one Create is ever in flight.
     * The status byte is written last: an interrupted update still reads
     * as "pending". */
    uint8_t tmp[FFB_LEN_BLOCK_LOAD];
    (void)ffb_pid_build_block_load(&s_engine, tmp, sizeof(tmp));
    s_bl_snapshot[0] = tmp[0];
    s_bl_snapshot[1] = tmp[1];
    s_bl_snapshot[3] = tmp[3];
    s_bl_snapshot[4] = tmp[4];
    s_bl_snapshot[2] = tmp[2];
  }
  (void)ffb_pid_build_state(&s_engine, actuator_power, s_state_snapshot, sizeof(s_state_snapshot));
  if (status16 != NULL)
  {
    memcpy(s_status_snapshot, status16, sizeof(s_status_snapshot));
  }
}

float ffb_pid_force(uint32_t now_ms, float pos, float vel, bool *active)
{
  return ffb_fx_force(&s_engine, now_ms, pos, vel, active);
}

bool ffb_pid_actuators_enabled(void)
{
  return s_engine.actuators_enabled;
}

const ffb_pid_stats_t *ffb_pid_stats(void)
{
  return &s_stats;
}
