/**
 * @file    steering_pot.c
 * @brief   Non-blocking potentiometer reader.
 *
 * One conversion (601.5 ADC cycles, about 13 us) is started every tick and
 * collected on the next tick, so the main loop never polls the ADC.
 * Filtering: exponential moving average (alpha = 1/8 per 1 ms sample),
 * deadband against jitter and clamping at both ends.
 */
#include "steering_pot.h"
#include "wheel_config.h"

static ADC_HandleTypeDef *s_hadc;
static int32_t  s_acc_q8;        /* EMA accumulator, 16-bit value in Q8     */
static uint16_t s_last_output;
static uint8_t  s_initialized;

void steering_pot_init(ADC_HandleTypeDef *hadc)
{
  s_hadc = hadc;
  (void)HAL_ADCEx_Calibration_Start(hadc, ADC_SINGLE_ENDED);
  (void)HAL_ADC_Start(hadc);     /* enables the ADC and starts conversion 1 */
}

void steering_pot_update(void)
{
  if (s_hadc == NULL)
  {
    return;
  }

  if (__HAL_ADC_GET_FLAG(s_hadc, ADC_FLAG_EOC) == RESET)
  {
    return;                      /* not finished yet: keep the old value    */
  }

  const uint32_t raw12 = s_hadc->Instance->DR & 0x0FFFU;   /* clears EOC    */
  SET_BIT(s_hadc->Instance->CR, ADC_CR_ADSTART);           /* next sample   */

  const int32_t v16 = (int32_t)((raw12 * 65535U) / 4095U);

  if (!s_initialized)
  {
    s_acc_q8      = v16 << 8;
    s_last_output = (uint16_t)v16;
    s_initialized = 1U;
    return;
  }

  s_acc_q8 += ((v16 << 8) - s_acc_q8) >> 3;
  const int32_t smoothed = s_acc_q8 >> 8;

  int32_t diff = smoothed - (int32_t)s_last_output;
  if (diff < 0)
  {
    diff = -diff;
  }
  if (diff >= (int32_t)POT_DEADBAND_16BIT)
  {
    s_last_output = (uint16_t)smoothed;
  }
}

uint16_t steering_pot_get_axis16(void)
{
  /* Same end clamps as the original 12-bit code (<= 8 and >= 4087). */
  if (s_last_output <= (8U * 16U))
  {
    return 0U;
  }
  if (s_last_output >= (4087U * 16U))
  {
    return 65535U;
  }
  return s_last_output;
}
