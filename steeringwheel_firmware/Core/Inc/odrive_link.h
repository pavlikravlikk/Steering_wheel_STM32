/**
 * @file    odrive_link.h
 * @brief   Steering position from the ODrive (READ-ONLY). No HAL dependency:
 *          talks to the hardware only through odrive_uart.h, so it can be
 *          unit-tested with a mock UART.
 */
#ifndef ODRIVE_LINK_H
#define ODRIVE_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "odrive_ascii.h"
#include "wheel_config.h"

/** Init UART, protocol client and mapping (config from wheel_config.h). */
void odrive_link_init(void);

/** Drain received bytes and parse them. Call on every main-loop pass.
 *  The first valid position after start-up becomes the centre. */
void odrive_link_process(uint32_t now_ms);

/** Send the next position request if allowed. Call at the poll rate. */
void odrive_link_request(uint32_t now_ms);

/** true when a valid position arrived within ODRIVE_STALE_MS. */
bool odrive_link_ok(uint32_t now_ms);

/** HID X. Holds the last value while stale; 32768 before the first sample. */
uint16_t odrive_link_axis16(void);

/** Last position / velocity (turns, turns/s) and whether they are valid. */
bool odrive_link_position(uint32_t now_ms, float *pos_turns, float *vel_turns_s);

/** Make the current position the centre. Refused while data is stale. */
bool odrive_link_set_centre(uint32_t now_ms);

const odr_stats_t *odrive_link_stats(void);

#if WHEEL_FFB
/** FFB precheck only: queue a one-shot read-only "r <path>" (sent instead
 *  of the next "f" request) and collect its result. */
bool odrive_link_queue_read(const char *path);
odr_read_state_t odrive_link_read_poll(float *value);

/** Wheel angle from the centre and wheel velocity, in wheel turns and turns/s
 *  in the MOTOR frame (no HID inversion), plus the age of the sample.
 *  false before the first sample/centre. */
bool odrive_link_wheel(uint32_t now_ms, float *angle_turns, float *vel_turns_s, uint32_t *age_ms);
#endif

#endif /* ODRIVE_LINK_H */
