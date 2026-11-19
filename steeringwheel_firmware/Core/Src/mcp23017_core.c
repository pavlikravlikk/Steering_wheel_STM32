/**
 * @file    mcp23017_core.c
 * @brief   MCP23017 state machine + debounce. Must not include any HAL header.
 */
#include "mcp23017_core.h"

#include <string.h>

uint16_t mcp_decode_ports(uint8_t gpioa, uint8_t gpiob, bool chip_inverts)
{
  uint16_t raw = (uint16_t)(gpioa | ((uint16_t)(gpiob & 0x03U) << 8));

  if (!chip_inverts)
  {
    raw = (uint16_t)~raw;     /* active low: pull-up, switch to GND */
  }
  return (uint16_t)(raw & ((1U << MCP_BUTTON_COUNT) - 1U));
}

static void set_state(mcp_core_t *m, mcp_state_t st, uint32_t now_ms)
{
  m->state = st;
  m->state_since_ms = now_ms;
}

static void enter_fault(mcp_core_t *m, uint32_t now_ms)
{
  set_state(m, MCP_ST_FAULT, now_ms);
  m->stats.faults++;
  debounce_reset(&m->deb, now_ms);   /* fail released, never stuck pressed */
}

/* Flags are cleared BEFORE a transfer is started, so a fast ISR completion
 * can never be wiped afterwards. */
static bool start_write(mcp_core_t *m, uint8_t reg)
{
  m->xfer_done = 0U;
  m->xfer_error = 0U;
  return m->io.start_write(m->io.ctx, reg, m->cfg_ff, 2U);
}

static bool start_read(mcp_core_t *m)
{
  m->xfer_done = 0U;
  m->xfer_error = 0U;
  return m->io.start_read(m->io.ctx, MCP_REG_GPIOA, m->rx, 2U);
}

void mcp_core_init(mcp_core_t *m, const mcp_io_t *io, bool chip_inverts,
                   uint32_t debounce_ms, uint32_t retry_ms, uint32_t timeout_ms,
                   uint32_t now_ms)
{
  memset(m, 0, sizeof(*m));
  m->io = *io;
  m->chip_inverts = chip_inverts;
  m->retry_ms = retry_ms;
  m->timeout_ms = timeout_ms;
  m->cfg_ff[0] = 0xFFU;
  m->cfg_ff[1] = 0xFFU;
  debounce_init(&m->deb, MCP_BUTTON_COUNT, debounce_ms, now_ms);
  /* FAULT with an already expired retry timer: first update starts setup. */
  m->state = MCP_ST_FAULT;
  m->state_since_ms = now_ms - retry_ms;
}

void mcp_core_on_xfer_done(mcp_core_t *m)
{
  m->xfer_done = 1U;
}

void mcp_core_on_xfer_error(mcp_core_t *m)
{
  m->xfer_error = 1U;
}

/* Common handling of a WAIT state: error/timeout -> fault, done -> next. */
static void wait_then(mcp_core_t *m, mcp_state_t next, uint32_t now_ms)
{
  if (m->xfer_error != 0U)
  {
    enter_fault(m, now_ms);
  }
  else if (m->xfer_done != 0U)
  {
    set_state(m, next, now_ms);
  }
  else if ((now_ms - m->state_since_ms) >= m->timeout_ms)
  {
    m->stats.timeouts++;
    enter_fault(m, now_ms);
  }
}

static void start_or_fault(mcp_core_t *m, bool started, mcp_state_t wait_state,
                           uint32_t now_ms)
{
  if (started)
  {
    set_state(m, wait_state, now_ms);
  }
  else
  {
    enter_fault(m, now_ms);
  }
}

void mcp_core_update(mcp_core_t *m, uint32_t now_ms)
{
  if (m->sim)
  {
    (void)debounce_update(&m->deb, m->sim_raw, now_ms);
    return;
  }

  switch (m->state)
  {
    case MCP_ST_FAULT:
      if ((now_ms - m->state_since_ms) >= m->retry_ms)
      {
        if (m->io.reinit(m->io.ctx))
        {
          set_state(m, MCP_ST_START_IODIR, now_ms);
        }
        else
        {
          m->state_since_ms = now_ms;   /* bus not ready: wait another period */
        }
      }
      break;

    case MCP_ST_START_IODIR:
      start_or_fault(m, start_write(m, MCP_REG_IODIRA), MCP_ST_WAIT_IODIR, now_ms);
      break;

    case MCP_ST_WAIT_IODIR:
      wait_then(m, MCP_ST_START_GPPU, now_ms);
      break;

    case MCP_ST_START_GPPU:
      start_or_fault(m, start_write(m, MCP_REG_GPPUA), MCP_ST_WAIT_GPPU, now_ms);
      break;

    case MCP_ST_WAIT_GPPU:
      wait_then(m, m->chip_inverts ? MCP_ST_START_IPOL : MCP_ST_START_READ, now_ms);
      break;

    case MCP_ST_START_IPOL:
      start_or_fault(m, start_write(m, MCP_REG_IPOLA), MCP_ST_WAIT_IPOL, now_ms);
      break;

    case MCP_ST_WAIT_IPOL:
      wait_then(m, MCP_ST_START_READ, now_ms);
      break;

    case MCP_ST_START_READ:
      start_or_fault(m, start_read(m), MCP_ST_WAIT_READ, now_ms);
      break;

    case MCP_ST_WAIT_READ:
      if ((m->xfer_error == 0U) && (m->xfer_done != 0U))
      {
        m->stats.samples++;
        (void)debounce_update(&m->deb,
                              mcp_decode_ports(m->rx[0], m->rx[1], m->chip_inverts),
                              now_ms);
        /* next sample right away: one read per 1 ms update */
        start_or_fault(m, start_read(m), MCP_ST_WAIT_READ, now_ms);
      }
      else
      {
        wait_then(m, MCP_ST_WAIT_READ, now_ms);   /* error / timeout -> fault */
      }
      break;

    default:
      enter_fault(m, now_ms);
      break;
  }
}

uint16_t mcp_core_buttons(const mcp_core_t *m)
{
  return m->deb.stable;
}

bool mcp_core_present(const mcp_core_t *m)
{
  return m->sim || (m->state == MCP_ST_START_READ) || (m->state == MCP_ST_WAIT_READ);
}

void mcp_core_sim_enable(mcp_core_t *m, bool on, uint32_t now_ms)
{
  m->sim = on;
  m->sim_raw = 0U;
  debounce_reset(&m->deb, now_ms);
  if (!on)
  {
    set_state(m, MCP_ST_FAULT, now_ms - m->retry_ms);   /* re-probe the chip */
  }
}

void mcp_core_sim_set(mcp_core_t *m, uint16_t raw_pressed_mask)
{
  m->sim_raw = (uint16_t)(raw_pressed_mask & ((1U << MCP_BUTTON_COUNT) - 1U));
}
