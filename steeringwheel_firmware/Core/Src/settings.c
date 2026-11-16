/**
 * @file    settings.c
 * @brief   Persistent settings: encoding, CRC, load with fallback. Pure.
 *
 * Blob layout (little endian, 32 bytes):
 *   0 magic u32 | 4 version u16 | 6 payload length u16 |
 *   8 lock_to_lock u16 | 10 invert u8 | 11 preset u8 | 12 centre f32 |
 *  16 force_gain f32 | 20 spring_kp f32 | 24 reserved (0) u32 | 28 crc32 u32
 *   (CRC over bytes 0..27)
 */
#include "settings.h"

#include <string.h>

#include "wheel_config.h"

#define OFF_CRC 28U

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void putf(uint8_t *p, float f) { uint32_t v; memcpy(&v, &f, 4); put32(p, v); }
static float getf(const uint8_t *p) { uint32_t v = get32(p); float f; memcpy(&f, &v, 4); return f; }

static bool finite_f(float v)
{
  return (v == v) && (v < 3.0e38f) && (v > -3.0e38f);
}

void settings_defaults(wheel_settings_t *s)
{
  memset(s, 0, sizeof(*s));
  s->lock_to_lock_deg = (uint16_t)WHEEL_LOCK_TO_LOCK_DEG;
  s->invert = (ODRIVE_INVERT_DIRECTION != 0) ? 1U : 0U;
  s->angle_preset = 0U;
  s->centre_offset_turns = 0.0f;
  s->force_gain = 1.0f;
  s->spring_kp = FFB_SPRING_KP;
}

bool settings_valid(const wheel_settings_t *s)
{
  return (s->lock_to_lock_deg >= 90U) && (s->lock_to_lock_deg <= 3600U) && (s->invert <= 1U)
      && finite_f(s->centre_offset_turns) && (s->centre_offset_turns >= -2.0f)
      && (s->centre_offset_turns <= 2.0f) && finite_f(s->force_gain) && (s->force_gain >= 0.0f)
      && (s->force_gain <= 1.0f) && finite_f(s->spring_kp) && (s->spring_kp >= 0.0f)
      && (s->spring_kp <= FFB_SPRING_KP);      /* never above the profile */
}

uint32_t settings_crc32(const uint8_t *data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;              /* CRC-32 (IEEE 802.3), bitwise */
  for (size_t i = 0U; i < len; i++)
  {
    crc ^= data[i];
    for (unsigned k = 0U; k < 8U; k++)
    {
      crc = (crc & 1U) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return ~crc;
}

void settings_encode(const wheel_settings_t *s, uint8_t out[SETTINGS_BLOB_LEN])
{
  memset(out, 0, SETTINGS_BLOB_LEN);
  put32(&out[0], SETTINGS_MAGIC);
  put16(&out[4], SETTINGS_VERSION);
  put16(&out[6], (uint16_t)(OFF_CRC - 8U));
  put16(&out[8], s->lock_to_lock_deg);
  out[10] = s->invert;
  out[11] = s->angle_preset;
  putf(&out[12], s->centre_offset_turns);
  putf(&out[16], s->force_gain);
  putf(&out[20], s->spring_kp);
  put32(&out[OFF_CRC], settings_crc32(out, OFF_CRC));
}

settings_status_t settings_decode(const uint8_t in[SETTINGS_BLOB_LEN], wheel_settings_t *s)
{
  bool erased = true;
  wheel_settings_t t;

  for (unsigned i = 0U; i < SETTINGS_BLOB_LEN; i++)
  {
    erased = erased && (in[i] == 0xFFU);
  }
  if (erased)                                  return SETTINGS_EMPTY;
  if (get32(&in[0]) != SETTINGS_MAGIC)         return SETTINGS_BAD_MAGIC;
  if (get16(&in[4]) != SETTINGS_VERSION)       return SETTINGS_BAD_VERSION;
  if (get16(&in[6]) != (uint16_t)(OFF_CRC - 8U)) return SETTINGS_BAD_VERSION;
  if (get32(&in[OFF_CRC]) != settings_crc32(in, OFF_CRC)) return SETTINGS_BAD_CRC;
  t.lock_to_lock_deg = get16(&in[8]);
  t.invert = in[10];
  t.angle_preset = in[11];
  t.centre_offset_turns = getf(&in[12]);
  t.force_gain = getf(&in[16]);
  t.spring_kp = getf(&in[20]);
  if (!settings_valid(&t))                     return SETTINGS_BAD_VALUE;
  *s = t;
  return SETTINGS_OK;
}

settings_status_t settings_load(const settings_flash_t *f, wheel_settings_t *s)
{
  uint8_t blob[SETTINGS_BLOB_LEN];
  settings_status_t st;

  settings_defaults(s);
  if ((f == NULL) || (f->read == NULL) || !f->read(f->ctx, blob, sizeof(blob)))
  {
    return SETTINGS_IO_ERROR;
  }
  st = settings_decode(blob, s);
  if (st != SETTINGS_OK)
  {
    settings_defaults(s);                      /* safe fallback */
  }
  return st;
}

settings_status_t settings_save(const settings_flash_t *f, const wheel_settings_t *s)
{
  uint8_t blob[SETTINGS_BLOB_LEN];
  uint8_t back[SETTINGS_BLOB_LEN];

  if (!settings_valid(s))
  {
    return SETTINGS_BAD_VALUE;
  }
  settings_encode(s, blob);
  if ((f == NULL) || !f->erase(f->ctx) || !f->write(f->ctx, blob, sizeof(blob))
      || !f->read(f->ctx, back, sizeof(back)) || (memcmp(blob, back, sizeof(blob)) != 0))
  {
    return SETTINGS_IO_ERROR;
  }
  return SETTINGS_OK;
}
