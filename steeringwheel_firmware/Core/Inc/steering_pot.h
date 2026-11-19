/**
 * @file    steering_pot.h
 * @brief   Potentiometer steering fallback (PA1 / ADC1_IN2), non-blocking.
 */
#ifndef STEERING_POT_H
#define STEERING_POT_H

#include <stdint.h>
#include "main.h"

/** Calibrate ADC1 and start the first conversion. */
void steering_pot_init(ADC_HandleTypeDef *hadc);

/** Collect the finished conversion, filter it and start the next one.
 *  Never waits for the ADC. Call every 1 ms. */
void steering_pot_update(void);

/** Filtered steering axis scaled from 12 to 16 bit (0..65535). */
uint16_t steering_pot_get_axis16(void);

#endif /* STEERING_POT_H */
