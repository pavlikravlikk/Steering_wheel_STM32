/**
 * @file    hid_pack.h
 * @brief   Pure HID report packing (no HAL; unit-tested on the host).
 *
 * Report layout (no Report ID, 8 bytes, little-endian), must match the
 * descriptor in usbd_custom_hid_if.c:
 *   bytes 0-1  X  steering   0..65535
 *   bytes 2-3  Y  throttle   0..65535
 *   bytes 4-5  Z  brake      0..65535
 *   bytes 6-7  buttons       bit 0 = button 1 ... bit 15 = button 16
 */
#ifndef HID_PACK_H
#define HID_PACK_H

#include <stdint.h>

#define HID_REPORT_SIZE   8U
#define HID_AXIS_MAX      65535
#define HID_AXIS_CENTRE   32768U

/** Clamp a signed 32-bit value into the 0..65535 axis range. */
uint16_t hid_clamp_axis(int32_t v);

/** Write the 8-byte report. Buttons are masked to the lowest
 *  @p button_count bits (0..16). */
void hid_pack_report(uint8_t out[HID_REPORT_SIZE], int32_t x, int32_t y,
                     int32_t z, uint32_t buttons, uint32_t button_count);

#endif /* HID_PACK_H */
