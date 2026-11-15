/**
 * @file    buttons_mcp23017.c
 * @brief   HAL glue for the MCP23017 button expander on I2C1 (PB6/PB7).
 *
 * All logic (state machine, debounce, mapping, simulation) lives in the pure
 * mcp23017_core.c and is unit-tested on the host. This file only provides:
 *   - the asynchronous I2C interface (interrupt-driven HAL_I2C_Mem_*_IT),
 *   - the I2C1 interrupt handlers / HAL completion callbacks,
 *   - the red LED LD10 blinking while the expander is missing.
 * Nothing here blocks.
 *
 * Note: I2C1 is shared with the on-board LSM303DLHC (addresses 0x19/0x1E);
 * no other code talks to it at the moment.
 */
#include "buttons_mcp23017.h"

#include "mcp23017_core.h"
#include "wheel_config.h"

#define MCP_ADDR_8BIT   (MCP23017_I2C_ADDR_7BIT << 1)

static I2C_HandleTypeDef *s_hi2c;
static mcp_core_t         s_core;
static uint32_t           s_led_ms;
static uint32_t           s_last_now;

/* ---------------------------------------------------------------------- */
/* Interrupt plumbing                                                     */
/* ---------------------------------------------------------------------- */
void I2C1_EV_IRQHandler(void)
{
  HAL_I2C_EV_IRQHandler(s_hi2c);
}

void I2C1_ER_IRQHandler(void)
{
  HAL_I2C_ER_IRQHandler(s_hi2c);
}

void HAL_I2C_MemTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == s_hi2c)
  {
    mcp_core_on_xfer_done(&s_core);
  }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == s_hi2c)
  {
    mcp_core_on_xfer_done(&s_core);
  }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == s_hi2c)
  {
    mcp_core_on_xfer_error(&s_core);
  }
}

/* ---------------------------------------------------------------------- */
/* Asynchronous I2C interface for the core                                */
/* ---------------------------------------------------------------------- */
static bool io_reinit(void *ctx)
{
  RCC_PeriphCLKInitTypeDef clk = {0};
  (void)ctx;

  (void)HAL_I2C_DeInit(s_hi2c);    /* also aborts a stuck transfer */

  /* Run I2C1 from SYSCLK (48 MHz) instead of HSI (8 MHz) so that the timing
   * constant for 400 kHz fast mode is available. */
  clk.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
  clk.I2c1ClockSelection   = RCC_I2C1CLKSOURCE_SYSCLK;
  if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
  {
    return false;
  }

  s_hi2c->Init.Timing = I2C1_TIMING_400KHZ_48MHZ;
  if (HAL_I2C_Init(s_hi2c) != HAL_OK)
  {
    return false;
  }
  if (HAL_I2CEx_ConfigAnalogFilter(s_hi2c, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    return false;
  }
  return (HAL_I2CEx_ConfigDigitalFilter(s_hi2c, 0U) == HAL_OK);
}

static bool io_start_write(void *ctx, uint8_t reg, const uint8_t *data, uint8_t len)
{
  (void)ctx;
  return HAL_I2C_Mem_Write_IT(s_hi2c, MCP_ADDR_8BIT, reg, I2C_MEMADD_SIZE_8BIT,
                              (uint8_t *)data, len) == HAL_OK;
}

static bool io_start_read(void *ctx, uint8_t reg, uint8_t *buf, uint8_t len)
{
  (void)ctx;
  return HAL_I2C_Mem_Read_IT(s_hi2c, MCP_ADDR_8BIT, reg, I2C_MEMADD_SIZE_8BIT,
                             buf, len) == HAL_OK;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */
void buttons_init(I2C_HandleTypeDef *hi2c)
{
  static const mcp_io_t io = { io_reinit, io_start_write, io_start_read, NULL };

  s_hi2c = hi2c;
  mcp_core_init(&s_core, &io, MCP23017_USE_IPOL != 0, BUTTON_DEBOUNCE_MS,
                MCP23017_RETRY_MS, MCP23017_XFER_TIMEOUT_MS, 0U);

  HAL_NVIC_SetPriority(I2C1_EV_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  HAL_NVIC_SetPriority(I2C1_ER_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void buttons_update(uint32_t now_ms)
{
  s_last_now = now_ms;
  mcp_core_update(&s_core, now_ms);

  if (!mcp_core_present(&s_core))
  {
    /* Blink the red LED (LD10) while the expander is missing. */
    if ((now_ms - s_led_ms) >= 125U)
    {
      s_led_ms = now_ms;
      HAL_GPIO_TogglePin(LD10_GPIO_Port, LD10_Pin);
    }
  }
  else
  {
    HAL_GPIO_WritePin(LD10_GPIO_Port, LD10_Pin, GPIO_PIN_RESET);
  }
}

uint16_t buttons_get(void)
{
  return mcp_core_buttons(&s_core);
}

bool buttons_is_present(void)
{
  return mcp_core_present(&s_core);
}

void buttons_sim_enable(bool on)
{
  mcp_core_sim_enable(&s_core, on, s_last_now);
}

void buttons_sim_set(uint16_t raw_pressed_mask)
{
  mcp_core_sim_set(&s_core, raw_pressed_mask);
}
