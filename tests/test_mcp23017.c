#include "test_runner.h"
#include "mcp23017_core.h"

#include <string.h>

/* ---------------- mock asynchronous I2C bus ---------------- */
typedef struct
{
  bool    reinit_ok;
  bool    start_ok;
  int     reinit_calls;
  int     writes;
  uint8_t write_regs[16];
  uint8_t write_data[16][2];
  int     reads;
  uint8_t *pending_rx;       /* buffer of the read in flight */
  uint8_t port_a, port_b;    /* electrical levels (1 = high = released) */
  enum { AUTO_DONE, AUTO_ERROR, NEVER } completion;
  mcp_core_t *core;
} mock_bus_t;

static mock_bus_t g_bus;

static void complete(void)
{
  if (g_bus.completion == AUTO_DONE)
  {
    if (g_bus.pending_rx != NULL)
    {
      g_bus.pending_rx[0] = g_bus.port_a;
      g_bus.pending_rx[1] = g_bus.port_b;
      g_bus.pending_rx = NULL;
    }
    mcp_core_on_xfer_done(g_bus.core);        /* "ISR" */
  }
  else if (g_bus.completion == AUTO_ERROR)
  {
    mcp_core_on_xfer_error(g_bus.core);       /* e.g. NACK */
  }
}

static bool m_reinit(void *ctx)
{
  (void)ctx;
  g_bus.reinit_calls++;
  return g_bus.reinit_ok;
}

static bool m_write(void *ctx, uint8_t reg, const uint8_t *data, uint8_t len)
{
  (void)ctx;
  if (!g_bus.start_ok) return false;
  if (g_bus.writes < 16)
  {
    g_bus.write_regs[g_bus.writes] = reg;
    memcpy(g_bus.write_data[g_bus.writes], data, len < 2 ? len : 2);
  }
  g_bus.writes++;
  complete();
  return true;
}

static bool m_read(void *ctx, uint8_t reg, uint8_t *buf, uint8_t len)
{
  (void)ctx;
  if (!g_bus.start_ok || reg != MCP_REG_GPIOA || len != 2) return false;
  g_bus.reads++;
  g_bus.pending_rx = buf;
  complete();
  return true;
}

static mcp_core_t g_core;

static void setup(bool chip_inverts)
{
  memset(&g_bus, 0, sizeof(g_bus));
  g_bus.reinit_ok = true;
  g_bus.start_ok = true;
  g_bus.port_a = 0xFF;          /* all released (pull-ups) */
  g_bus.port_b = 0xFF;
  g_bus.completion = AUTO_DONE;
  g_bus.core = &g_core;
  mcp_io_t io = { m_reinit, m_write, m_read, NULL };
  mcp_core_init(&g_core, &io, chip_inverts, 5, 250, 10, 0);
}

static void run_ms(uint32_t from, uint32_t to)
{
  for (uint32_t t = from; t < to; t++) mcp_core_update(&g_core, t);
}

/* ---------------- tests ---------------- */
TEST(test_mcp_decode_mapping)
{
  /* all released */
  CHECK_EQ_INT(mcp_decode_ports(0xFF, 0xFF, false), 0);
  /* GPA0 low = button 1 */
  CHECK_EQ_INT(mcp_decode_ports(0xFE, 0xFF, false), 0x001);
  /* GPA7 low = button 8 */
  CHECK_EQ_INT(mcp_decode_ports(0x7F, 0xFF, false), 0x080);
  /* GPB0 low = left paddle = button 9 = bit 8 */
  CHECK_EQ_INT(mcp_decode_ports(0xFF, 0xFE, false), 0x100);
  /* GPB1 low = right paddle = button 10 = bit 9 */
  CHECK_EQ_INT(mcp_decode_ports(0xFF, 0xFD, false), 0x200);
  /* GPB2..7 are ignored */
  CHECK_EQ_INT(mcp_decode_ports(0xFF, 0x03, false), 0);
  /* everything pressed */
  CHECK_EQ_INT(mcp_decode_ports(0x00, 0x00, false), 0x3FF);
  /* chip-side inversion (IPOL): 1 = pressed already */
  CHECK_EQ_INT(mcp_decode_ports(0x01, 0x02, true), 0x201);
}

TEST(test_mcp_init_sequence_and_read)
{
  setup(false);
  run_ms(0, 6);                                /* probe, 2 writes, first read */
  CHECK_EQ_INT(g_bus.reinit_calls, 1);         /* immediate first probe */
  CHECK(g_bus.writes >= 2);
  CHECK_EQ_INT(g_bus.write_regs[0], MCP_REG_IODIRA);
  CHECK_EQ_INT(g_bus.write_data[0][0], 0xFF);  /* all inputs */
  CHECK_EQ_INT(g_bus.write_data[0][1], 0xFF);
  CHECK_EQ_INT(g_bus.write_regs[1], MCP_REG_GPPUA);
  CHECK_EQ_INT(g_bus.write_data[1][0], 0xFF);  /* pull-ups on */
  CHECK_EQ_INT(g_bus.writes, 2);               /* no IPOL by default */
  CHECK(mcp_core_present(&g_core));
  CHECK(g_bus.reads > 0);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
}

TEST(test_mcp_ipol_sequence)
{
  setup(true);
  g_bus.port_a = 0x00;                         /* chip inverts: 0 = released */
  g_bus.port_b = 0x00;
  run_ms(0, 10);
  CHECK_EQ_INT(g_bus.writes, 3);
  CHECK_EQ_INT(g_bus.write_regs[2], MCP_REG_IPOLA);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
  g_bus.port_b = 0x02;                         /* right paddle */
  run_ms(10, 20);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x200);
}

TEST(test_mcp_press_debounced_5ms)
{
  setup(false);
  run_ms(0, 10);
  g_bus.port_a = 0xFE;                         /* button 1 pressed at t=10 */
  run_ms(10, 14);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
  run_ms(14, 17);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x001);
  /* bounce on release */
  g_bus.port_a = 0xFF; run_ms(17, 19);
  g_bus.port_a = 0xFE; run_ms(19, 20);
  g_bus.port_a = 0xFF; run_ms(20, 23);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x001);
  run_ms(23, 30);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
}

TEST(test_mcp_paddles)
{
  setup(false);
  run_ms(0, 10);
  g_bus.port_b = 0xFC;                         /* both paddles */
  run_ms(10, 20);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x300);
}

TEST(test_mcp_absent_chip_retries_without_blocking)
{
  setup(false);
  g_bus.completion = AUTO_ERROR;               /* NACK: chip not there */
  run_ms(0, 1000);
  CHECK(!mcp_core_present(&g_core));
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
  /* probe at t=0, 250, 500, 750 */
  CHECK_EQ_INT(g_bus.reinit_calls, 4);
  CHECK_EQ_INT(g_core.stats.faults, 4);
  /* chip appears */
  g_bus.completion = AUTO_DONE;
  run_ms(1000, 1260);
  CHECK(mcp_core_present(&g_core));
}

TEST(test_mcp_error_mid_operation_releases_buttons)
{
  setup(false);
  run_ms(0, 5);
  g_bus.port_a = 0x00;                         /* all 8 buttons pressed */
  run_ms(5, 20);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0xFF);
  g_bus.completion = AUTO_ERROR;               /* cable pulled */
  run_ms(20, 22);
  CHECK(!mcp_core_present(&g_core));
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);  /* fail released */
}

TEST(test_mcp_timeout_when_isr_never_fires)
{
  setup(false);
  g_bus.completion = NEVER;                    /* stuck bus */
  run_ms(0, 9);
  CHECK_EQ_INT(g_core.state, MCP_ST_WAIT_IODIR);
  run_ms(9, 12);
  CHECK_EQ_INT(g_core.state, MCP_ST_FAULT);
  CHECK_EQ_INT(g_core.stats.timeouts, 1);
}

TEST(test_mcp_start_failure_and_reinit_failure)
{
  setup(false);
  g_bus.start_ok = false;                      /* HAL busy */
  run_ms(0, 3);
  CHECK_EQ_INT(g_core.state, MCP_ST_FAULT);
  setup(false);
  g_bus.reinit_ok = false;                     /* bus cannot be re-initialised */
  run_ms(0, 600);
  CHECK_EQ_INT(g_core.state, MCP_ST_FAULT);
  CHECK_EQ_INT(g_bus.reinit_calls, 3);         /* t = 0, 250, 500 */
  CHECK_EQ_INT(g_bus.writes, 0);
}

TEST(test_mcp_simulation_injection)
{
  setup(false);
  g_bus.completion = AUTO_ERROR;               /* no chip at all */
  mcp_core_sim_enable(&g_core, true, 0);
  CHECK(mcp_core_present(&g_core));
  mcp_core_sim_set(&g_core, 0x100);            /* left paddle */
  run_ms(0, 3);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);  /* still debouncing */
  run_ms(3, 8);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x100);
  mcp_core_sim_set(&g_core, 0x281);            /* buttons 1, 8, right paddle */
  run_ms(8, 20);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0x281);
  mcp_core_sim_set(&g_core, 0xFC00);           /* out-of-range bits masked */
  run_ms(20, 40);
  CHECK_EQ_INT(mcp_core_buttons(&g_core), 0);
  CHECK_EQ_INT(g_bus.writes + g_bus.reads, 0); /* bus never touched */
  mcp_core_sim_enable(&g_core, false, 40);
  CHECK(!mcp_core_present(&g_core));
  run_ms(40, 41);
  CHECK_EQ_INT(g_bus.reinit_calls, 1);         /* re-probes the chip */
}

void run_mcp23017_tests(void)
{
  printf("mcp23017 (mock I2C)\n");
  RUN(test_mcp_decode_mapping);
  RUN(test_mcp_init_sequence_and_read);
  RUN(test_mcp_ipol_sequence);
  RUN(test_mcp_press_debounced_5ms);
  RUN(test_mcp_paddles);
  RUN(test_mcp_absent_chip_retries_without_blocking);
  RUN(test_mcp_error_mid_operation_releases_buttons);
  RUN(test_mcp_timeout_when_isr_never_fires);
  RUN(test_mcp_start_failure_and_reinit_failure);
  RUN(test_mcp_simulation_injection);
}
