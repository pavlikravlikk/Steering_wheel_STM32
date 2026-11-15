/**
 * @file    odrive_uart.h
 * @brief   USART1 link to the ODrive (PA9 = TX -> ODrive GPIO2,
 *          PA10 = RX <- ODrive GPIO1), 115200 8N1, interrupt driven.
 *
 * USART1 is not enabled in the CubeMX project, so it is configured here
 * register-level (no HAL UART module needed). See docs/IOC_TODO.md.
 *
 * READ-ONLY: odrive_uart_send_readonly() refuses every string that
 * odrive_cmd_is_readonly() does not accept. There is no other TX function.
 */
#ifndef ODRIVE_UART_H
#define ODRIVE_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint32_t rx_dropped;     /* RX ring full                          */
  uint32_t hw_errors;      /* overrun / framing / noise flags       */
  uint32_t tx_refused;     /* not a read-only command               */
  uint32_t tx_busy;        /* previous command still being sent     */
} odrive_uart_stats_t;

/** Configure PA9/PA10 and USART1, enable the RX interrupt. */
void odrive_uart_init(void);

/** Queue a read-only command for transmission. Never blocks. Returns false
 *  if refused (not read-only) or the transmitter is still busy. */
bool odrive_uart_send_readonly(const char *cmd);

/** Copy up to @p max received bytes into @p buf. Never blocks. */
size_t odrive_uart_read(uint8_t *buf, size_t max);

const odrive_uart_stats_t *odrive_uart_stats(void);

#endif /* ODRIVE_UART_H */
