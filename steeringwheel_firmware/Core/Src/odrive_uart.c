/**
 * @file    odrive_uart.c
 * @brief   Register-level USART1 driver with RX/TX interrupts and ring buffers.
 *
 * Clock: USART1 kernel clock = PCLK2 (48 MHz with the current clock tree,
 * matches RCC.USART1Freq_Value in the .ioc). BRR is computed at run time.
 */
#include "odrive_uart.h"

#include <string.h>

#include "main.h"
#include "odrive_ascii.h"
#include "ring_buffer.h"
#include "wheel_config.h"

#define ODRIVE_UART_IRQ_PRIO  1U

#if WHEEL_FFB
/* FFB build: a small queue of whole command lines (requests + allowlisted
 * writes, see odrive_uart_send_ffb()). */
#include "ffb_cmd.h"
#define ODRIVE_TX_BUF_SIZE    160U
typedef uint16_t tx_idx_t;
#else
/* Default build: exactly one read-only command at a time. */
#define ODRIVE_TX_BUF_SIZE    ODR_CMD_MAX
typedef uint8_t tx_idx_t;
#endif

static ring_buffer_t       s_rx;
static uint8_t             s_tx[ODRIVE_TX_BUF_SIZE];
static volatile tx_idx_t   s_tx_len;
static volatile tx_idx_t   s_tx_pos;
static volatile bool       s_tx_active;
static odrive_uart_stats_t s_stats;

void odrive_uart_init(void)
{
  GPIO_InitTypeDef gpio = {0};
  uint32_t pclk2;

  rb_init(&s_rx);
  s_tx_active = false;

  __HAL_RCC_GPIOA_CLK_ENABLE();
  /* PA9 = USART1_TX (AF7), push-pull, 3.3 V. */
  gpio.Pin       = GPIO_PIN_9;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_NOPULL;
  gpio.Speed     = GPIO_SPEED_FREQ_LOW;
  gpio.Alternate = GPIO_AF7_USART1;
  HAL_GPIO_Init(GPIOA, &gpio);
  /* PA10 = USART1_RX (AF7), pull-up keeps the line idle (high) when the
   * ODrive is unplugged, so no garbage is received. */
  gpio.Pin       = GPIO_PIN_10;
  gpio.Pull      = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &gpio);

  /* USART1 kernel clock = PCLK2 (USART1SW = 00). */
  RCC->CFGR3 &= ~RCC_CFGR3_USART1SW;
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_USART1_FORCE_RESET();
  __HAL_RCC_USART1_RELEASE_RESET();

  pclk2 = HAL_RCC_GetPCLK2Freq();
  USART1->CR1 = 0U;                                     /* disabled, 8N1, OVER16 */
  USART1->CR2 = 0U;                                     /* 1 stop bit            */
  USART1->CR3 = 0U;                                     /* no flow control       */
  USART1->BRR = (pclk2 + (ODRIVE_UART_BAUD / 2U)) / ODRIVE_UART_BAUD;
  USART1->ICR = 0xFFFFFFFFU;
  USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

  HAL_NVIC_SetPriority(USART1_IRQn, ODRIVE_UART_IRQ_PRIO, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}

#if !WHEEL_FFB
bool odrive_uart_send_readonly(const char *cmd)
{
  size_t n;

  if (!odrive_cmd_is_readonly(cmd))
  {
    s_stats.tx_refused++;
    return false;
  }
  if (s_tx_active)
  {
    s_stats.tx_busy++;
    return false;
  }
  n = strlen(cmd);              /* <= ODR_CMD_MAX - 1, checked above */
  memcpy(s_tx, cmd, n);
  s_tx_len    = (uint8_t)n;
  s_tx_pos    = 0U;
  s_tx_active = true;
  USART1->CR1 |= USART_CR1_TXEIE;   /* ISR feeds TDR */
  return true;
}
#else /* WHEEL_FFB */
/* Append one complete line. With @p urgent, every queued line that has not
 * started yet is dropped first (the line on the wire is finished, so the
 * ODrive never sees a truncated command). Runs with the USART1 IRQ masked
 * for a few microseconds; never waits. */
static bool tx_enqueue(const char *cmd, bool urgent)
{
  const size_t n = strlen(cmd);
  bool ok = false;

  HAL_NVIC_DisableIRQ(USART1_IRQn);
  if (!s_tx_active || (s_tx_pos >= s_tx_len))
  {
    s_tx_len = 0U;
    s_tx_pos = 0U;
  }
  else
  {
    if (urgent)
    {
      tx_idx_t end = s_tx_pos;
      if (end > 0U)
      {
        while ((end < s_tx_len) && (s_tx[end - 1U] != '\n'))
        {
          end++;                       /* finish the line being sent */
        }
      }
      s_tx_len = end;
    }
    if (s_tx_pos > 0U)                 /* compact: unsent bytes to the front */
    {
      memmove(s_tx, &s_tx[s_tx_pos], (size_t)(s_tx_len - s_tx_pos));
      s_tx_len = (tx_idx_t)(s_tx_len - s_tx_pos);
      s_tx_pos = 0U;
    }
  }
  if ((n > 0U) && ((size_t)s_tx_len + n <= ODRIVE_TX_BUF_SIZE))
  {
    memcpy(&s_tx[s_tx_len], cmd, n);
    s_tx_len = (tx_idx_t)(s_tx_len + n);
    ok = true;
  }
  if (s_tx_len > s_tx_pos)
  {
    s_tx_active = true;
    USART1->CR1 |= USART_CR1_TXEIE;   /* ISR feeds TDR */
  }
  HAL_NVIC_EnableIRQ(USART1_IRQn);
  if (!ok)
  {
    s_stats.tx_busy++;
  }
  return ok;
}

bool odrive_uart_send_readonly(const char *cmd)
{
  if (!odrive_cmd_is_readonly(cmd))
  {
    s_stats.tx_refused++;
    return false;
  }
  return tx_enqueue(cmd, false);
}

bool odrive_uart_send_ffb(const char *cmd, bool urgent)
{
  /* The ONLY write entry point: torque setpoint, IDLE/CLOSED_LOOP request,
   * watchdog feed - nothing else passes ffb_cmd_is_allowed(). */
  if (!ffb_cmd_is_allowed(cmd, ODRIVE_AXIS))
  {
    s_stats.tx_refused++;
    return false;
  }
  return tx_enqueue(cmd, urgent);
}
#endif /* WHEEL_FFB */

size_t odrive_uart_read(uint8_t *buf, size_t max)
{
  size_t n = 0U;
  while ((n < max) && rb_get(&s_rx, &buf[n]))
  {
    n++;
  }
  return n;
}

const odrive_uart_stats_t *odrive_uart_stats(void)
{
  return &s_stats;
}

void USART1_IRQHandler(void)
{
  const uint32_t isr = USART1->ISR;

  if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE)) != 0U)
  {
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
    s_stats.hw_errors++;
    /* inject a marker so the parser discards the damaged line */
    (void)rb_put(&s_rx, 0x00U);
  }

  if ((isr & USART_ISR_RXNE) != 0U)
  {
    const uint8_t b = (uint8_t)(USART1->RDR & 0xFFU);   /* clears RXNE */
    if (!rb_put(&s_rx, b))
    {
      s_stats.rx_dropped++;
    }
  }

  if (((USART1->CR1 & USART_CR1_TXEIE) != 0U) && ((isr & USART_ISR_TXE) != 0U))
  {
    if (s_tx_pos < s_tx_len)
    {
      USART1->TDR = s_tx[s_tx_pos];
      s_tx_pos++;
    }
    else
    {
      USART1->CR1 &= ~USART_CR1_TXEIE;
      s_tx_active = false;
    }
  }
}
