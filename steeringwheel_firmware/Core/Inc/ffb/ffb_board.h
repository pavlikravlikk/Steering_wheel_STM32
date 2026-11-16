/**
 * @file    ffb_board.h
 * @brief   Hardware glue of the FFB module (firmware only, -DWHEEL_FFB=ON).
 */
#ifndef FFB_BOARD_H
#define FFB_BOARD_H

#include <stdbool.h>
#include <stdint.h>

/** After hid_report_init() (UART/ODrive link already initialised). */
void ffb_board_init(void);

/** Every main-loop pass: runs the FFB control cycle when due, drives LEDs. */
void ffb_board_poll(uint32_t now_ms);

/** Every 1 ms tick: debounced B1 + HID button bits (arm / disarm gesture). */
void ffb_board_buttons(uint32_t now_ms, bool b1, uint16_t buttons);

/** B1 "set centre" is allowed (DISARMED, no paddle, not right after disarm). */
bool ffb_board_centre_allowed(uint32_t now_ms);

/** Vendor status feature report 0x40 (16 bytes incl. Report ID). */
void ffb_board_status_report(uint8_t *out16);

/** HID PID device control: enable / disable actuators. */
void ffb_board_set_actuators(bool enabled);

/** FFB build USB: joystick input report (Report ID 0x01 + 8 bytes, static buffer). */
bool ffb_board_usb_ready(void);
bool ffb_board_send_joystick(uint8_t *report9);

#endif /* FFB_BOARD_H */
