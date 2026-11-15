/* Mock of odrive_uart.h for host tests: records everything "sent",
 * lets the test inject received bytes. Every sent string is checked
 * against the read-only allowlist, exactly like the real driver. */
#include "mock_odrive_uart.h"

#include <string.h>

#include "odrive_ascii.h"

mock_uart_t g_mock_uart;

void mock_uart_reset(void)
{
  memset(&g_mock_uart, 0, sizeof(g_mock_uart));
}

void mock_uart_rx(const char *s)
{
  size_t n = strlen(s);
  if (g_mock_uart.rx_len + n <= sizeof(g_mock_uart.rx))
  {
    memcpy(&g_mock_uart.rx[g_mock_uart.rx_len], s, n);
    g_mock_uart.rx_len += n;
  }
}

void odrive_uart_init(void)
{
  g_mock_uart.init_calls++;
}

bool odrive_uart_send_readonly(const char *cmd)
{
  g_mock_uart.send_calls++;
  if (!odrive_cmd_is_readonly(cmd))
  {
    g_mock_uart.refused++;
    return false;
  }
  if (g_mock_uart.busy)
  {
    return false;
  }
  strncpy(g_mock_uart.last_sent, cmd, sizeof(g_mock_uart.last_sent) - 1);
  g_mock_uart.sent_ok++;
  return true;
}

size_t odrive_uart_read(uint8_t *buf, size_t max)
{
  size_t avail = g_mock_uart.rx_len - g_mock_uart.rx_pos;
  size_t n = (avail < max) ? avail : max;
  memcpy(buf, &g_mock_uart.rx[g_mock_uart.rx_pos], n);
  g_mock_uart.rx_pos += n;
  return n;
}

static odrive_uart_stats_t s_dummy;
const odrive_uart_stats_t *odrive_uart_stats(void)
{
  return &s_dummy;
}
