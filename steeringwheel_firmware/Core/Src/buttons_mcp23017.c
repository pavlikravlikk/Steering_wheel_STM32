/**
 * @file    buttons_mcp23017.c
 * @brief   MCP23017 driver: interrupt-driven I2C, 1 ms sampling, 5 ms debounce.
 *
 * Register layout assumed: IOCON.BANK = 0 (power-on default), sequential
 * operation enabled, so reading from GPIOA returns GPIOA then GPIOB in one
 * I2C transaction.
 *
 * I2C events are handled in the I2C1 interrupts (the strong handlers below
 * override the weak vector-table entries). The state machine is advanced from
 * the main context in buttons_update(), so nothing here blocks.
 */
#include "buttons_mcp23017.h"
#include "wheel_config.h"

#define MCP_ADDR_8BIT   (MCP23017_I2C_ADDR_7BIT << 1)
#define MCP_REG_IODIRA  0x00U   /* IODIRA, IODIRB follow                   */
#define MCP_REG_GPPUA   0x0CU   /* GPPUA,  GPPUB  follow                   */
#define MCP_REG_GPIOA   0x12U   /* GPIOA,  GPIOB  follow                   */

typedef enum
{
  ST_FAULT = 0,     /* absent / error: buttons = 0, LED blinks, retry later  */
  ST_START_IODIR,   /* issue IODIRA/B = 0xFF (all inputs)                    */
  ST_WAIT_IODIR,
  ST_START_GPPU,    /* issue GPPUA/B = 0xFF (pull-ups on)                    */
  ST_WAIT_GPPU,
  ST_START_READ,    /* issue first GPIOA/B read                              */
  ST_WAIT_READ      /* read in flight; restarted every time it completes     */
} btn_state_t;

static I2C_HandleTypeDef *s_hi2c;
static btn_state_t        s_state = ST_FAULT;

/* Written from the I2C ISR, consumed in buttons_update(). */
static volatile uint8_t s_xfer_done;
static volatile uint8_t s_xfer_error;

static uint8_t  s_cfg_ff[2] = {0xFFU, 0xFFU};
static uint8_t  s_rx[2];

static uint32_t s_state_since_ms;
static uint32_t s_led_ms;
static uint16_t s_stable;                       /* debounced, 1 = pressed   */
static uint8_t  s_debounce_cnt[BUTTON_COUNT];

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
    s_xfer_done = 1U;
  }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == s_hi2c)
  {
    s_xfer_done = 1U;
  }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == s_hi2c)
  {
    s_xfer_error = 1U;
  }
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */
static bool i2c_setup(void)
{
  RCC_PeriphCLKInitTypeDef clk = {0};

  (void)HAL_I2C_DeInit(s_hi2c);

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

static void enter_fault(uint32_t now_ms)
{
  s_state          = ST_FAULT;
  s_state_since_ms = now_ms;
  s_stable         = 0U;
  for (uint32_t i = 0U; i < BUTTON_COUNT; i++)
  {
    s_debounce_cnt[i] = 0U;
  }
}

static void set_state(btn_state_t st, uint32_t now_ms)
{
  s_state          = st;
  s_state_since_ms = now_ms;
}

/** Map the raw MCP23017 ports to the button mask (1 = pressed). */
static uint16_t decode_ports(uint8_t gpioa, uint8_t gpiob)
{
  const uint16_t raw = (uint16_t)(gpioa | ((uint16_t)(gpiob & 0x03U) << 8));
  /* Inputs are active low (pull-up, switch to GND): invert. */
  return (uint16_t)((~raw) & ((1U << BUTTON_COUNT) - 1U));
}

static void debounce(uint16_t sample)
{
  for (uint32_t i = 0U; i < BUTTON_COUNT; i++)
  {
    const uint16_t bit = (uint16_t)(1U << i);

    if (((sample ^ s_stable) & bit) == 0U)
    {
      s_debounce_cnt[i] = 0U;            /* sample agrees with stable state */
    }
    else if (++s_debounce_cnt[i] >= BUTTON_DEBOUNCE_MS)
    {
      s_stable         ^= bit;           /* held long enough: accept change */
      s_debounce_cnt[i] = 0U;
    }
  }
}

/* The completion flags are cleared BEFORE a transfer is started so that the
 * ISR can never set them and have them wiped afterwards. */
static bool start_write(uint8_t reg, uint8_t *data)
{
  s_xfer_done  = 0U;
  s_xfer_error = 0U;
  return HAL_I2C_Mem_Write_IT(s_hi2c, MCP_ADDR_8BIT, reg, I2C_MEMADD_SIZE_8BIT,
                              data, 2U) == HAL_OK;
}

static bool start_read(void)
{
  s_xfer_done  = 0U;
  s_xfer_error = 0U;
  return HAL_I2C_Mem_Read_IT(s_hi2c, MCP_ADDR_8BIT, MCP_REG_GPIOA,
                             I2C_MEMADD_SIZE_8BIT, s_rx, 2U) == HAL_OK;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */
void buttons_init(I2C_HandleTypeDef *hi2c)
{
  s_hi2c = hi2c;

  HAL_NVIC_SetPriority(I2C1_EV_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
  HAL_NVIC_SetPriority(I2C1_ER_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);

  /* Start in FAULT with an expired retry timer: the first buttons_update()
   * performs the I2C re-configuration and begins the chip setup. */
  s_state_since_ms = 0U;
  s_state          = ST_FAULT;
  s_stable         = 0U;
}

void buttons_update(uint32_t now_ms)
{
  const bool timed_out = (now_ms - s_state_since_ms) >= MCP23017_XFER_TIMEOUT_MS;

  switch (s_state)
  {
    case ST_FAULT:
      /* Blink the red LED (LD10) while the expander is missing. */
      if ((now_ms - s_led_ms) >= 125U)
      {
        s_led_ms = now_ms;
        HAL_GPIO_TogglePin(LD10_GPIO_Port, LD10_Pin);
      }
      if ((now_ms - s_state_since_ms) >= MCP23017_RETRY_MS)
      {
        if (i2c_setup())
        {
          set_state(ST_START_IODIR, now_ms);
        }
        else
        {
          s_state_since_ms = now_ms;
        }
      }
      break;

    case ST_START_IODIR:
      if (start_write(MCP_REG_IODIRA, s_cfg_ff))
      {
        set_state(ST_WAIT_IODIR, now_ms);
      }
      else
      {
        enter_fault(now_ms);
      }
      break;

    case ST_WAIT_IODIR:
      if (s_xfer_error || timed_out)
      {
        enter_fault(now_ms);
      }
      else if (s_xfer_done)
      {
        set_state(ST_START_GPPU, now_ms);
      }
      break;

    case ST_START_GPPU:
      if (start_write(MCP_REG_GPPUA, s_cfg_ff))
      {
        set_state(ST_WAIT_GPPU, now_ms);
      }
      else
      {
        enter_fault(now_ms);
      }
      break;

    case ST_WAIT_GPPU:
      if (s_xfer_error || timed_out)
      {
        enter_fault(now_ms);
      }
      else if (s_xfer_done)
      {
        set_state(ST_START_READ, now_ms);
      }
      break;

    case ST_START_READ:
      if (start_read())
      {
        set_state(ST_WAIT_READ, now_ms);
      }
      else
      {
        enter_fault(now_ms);
      }
      break;

    case ST_WAIT_READ:
      if (s_xfer_error || timed_out)
      {
        enter_fault(now_ms);
      }
      else if (s_xfer_done)
      {
        HAL_GPIO_WritePin(LD10_GPIO_Port, LD10_Pin, GPIO_PIN_RESET);
        debounce(decode_ports(s_rx[0], s_rx[1]));
        /* Start the next read right away: one sample per 1 ms tick. */
        if (start_read())
        {
          set_state(ST_WAIT_READ, now_ms);
        }
        else
        {
          enter_fault(now_ms);
        }
      }
      break;

    default:
      enter_fault(now_ms);
      break;
  }
}

uint16_t buttons_get(void)
{
  return s_stable;
}

bool buttons_is_present(void)
{
  return (s_state == ST_START_READ) || (s_state == ST_WAIT_READ);
}
