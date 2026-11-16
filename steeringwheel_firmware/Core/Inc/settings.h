/**
 * @file    settings.h
 * @brief   Persistent user settings (pure, host-tested) over a mockable flash
 *          interface. Stored in the LAST flash page with a magic, a version,
 *          the length and a CRC-32; anything wrong -> profile defaults.
 *
 * DEFAULT OFF: SETTINGS_FLASH_ENABLE = 0. The STM32 glue
 * (settings_flash_stm32.c) is compiled empty unless enabled. Enable only
 * after the bench procedure in docs/DECISIONS.md "Step 3".
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SETTINGS_MAGIC    0x57484C53UL   /* "SLHW" */
#define SETTINGS_VERSION  1U
#define SETTINGS_BLOB_LEN 32U            /* bytes written to flash */

typedef struct
{
  uint16_t lock_to_lock_deg;   /* 90..3600                                  */
  uint8_t  invert;             /* 0/1                                       */
  uint8_t  angle_preset;       /* index into a preset table (future)        */
  float    centre_offset_turns;/* added to the power-up centre              */
  float    force_gain;           /* 0..1 scale of the FFB strength            */
  float    spring_kp;          /* A/turn, <= profile value                  */
} wheel_settings_t;

typedef enum
{
  SETTINGS_OK = 0,
  SETTINGS_EMPTY,              /* erased flash (all 0xFF)                   */
  SETTINGS_BAD_MAGIC,
  SETTINGS_BAD_VERSION,
  SETTINGS_BAD_CRC,
  SETTINGS_BAD_VALUE,          /* CRC ok but a value out of range           */
  SETTINGS_IO_ERROR
} settings_status_t;

typedef struct
{
  bool (*read)(void *ctx, uint8_t *buf, size_t len);
  bool (*erase)(void *ctx);
  bool (*write)(void *ctx, const uint8_t *buf, size_t len);  /* len even */
  void *ctx;
} settings_flash_t;

void settings_defaults(wheel_settings_t *s);
bool settings_valid(const wheel_settings_t *s);
uint32_t settings_crc32(const uint8_t *data, size_t len);

void settings_encode(const wheel_settings_t *s, uint8_t out[SETTINGS_BLOB_LEN]);
settings_status_t settings_decode(const uint8_t in[SETTINGS_BLOB_LEN], wheel_settings_t *s);

/** Load; on ANY problem *s = defaults and the reason is returned. */
settings_status_t settings_load(const settings_flash_t *f, wheel_settings_t *s);

/** Erase + write + read back and compare. Refuses invalid settings. */
settings_status_t settings_save(const settings_flash_t *f, const wheel_settings_t *s);

#endif /* SETTINGS_H */
