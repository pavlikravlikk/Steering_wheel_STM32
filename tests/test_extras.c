/* Step 3 extras: persistent settings over a mock flash, reset-reason decoder,
 * IWDG reload, steering shaping. */
#include "test_runner.h"

#include <string.h>

#include "axis_shape.h"
#include "board_health.h"
#include "settings.h"
#include "wheel_config.h"

/* ---------------- mock flash ---------------- */
typedef struct { uint8_t page[64]; bool fail_erase, fail_write, corrupt_write; int erases; } mflash_t;

static bool m_read(void *c, uint8_t *b, size_t n) { memcpy(b, ((mflash_t *)c)->page, n); return true; }
static bool m_erase(void *c)
{
  mflash_t *m = c;
  if (m->fail_erase) return false;
  memset(m->page, 0xFF, sizeof(m->page));
  m->erases++;
  return true;
}
static bool m_write(void *c, const uint8_t *b, size_t n)
{
  mflash_t *m = c;
  if (m->fail_write) return false;
  for (size_t i = 0; i < n; i++) m->page[i] &= b[i];       /* flash can only clear bits */
  if (m->corrupt_write) m->page[3] ^= 0x10;
  return true;
}

TEST(test_settings_roundtrip_and_fallbacks)
{
  mflash_t m;
  settings_flash_t f = { m_read, m_erase, m_write, &m };
  wheel_settings_t s, d, got;
  memset(&m, 0xFF, sizeof(m.page));
  m.fail_erase = m.fail_write = m.corrupt_write = false;
  settings_defaults(&d);
  CHECK(settings_valid(&d));
  CHECK_EQ_INT(d.lock_to_lock_deg, WHEEL_LOCK_TO_LOCK_DEG);
  /* empty flash -> defaults */
  CHECK_EQ_INT(settings_load(&f, &got), SETTINGS_EMPTY);
  CHECK(memcmp(&got, &d, sizeof(d)) == 0);
  /* save + load */
  s = d;
  s.lock_to_lock_deg = 900;
  s.invert = 1;
  s.centre_offset_turns = -0.125f;
  s.force_gain = 0.5f;
  CHECK_EQ_INT(settings_save(&f, &s), SETTINGS_OK);
  CHECK_EQ_INT(settings_load(&f, &got), SETTINGS_OK);
  CHECK_EQ_INT(got.lock_to_lock_deg, 900);
  CHECK_EQ_INT(got.invert, 1);
  CHECK_NEAR(got.centre_offset_turns, -0.125, 0);
  /* every single-bit corruption of the blob is detected -> defaults */
  for (unsigned byte = 0; byte < SETTINGS_BLOB_LEN; byte++)
  {
    for (unsigned bit = 0; bit < 8; bit++)
    {
      mflash_t c = m;
      settings_flash_t fc = { m_read, m_erase, m_write, &c };
      c.page[byte] ^= (uint8_t)(1U << bit);
      CHECK(settings_load(&fc, &got) != SETTINGS_OK);
      CHECK(memcmp(&got, &d, sizeof(d)) == 0);
    }
  }
  /* wrong version */
  {
    uint8_t blob[SETTINGS_BLOB_LEN];
    settings_encode(&s, blob);
    blob[4] = 2;
    CHECK_EQ_INT(settings_decode(blob, &got), SETTINGS_BAD_VERSION);
  }
  /* invalid values are refused on save and on load */
  s.spring_kp = FFB_SPRING_KP * 2.0f;
  CHECK_EQ_INT(settings_save(&f, &s), SETTINGS_BAD_VALUE);
  s = d;
  s.lock_to_lock_deg = 10;
  CHECK_EQ_INT(settings_save(&f, &s), SETTINGS_BAD_VALUE);
  s = d;
  s.force_gain = NAN;
  CHECK(!settings_valid(&s));
  /* I/O errors and verify-after-write */
  s = d;
  m.fail_erase = true;
  CHECK_EQ_INT(settings_save(&f, &s), SETTINGS_IO_ERROR);
  m.fail_erase = false;
  m.corrupt_write = true;
  CHECK_EQ_INT(settings_save(&f, &s), SETTINGS_IO_ERROR);
  CHECK(settings_load(&f, &got) != SETTINGS_OK);
  CHECK(memcmp(&got, &d, sizeof(d)) == 0);
  CHECK_EQ_INT(settings_load(NULL, &got), SETTINGS_IO_ERROR);
}

TEST(test_settings_crc32_known_value)
{
  CHECK_EQ_INT(settings_crc32((const uint8_t *)"123456789", 9), 0xCBF43926UL);   /* standard check value */
}

TEST(test_reset_reason)
{
  CHECK_EQ_INT(reset_reason_from_csr((1UL << 27) | (1UL << 26)), RESET_POWER_ON);
  CHECK_EQ_INT(reset_reason_from_csr(1UL << 26), RESET_PIN);
  CHECK_EQ_INT(reset_reason_from_csr((1UL << 29) | (1UL << 26)), RESET_IWDG);
  CHECK_EQ_INT(reset_reason_from_csr(1UL << 28), RESET_SOFTWARE);
  CHECK_EQ_INT(reset_reason_from_csr(1UL << 30), RESET_WWDG);
  CHECK_EQ_INT(reset_reason_from_csr(1UL << 31), RESET_LOW_POWER);
  CHECK_EQ_INT(reset_reason_from_csr(1UL << 25), RESET_OPTION_BYTES);
  CHECK_EQ_INT(reset_reason_from_csr(0), RESET_UNKNOWN);
  CHECK_EQ_INT(IWDG_ENABLE, 0);            /* default OFF */
  CHECK_EQ_INT(RESET_REASON_LED, 0);
}

TEST(test_iwdg_reload)
{
  CHECK_EQ_INT(iwdg_reload_for_ms(500), 625);
  CHECK_EQ_INT(iwdg_reload_for_ms(0), 1);
  CHECK_EQ_INT(iwdg_reload_for_ms(100000), 0xFFF);
}

TEST(test_axis_shape)
{
  CHECK_EQ_INT(AXIS_SHAPING_ENABLE, 0);    /* default OFF */
  /* defaults = exact identity for every value */
  for (uint32_t x = 0; x <= 65535; x += 7) CHECK_EQ_INT(axis_shape((uint16_t)x, 0.0f, 0.0f), x);
  CHECK_EQ_INT(axis_shape(65535, 0.0f, 0.0f), 65535);
  /* deadzone */
  CHECK_EQ_INT(axis_shape(32768 + 1000, 0.05f, 0.0f), 32768);
  CHECK_EQ_INT(axis_shape(32768 - 1000, 0.05f, 0.0f), 32768);
  CHECK_EQ_INT(axis_shape(65535, 0.05f, 0.0f), 65535);   /* full range kept */
  CHECK_EQ_INT(axis_shape(0, 0.05f, 0.0f), 0);
  /* curve: monotonic, ends fixed, softer near centre */
  {
    uint16_t prev = 0;
    for (uint32_t x = 0; x <= 65535; x += 97)
    {
      uint16_t y = axis_shape((uint16_t)x, 0.02f, 0.6f);
      CHECK(y >= prev);
      prev = y;
    }
    CHECK(axis_shape(32768 + 8192, 0.0f, 1.0f) < 32768 + 8192);
    CHECK_EQ_INT(axis_shape(65535, 0.0f, 1.0f), 65535);
  }
  /* invalid parameters -> identity */
  CHECK_EQ_INT(axis_shape(40000, NAN, NAN), 40000);
  CHECK_EQ_INT(axis_shape(40000, 0.9f, 3.0f), 40000);
}

void run_extras_tests(void)
{
  printf("step 3 extras (settings, reset reason, IWDG, axis shaping)\n");
  RUN(test_settings_roundtrip_and_fallbacks);
  RUN(test_settings_crc32_known_value);
  RUN(test_reset_reason);
  RUN(test_iwdg_reload);
  RUN(test_axis_shape);
}
