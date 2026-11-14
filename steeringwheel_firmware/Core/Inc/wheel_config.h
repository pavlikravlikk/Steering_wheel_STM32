/**
 * @file    wheel_config.h
 * @brief   Central, user-tunable configuration of the steering wheel firmware.
 *
 * Every macro can be overridden from the command line, e.g.
 *   cmake --preset Debug -DSIMULATE_INPUTS=ON -DSTEERING_SOURCE=POT
 */
#ifndef WHEEL_CONFIG_H
#define WHEEL_CONFIG_H

/* ------------------------------------------------------------------------- */
/* Input source selection                                                    */
/* ------------------------------------------------------------------------- */
#define STEERING_SOURCE_POT      1   /* potentiometer on PA1 (ADC1_IN2)      */
#define STEERING_SOURCE_ENCODER  2   /* LPD3806 quadrature encoder on TIM3   */

#ifndef STEERING_SOURCE
#define STEERING_SOURCE          STEERING_SOURCE_ENCODER
#endif

/* 1 = generate a sine wave on X and toggle buttons one by one, touch no
 * external hardware (nothing needs to be wired to verify the HID path). */
#ifndef SIMULATE_INPUTS
#define SIMULATE_INPUTS          0
#endif

/* ------------------------------------------------------------------------- */
/* Steering encoder (LPD3806-600BM)                                          */
/* ------------------------------------------------------------------------- */
/* 600 pulses/rev * 4 (x4 quadrature) = 2400 counts per encoder revolution.
 * Belt ratio between wheel and encoder is assumed 1:1 for now. */
#ifndef ENCODER_COUNTS_PER_WHEEL_REV
#define ENCODER_COUNTS_PER_WHEEL_REV  2400
#endif

/* Total wheel rotation from full left lock to full right lock, in degrees. */
#ifndef WHEEL_LOCK_TO_LOCK_DEG
#define WHEEL_LOCK_TO_LOCK_DEG        540
#endif

/* 1 = swap steering direction. */
#ifndef ENCODER_INVERT_DIRECTION
#define ENCODER_INVERT_DIRECTION      0
#endif

/* Encoder counts that span the whole lock-to-lock range (3600 by default).
 * The centre is the power-up position, so each side gets half of this. */
#define ENCODER_COUNTS_FULL_RANGE \
  ((ENCODER_COUNTS_PER_WHEEL_REV * WHEEL_LOCK_TO_LOCK_DEG) / 360)

/* TIM3 input capture filter (ICxF, 0..15). 8 = fDTS/8, N=6 -> about 1 us at
 * 48 MHz: rejects ringing/glitches, still far above the 600 PPR pulse rate. */
#define ENCODER_INPUT_FILTER          8U

/* ------------------------------------------------------------------------- */
/* MCP23017 button expander (I2C1, PB6 = SCL, PB7 = SDA)                      */
/* ------------------------------------------------------------------------- */
#define MCP23017_I2C_ADDR_7BIT        0x20U  /* A0..A2 tied to GND            */
#define BUTTON_DEBOUNCE_MS            5U     /* samples are taken every 1 ms  */
#define BUTTON_COUNT                  10U    /* bits 0..9 of the report       */
#define MCP23017_RETRY_MS             250U   /* retry period when absent      */
#define MCP23017_XFER_TIMEOUT_MS      10U    /* abort a stuck transfer        */

/* I2C1 TIMINGR for 400 kHz with I2C1CLK = SYSCLK = 48 MHz (see README). */
#define I2C1_TIMING_400KHZ_48MHZ      0x10930F20UL

/* ------------------------------------------------------------------------- */
/* Potentiometer fallback (PA1)                                              */
/* ------------------------------------------------------------------------- */
#define POT_DEADBAND_16BIT            64U    /* ~4 ADC counts, as before      */

#endif /* WHEEL_CONFIG_H */
