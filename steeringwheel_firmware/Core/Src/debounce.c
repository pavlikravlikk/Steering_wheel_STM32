/**
 * @file    debounce.c
 * @brief   Time-based debouncer. Must not include any HAL header.
 */
#include "debounce.h"

void debounce_init(debounce_t *d, uint32_t nbits, uint32_t delay_ms, uint32_t now_ms)
{
  if (nbits > DEBOUNCE_MAX_BITS)
  {
    nbits = DEBOUNCE_MAX_BITS;
  }
  d->mask = (nbits >= 16U) ? 0xFFFFU : (uint16_t)((1UL << nbits) - 1UL);
  d->delay_ms = delay_ms;
  debounce_reset(d, now_ms);
}

void debounce_reset(debounce_t *d, uint32_t now_ms)
{
  d->stable = 0U;
  d->last_raw = 0U;
  for (uint32_t i = 0U; i < DEBOUNCE_MAX_BITS; i++)
  {
    d->raw_since_ms[i] = now_ms;
  }
}

uint16_t debounce_update(debounce_t *d, uint16_t raw, uint32_t now_ms)
{
  raw &= d->mask;
  const uint16_t changed = (uint16_t)(raw ^ d->last_raw);

  for (uint32_t i = 0U; i < DEBOUNCE_MAX_BITS; i++)
  {
    const uint16_t bit = (uint16_t)(1U << i);

    if ((d->mask & bit) == 0U)
    {
      continue;
    }
    if ((changed & bit) != 0U)
    {
      d->raw_since_ms[i] = now_ms;        /* raw level changed: restart */
    }
    if ((((raw ^ d->stable) & bit) != 0U)
        && ((now_ms - d->raw_since_ms[i]) >= d->delay_ms))
    {
      d->stable ^= bit;                   /* steady long enough: accept */
    }
  }
  d->last_raw = raw;
  return d->stable;
}

void hold_init(hold_detector_t *h, uint32_t hold_ms)
{
  h->hold_ms = hold_ms;
  h->pressed_since_ms = 0U;
  h->pressed = false;
  h->fired = false;
}

bool hold_update(hold_detector_t *h, bool pressed, uint32_t now_ms)
{
  if (!pressed)
  {
    h->pressed = false;
    h->fired = false;
    return false;
  }
  if (!h->pressed)
  {
    h->pressed = true;
    h->pressed_since_ms = now_ms;
  }
  if (!h->fired && ((now_ms - h->pressed_since_ms) >= h->hold_ms))
  {
    h->fired = true;
    return true;
  }
  return false;
}
