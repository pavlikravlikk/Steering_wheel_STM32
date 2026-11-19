#include "test_runner.h"
#include "mock_odrive_uart.h"
#include "odrive_link.h"
#include "ring_buffer.h"
#include "wheel_config.h"

#include <string.h>

static void setup(void)
{
  mock_uart_reset();
  odrive_link_init();
}

TEST(test_link_power_up_centre_and_axis)
{
  setup();
  CHECK_EQ_INT(g_mock_uart.init_calls, 1);
  CHECK_EQ_INT(odrive_link_axis16(), 32768);
  CHECK(!odrive_link_ok(0));

  odrive_link_request(0);
  CHECK(strcmp(g_mock_uart.last_sent, "f 0\n") == 0);
  mock_uart_rx("2.000000 0.000000\r\n");        /* power-up position */
  odrive_link_process(2);
  CHECK(odrive_link_ok(2));
  CHECK_EQ_INT(odrive_link_axis16(), 32768);    /* became the centre */

  odrive_link_request(10);
  mock_uart_rx("2.375000 0.100000\r\n");        /* +135 deg (540 lock) */
  odrive_link_process(12);
  CHECK_EQ_INT(odrive_link_axis16(), 49152);
}

TEST(test_link_holds_value_when_stale)
{
  setup();
  odrive_link_request(0);
  mock_uart_rx("0 0\n");
  odrive_link_process(1);
  odrive_link_request(10);
  mock_uart_rx("0.375 0\n");
  odrive_link_process(11);
  CHECK_EQ_INT(odrive_link_axis16(), 49152);
  /* ODrive goes silent */
  for (uint32_t t = 20; t < 400; t += 10)
  {
    odrive_link_request(t);
    odrive_link_process(t + 1);
  }
  CHECK(!odrive_link_ok(400));
  CHECK_EQ_INT(odrive_link_axis16(), 49152);    /* last value held */
  CHECK(odrive_link_stats()->timeouts > 0);
  float p, v;
  CHECK(!odrive_link_position(400, &p, &v));
}

TEST(test_link_set_centre)
{
  setup();
  CHECK(!odrive_link_set_centre(0));           /* no data: refused */
  odrive_link_request(0);
  mock_uart_rx("0 0\n");
  odrive_link_process(1);
  odrive_link_request(10);
  mock_uart_rx("0.375 0\n");
  odrive_link_process(11);
  CHECK(odrive_link_set_centre(12));
  CHECK_EQ_INT(odrive_link_axis16(), 32768);
  CHECK(!odrive_link_set_centre(12 + ODRIVE_STALE_MS));  /* stale: refused */
}

TEST(test_link_never_sends_non_readonly)
{
  setup();
  for (uint32_t t = 0; t < 1000; t++)
  {
    odrive_link_request(t);
    if ((t % 10) == 3)
    {
      mock_uart_rx("0.1 0.2\r\n");
    }
    odrive_link_process(t);
  }
  CHECK(g_mock_uart.sent_ok > 50);
  CHECK_EQ_INT(g_mock_uart.refused, 0);
  CHECK(strcmp(g_mock_uart.last_sent, "f 0\n") == 0);
}

TEST(test_link_rate_limited_by_outstanding_request)
{
  setup();
  /* called every ms, no reply: one request per reply timeout */
  for (uint32_t t = 0; t < 80; t++)
  {
    odrive_link_request(t);
  }
  CHECK_EQ_INT(g_mock_uart.sent_ok, 80 / ODRIVE_REPLY_TIMEOUT_MS);
}

TEST(test_link_uart_busy_does_not_mark_sent)
{
  setup();
  g_mock_uart.busy = true;
  odrive_link_request(0);
  CHECK_EQ_INT(odrive_link_stats()->requests, 0);
  g_mock_uart.busy = false;
  odrive_link_request(1);
  CHECK_EQ_INT(odrive_link_stats()->requests, 1);
}

TEST(test_ring_buffer)
{
  static ring_buffer_t rb;
  uint8_t b = 0;
  rb_init(&rb);
  CHECK(!rb_get(&rb, &b));
  for (unsigned i = 0; i < RB_SIZE - 1; i++)
  {
    CHECK(rb_put(&rb, (uint8_t)i));
  }
  CHECK(!rb_put(&rb, 0xAA));                    /* full: capacity N-1 */
  CHECK_EQ_INT(rb_count(&rb), RB_SIZE - 1);
  for (unsigned i = 0; i < RB_SIZE - 1; i++)
  {
    CHECK(rb_get(&rb, &b));
    CHECK_EQ_INT(b, (uint8_t)i);
  }
  CHECK(!rb_get(&rb, &b));
  /* wrap-around */
  for (unsigned i = 0; i < 1000; i++)
  {
    CHECK(rb_put(&rb, (uint8_t)(i * 7)));
    CHECK(rb_get(&rb, &b));
    CHECK_EQ_INT(b, (uint8_t)(i * 7));
  }
  CHECK_EQ_INT(rb_count(&rb), 0);
}

void run_odrive_link_tests(void)
{
  printf("odrive_link (mock UART)\n");
  RUN(test_link_power_up_centre_and_axis);
  RUN(test_link_holds_value_when_stale);
  RUN(test_link_set_centre);
  RUN(test_link_never_sends_non_readonly);
  RUN(test_link_rate_limited_by_outstanding_request);
  RUN(test_link_uart_busy_does_not_mark_sent);
  RUN(test_ring_buffer);
}
