/**
 * @file    board_health.c
 * @brief   Reset reason decoder (pure part) and IWDG / LED glue (HAL part,
 *          excluded from host builds with BOARD_HEALTH_HOST).
 */
#include "board_health.h"

reset_reason_t reset_reason_from_csr(uint32_t csr)
{
  if (csr & (1UL << 29)) return RESET_IWDG;
  if (csr & (1UL << 30)) return RESET_WWDG;
  if (csr & (1UL << 31)) return RESET_LOW_POWER;
  if (csr & (1UL << 28)) return RESET_SOFTWARE;
  if (csr & (1UL << 27)) return RESET_POWER_ON;     /* POR also sets PINRSTF */
  if (csr & (1UL << 25)) return RESET_OPTION_BYTES;
  if (csr & (1UL << 26)) return RESET_PIN;
  return RESET_UNKNOWN;
}

uint32_t iwdg_reload_for_ms(uint32_t ms)
{
  uint32_t r = (ms * 40U) / 32U;                    /* 40 kHz LSI / 32 */
  if (r < 1U) r = 1U;
  if (r > 0xFFFU) r = 0xFFFU;                       /* 3.27 s maximum */
  return r;
}

#ifndef BOARD_HEALTH_HOST
#include "main.h"
#include "wheel_config.h"

static reset_reason_t s_reason = RESET_UNKNOWN;

void board_health_init(void)
{
  s_reason = reset_reason_from_csr(RCC->CSR);
  RCC->CSR |= RCC_CSR_RMVF;                         /* clear for the next boot */
#if IWDG_ENABLE
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;   /* frozen while halted */
  IWDG->KR = 0xCCCCU;                               /* start (LSI on)      */
  IWDG->KR = 0x5555U;                               /* unlock PR/RLR       */
  IWDG->PR = 0x3U;                                  /* /32                 */
  IWDG->RLR = iwdg_reload_for_ms(IWDG_TIMEOUT_MS);
  IWDG->KR = 0xAAAAU;                               /* reload              */
#endif
}

void board_health_poll(uint32_t now_ms)
{
#if IWDG_ENABLE
  IWDG->KR = 0xAAAAU;                               /* refresh, never waits */
#endif
#if RESET_REASON_LED && !WHEEL_FFB
  /* first 5 s: N short flashes of LD4 (N = reason), repeated */
  if (now_ms < 5000U && s_reason != RESET_POWER_ON)
  {
    const uint32_t n = (uint32_t)s_reason;
    const uint32_t t = now_ms % ((n * 400U) + 1000U);
    const int on = (t < n * 400U) && ((t % 400U) < 150U);
    HAL_GPIO_WritePin(LD4_GPIO_Port, LD4_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
  }
  else if (now_ms < 5100U)
  {
    HAL_GPIO_WritePin(LD4_GPIO_Port, LD4_Pin, GPIO_PIN_RESET);
  }
#else
  (void)now_ms;
#endif
}

reset_reason_t board_health_reset_reason(void)
{
  return s_reason;
}
#endif /* !BOARD_HEALTH_HOST */
