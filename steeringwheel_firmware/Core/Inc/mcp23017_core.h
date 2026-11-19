/**
 * @file    mcp23017_core.h
 * @brief   MCP23017 button expander logic (pure, host-tested).
 *
 * Talks to the chip through a small asynchronous I2C interface (start a
 * transfer, completion reported later via mcp_core_on_xfer_done/_error, e.g.
 * from an ISR), so it never blocks and can be driven by a mock.
 *
 * Sequence: IODIRA/B = 0xFF (inputs) -> GPPUA/B = 0xFF (pull-ups) ->
 * [IPOLA/B = 0xFF if the chip should invert] -> continuous GPIOA/B reads.
 * Any start failure, error or timeout -> FAULT: buttons read as released,
 * retry after retry_ms. Assumes IOCON.BANK = 0 (power-on default).
 *
 * Button map (bit = HID button - 1, 1 = pressed):
 *   GPA0..GPA7 -> bits 0..7 (buttons 1..8)
 *   GPB0 -> bit 8 (button 9, left paddle), GPB1 -> bit 9 (button 10, right paddle)
 *
 * Simulation: mcp_core_sim_enable() + mcp_core_sim_set() inject a raw button
 * mask (1 = pressed) instead of reading the chip; it still goes through the
 * debouncer, so tests and SIMULATE_INPUTS builds exercise the same path.
 */
#ifndef MCP23017_CORE_H
#define MCP23017_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "debounce.h"

#define MCP_REG_IODIRA  0x00U   /* IODIRB follows */
#define MCP_REG_IPOLA   0x02U   /* IPOLB  follows */
#define MCP_REG_GPPUA   0x0CU   /* GPPUB  follows */
#define MCP_REG_GPIOA   0x12U   /* GPIOB  follows */

#define MCP_BUTTON_COUNT 10U

typedef struct
{
  /** (Re)initialise the bus after an error. false = try again later. */
  bool (*reinit)(void *ctx);
  /** Start writing @p len bytes from register @p reg. false = not started. */
  bool (*start_write)(void *ctx, uint8_t reg, const uint8_t *data, uint8_t len);
  /** Start reading @p len bytes from register @p reg into @p buf. */
  bool (*start_read)(void *ctx, uint8_t reg, uint8_t *buf, uint8_t len);
  void *ctx;
} mcp_io_t;

typedef enum
{
  MCP_ST_FAULT = 0,
  MCP_ST_START_IODIR,
  MCP_ST_WAIT_IODIR,
  MCP_ST_START_GPPU,
  MCP_ST_WAIT_GPPU,
  MCP_ST_START_IPOL,
  MCP_ST_WAIT_IPOL,
  MCP_ST_START_READ,
  MCP_ST_WAIT_READ
} mcp_state_t;

typedef struct
{
  uint32_t faults;       /* entries into FAULT (errors + timeouts)  */
  uint32_t timeouts;
  uint32_t samples;      /* completed GPIO reads                    */
} mcp_stats_t;

typedef struct
{
  mcp_io_t    io;
  bool        chip_inverts;     /* IPOL used: chip returns 1 = pressed */
  uint32_t    retry_ms;
  uint32_t    timeout_ms;

  mcp_state_t state;
  uint32_t    state_since_ms;
  volatile uint8_t xfer_done;   /* set from ISR context */
  volatile uint8_t xfer_error;  /* set from ISR context */
  uint8_t     cfg_ff[2];
  uint8_t     rx[2];

  debounce_t  deb;
  bool        sim;
  uint16_t    sim_raw;
  mcp_stats_t stats;
} mcp_core_t;

void mcp_core_init(mcp_core_t *m, const mcp_io_t *io, bool chip_inverts,
                   uint32_t debounce_ms, uint32_t retry_ms, uint32_t timeout_ms,
                   uint32_t now_ms);

/** Completion callbacks (safe to call from an ISR). */
void mcp_core_on_xfer_done(mcp_core_t *m);
void mcp_core_on_xfer_error(mcp_core_t *m);

/** Advance the state machine and debounce. Call every 1 ms. Never blocks. */
void mcp_core_update(mcp_core_t *m, uint32_t now_ms);

/** Debounced mask, bits 0..9, 1 = pressed. 0 while in FAULT. */
uint16_t mcp_core_buttons(const mcp_core_t *m);

/** true while the chip is configured and being read (or simulated). */
bool mcp_core_present(const mcp_core_t *m);

void mcp_core_sim_enable(mcp_core_t *m, bool on, uint32_t now_ms);
void mcp_core_sim_set(mcp_core_t *m, uint16_t raw_pressed_mask);

/** Raw ports -> button mask (1 = pressed). */
uint16_t mcp_decode_ports(uint8_t gpioa, uint8_t gpiob, bool chip_inverts);

#endif /* MCP23017_CORE_H */
