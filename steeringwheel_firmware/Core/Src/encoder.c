/**
 * @file    encoder.c
 * @brief   TIM3 hardware quadrature decoder (x4) with 32-bit accumulation.
 *
 * TIM3 is programmed through CMSIS registers, so no HAL TIM module and no
 * CubeMX-generated MSP code is required.
 */
#include "encoder.h"
#include "main.h"
#include "wheel_config.h"

static uint16_t s_last_cnt;
static int32_t  s_counts;

void encoder_init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_PULLUP;      /* open-collector outputs need pull-ups */
  gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
  gpio.Alternate = GPIO_AF2_TIM3;
  HAL_GPIO_Init(GPIOC, &gpio);

  __HAL_RCC_TIM3_CLK_ENABLE();

  TIM3->CR1   = 0U;
  TIM3->PSC   = 0U;
  TIM3->ARR   = 0xFFFFU;
  /* CC1S = CC2S = 01: inputs mapped on TI1 / TI2, with digital filters. */
  TIM3->CCMR1 = (1U << TIM_CCMR1_CC1S_Pos) | (1U << TIM_CCMR1_CC2S_Pos)
              | (ENCODER_INPUT_FILTER << TIM_CCMR1_IC1F_Pos)
              | (ENCODER_INPUT_FILTER << TIM_CCMR1_IC2F_Pos);
  TIM3->CCER  = 0U;                  /* both inputs non-inverted, rising */
  /* SMS = 0b011: encoder mode 3, count on both edges of TI1 and TI2 (x4). */
  TIM3->SMCR  = (TIM3->SMCR & ~TIM_SMCR_SMS) | 3U;
  TIM3->CNT   = 0U;
  TIM3->CR1   = TIM_CR1_CEN;

  s_last_cnt = 0U;
  s_counts   = 0;
}

void encoder_update(void)
{
  const uint16_t cnt = (uint16_t)TIM3->CNT;
  /* Unsigned subtraction then reinterpretation as int16 handles wrap-around
   * of the 16-bit counter in both directions. */
  const int16_t delta = (int16_t)(uint16_t)(cnt - s_last_cnt);

  s_last_cnt = cnt;
  s_counts  += delta;
}

int32_t encoder_get_counts(void)
{
#if ENCODER_INVERT_DIRECTION
  return -s_counts;
#else
  return s_counts;
#endif
}

uint16_t encoder_get_axis16(void)
{
  /* 64-bit intermediate: counts * 65535 can exceed 32 bits if the wheel is
   * turned many revolutions past the lock (the result is clamped anyway). */
  int64_t x = 32768 + ((int64_t)encoder_get_counts() * 65535) / ENCODER_COUNTS_FULL_RANGE;

  if (x < 0)
  {
    x = 0;
  }
  else if (x > 65535)
  {
    x = 65535;
  }
  return (uint16_t)x;
}

void encoder_set_center(void)
{
  s_counts = 0;
}

void encoder_home_to_limit_switch(void)
{
  /* TODO (future): drive/turn the wheel to a limit switch, read the switch
   * on a free GPIO, then call encoder_set_center() (or set the count to the
   * known lock position) so the centre survives power cycles. */
}
