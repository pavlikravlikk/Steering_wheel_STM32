/**
 * @file    buttons_mcp23017.h
 * @brief   Non-blocking button reader for an MCP23017 on I2C1 (interrupt driven).
 *
 * Button map (bit in the HID button field, 1 = pressed):
 *   GPA0..GPA7 -> bits 0..7  (buttons 1..8)
 *   GPB0       -> bit 8      (button 9,  left paddle)
 *   GPB1       -> bit 9      (button 10, right paddle)
 */
#ifndef BUTTONS_MCP23017_H
#define BUTTONS_MCP23017_H

#include <stdbool.h>
#include <stdint.h>
#include "main.h"

/** Reconfigure I2C1 for 400 kHz and start the (asynchronous) chip setup. */
void buttons_init(I2C_HandleTypeDef *hi2c);

/** Advance the state machine and debounce. Call every 1 ms from the main
 *  context; it only starts interrupt-driven transfers and never waits. */
void buttons_update(uint32_t now_ms);

/** Debounced button mask, bits 0..9, 1 = pressed. 0 while the chip is absent. */
uint16_t buttons_get(void);

/** true once the expander answered and is configured. */
bool buttons_is_present(void);

#endif /* BUTTONS_MCP23017_H */
