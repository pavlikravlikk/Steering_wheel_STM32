#ifndef MOCK_ODRIVE_UART_H
#define MOCK_ODRIVE_UART_H

#include "odrive_uart.h"

typedef struct
{
  int    init_calls;
  int    send_calls;
  int    sent_ok;
  int    refused;
  bool   busy;
  char   last_sent[64];
  char   rx[1024];
  size_t rx_len;
  size_t rx_pos;
} mock_uart_t;

extern mock_uart_t g_mock_uart;

void mock_uart_reset(void);
void mock_uart_rx(const char *s);

#endif
