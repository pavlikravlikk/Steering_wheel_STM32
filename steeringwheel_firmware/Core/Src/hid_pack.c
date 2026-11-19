/**
 * @file    hid_pack.c
 * @brief   Pure HID report packing. Must not include any HAL header.
 */
#include "hid_pack.h"

uint16_t hid_clamp_axis(int32_t v)
{
  if (v < 0)
  {
    return 0U;
  }
  if (v > HID_AXIS_MAX)
  {
    return (uint16_t)HID_AXIS_MAX;
  }
  return (uint16_t)v;
}

static void put_le16(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)(v >> 8);
}

void hid_pack_report(uint8_t out[HID_REPORT_SIZE], int32_t x, int32_t y,
                     int32_t z, uint32_t buttons, uint32_t button_count)
{
  uint32_t mask;

  if (button_count >= 16U)
  {
    mask = 0xFFFFU;
  }
  else
  {
    mask = (1UL << button_count) - 1UL;
  }

  put_le16(&out[0], hid_clamp_axis(x));
  put_le16(&out[2], hid_clamp_axis(y));
  put_le16(&out[4], hid_clamp_axis(z));
  put_le16(&out[6], (uint16_t)(buttons & mask));
}
