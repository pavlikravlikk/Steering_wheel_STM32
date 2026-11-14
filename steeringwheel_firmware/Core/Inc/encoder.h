/**
 * @file    encoder.h
 * @brief   Steering angle from a quadrature encoder on TIM3 (PC6 = A, PC7 = B).
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>

/** Configure PC6/PC7 (AF2, internal pull-ups) and TIM3 in x4 encoder mode.
 *  The position at the time of the call becomes the wheel centre (zero). */
void encoder_init(void);

/** Read TIM3->CNT and accumulate the signed 16-bit delta. Call every 1 ms
 *  (must be called more often than half a TIM3 revolution of 65536 counts). */
void encoder_update(void);

/** Signed position relative to power-up, in encoder counts (direction
 *  inversion already applied). */
int32_t encoder_get_counts(void);

/** Steering axis for the HID report: 32768 at centre, clamped to 0..65535. */
uint16_t encoder_get_axis16(void);

/** Stub for future homing against a limit switch: should find the physical
 *  centre/limit and call encoder_set_center(). Currently does nothing. */
void encoder_home_to_limit_switch(void);

/** Make the current position the new centre (zero counts). */
void encoder_set_center(void);

#endif /* ENCODER_H */
