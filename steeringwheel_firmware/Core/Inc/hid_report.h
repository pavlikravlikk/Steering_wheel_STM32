/**
 * @file    hid_report.h
 * @brief   Collects all inputs every 1 ms and sends the USB HID report.
 *
 * Report layout (no Report ID, 8 bytes, little-endian, see JoystickReport_t):
 *   bytes 0-1  X  steering   0..65535
 *   bytes 2-3  Y  throttle   0..65535 (reserved, always 0 for now)
 *   bytes 4-5  Z  brake      0..65535 (reserved, always 0 for now)
 *   bytes 6-7  buttons       bit 0 = button 1 ... bit 15 = button 16
 */
#ifndef HID_REPORT_H
#define HID_REPORT_H

#include <stdint.h>

/** Initialise the selected inputs and start the 1 kHz tick (TIM6). */
void hid_report_init(void);

/** Call continuously from the main loop. Does work once per 1 ms tick:
 *  update inputs, build the report and send it if the previous USB transfer
 *  has completed. Never blocks. */
void hid_report_poll(void);

#endif /* HID_REPORT_H */
