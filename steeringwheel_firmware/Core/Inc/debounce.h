/**
 * @file    debounce.h
 * @brief   Time-based multi-bit debouncer and long-press detector (pure).
 *
 * A bit changes its stable state only after the raw input has been steady
 * at the new level for at least delay_ms (wall time, independent of the
 * sampling rate).
 */
#ifndef DEBOUNCE_H
#define DEBOUNCE_H

#include <stdbool.h>
#include <stdint.h>

#define DEBOUNCE_MAX_BITS 16U

typedef struct
{
  uint16_t stable;                          /* debounced state, 1 = active  */
  uint16_t last_raw;
  uint16_t mask;
  uint32_t delay_ms;
  uint32_t raw_since_ms[DEBOUNCE_MAX_BITS]; /* when the raw bit last changed */
} debounce_t;

void debounce_init(debounce_t *d, uint32_t nbits, uint32_t delay_ms, uint32_t now_ms);

/** Feed one raw sample (1 = active); returns the debounced state. */
uint16_t debounce_update(debounce_t *d, uint16_t raw, uint32_t now_ms);

/** Force everything to released (e.g. after a bus error). */
void debounce_reset(debounce_t *d, uint32_t now_ms);

/* ---------------------------------------------------------------------- */
typedef struct
{
  uint32_t hold_ms;
  uint32_t pressed_since_ms;
  bool     pressed;
  bool     fired;
} hold_detector_t;

void hold_init(hold_detector_t *h, uint32_t hold_ms);

/** Feed the debounced state; returns true exactly once per press, when the
 *  button has been held for hold_ms. */
bool hold_update(hold_detector_t *h, bool pressed, uint32_t now_ms);

#endif /* DEBOUNCE_H */
