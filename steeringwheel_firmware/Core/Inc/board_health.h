/**
 * @file    board_health.h
 * @brief   Reset reason (pure decoder, host-tested) + optional independent
 *          watchdog (IWDG) of the STM32 and a reset-reason LED code.
 *
 * Both are OFF by default (IWDG_ENABLE = 0, RESET_REASON_LED = 0): main.c only
 * calls the glue when one of them is enabled.
 *   IWDG: started at boot, refreshed every main-loop pass; a hung loop
 *         resets the STM32 after IWDG_TIMEOUT_MS. With FFB this stops the
 *         UART, so the ODrive watchdog then stops the motor as well.
 *         The IWDG cannot be stopped once started; it is frozen while the
 *         debugger halts the core (DBGMCU).
 *   LED:  LD4 (blue) flashes the reason N times during the first seconds
 *         (default builds only; FFB builds use LD4 for FFB status).
 */
#ifndef BOARD_HEALTH_H
#define BOARD_HEALTH_H

#include <stdint.h>

#ifndef IWDG_ENABLE
#define IWDG_ENABLE          0
#endif
#ifndef IWDG_TIMEOUT_MS
#define IWDG_TIMEOUT_MS      500U    /* main loop must come round within this */
#endif
#ifndef RESET_REASON_LED
#define RESET_REASON_LED     0
#endif

typedef enum
{
  RESET_POWER_ON = 0,   /* normal power-up: no LED code              */
  RESET_PIN = 1,        /* NRST button / debugger                    */
  RESET_SOFTWARE = 2,
  RESET_IWDG = 3,       /* the main loop hung                        */
  RESET_WWDG = 4,
  RESET_LOW_POWER = 5,
  RESET_OPTION_BYTES = 6,
  RESET_UNKNOWN = 7
} reset_reason_t;

/** Decode RCC->CSR (STM32F3 bit layout: LPWR 31, WWDG 30, IWDG 29,
 *  SFT 28, POR 27, PIN 26, OBL 25). Most specific cause wins. */
reset_reason_t reset_reason_from_csr(uint32_t csr);

/** IWDG reload for prescaler 32 (LSI ~40 kHz -> 1.25 kHz), clamped to 12 bit. */
uint32_t iwdg_reload_for_ms(uint32_t ms);

/* ---- firmware glue (board_health.c, HAL) ---- */
void board_health_init(void);              /* read + clear CSR, start IWDG  */
void board_health_poll(uint32_t now_ms);   /* refresh IWDG, LED code        */
reset_reason_t board_health_reset_reason(void);

#endif /* BOARD_HEALTH_H */
