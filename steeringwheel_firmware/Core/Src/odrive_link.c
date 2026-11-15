/**
 * @file    odrive_link.c
 * @brief   ODrive position -> HID X. READ-ONLY. Must not include any HAL header.
 */
#include "odrive_link.h"

#include "odrive_uart.h"
#include "steering_map.h"
#include "wheel_config.h"

static odr_t          s_odr;
static steering_map_t s_map;
static uint16_t       s_axis = 32768U;

void odrive_link_init(void)
{
  odr_init(&s_odr, ODRIVE_AXIS, ODRIVE_REPLY_TIMEOUT_MS, ODRIVE_STALE_MS);
  steering_map_init(&s_map, (float)WHEEL_LOCK_TO_LOCK_DEG,
                    ODRIVE_TURNS_PER_WHEEL_REV, ODRIVE_INVERT_DIRECTION != 0);
  s_axis = 32768U;
  odrive_uart_init();
}

void odrive_link_process(uint32_t now_ms)
{
  uint8_t chunk[32];
  size_t n;
  const uint32_t ok_before = s_odr.stats.replies_ok;

  while ((n = odrive_uart_read(chunk, sizeof(chunk))) > 0U)
  {
    odr_feed(&s_odr, chunk, n, now_ms);
  }

  if (s_odr.stats.replies_ok != ok_before)
  {
    if (!s_map.centred)
    {
      (void)steering_map_set_centre(&s_map, s_odr.pos_turns);  /* power-up centre */
    }
    s_axis = steering_map_axis16(&s_map, s_odr.pos_turns);
  }
}

void odrive_link_request(uint32_t now_ms)
{
  if (odr_may_send(&s_odr, now_ms))
  {
    if (odrive_uart_send_readonly(odr_request(&s_odr)))
    {
      odr_note_sent(&s_odr, now_ms);
    }
  }
}

bool odrive_link_ok(uint32_t now_ms)
{
  return !odr_is_stale(&s_odr, now_ms);
}

uint16_t odrive_link_axis16(void)
{
  return s_axis;
}

bool odrive_link_position(uint32_t now_ms, float *pos_turns, float *vel_turns_s)
{
  *pos_turns = s_odr.pos_turns;
  *vel_turns_s = s_odr.vel_turns_s;
  return !odr_is_stale(&s_odr, now_ms);
}

bool odrive_link_set_centre(uint32_t now_ms)
{
  if (odr_is_stale(&s_odr, now_ms))
  {
    return false;
  }
  if (!steering_map_set_centre(&s_map, s_odr.pos_turns))
  {
    return false;
  }
  s_axis = steering_map_axis16(&s_map, s_odr.pos_turns);
  return true;
}

const odr_stats_t *odrive_link_stats(void)
{
  return &s_odr.stats;
}
