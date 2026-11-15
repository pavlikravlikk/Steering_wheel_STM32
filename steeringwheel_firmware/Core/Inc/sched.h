/**
 * @file    sched.h
 * @brief   Minimal cooperative scheduler helper (pure, wrap-around safe).
 */
#ifndef SCHED_H
#define SCHED_H

#include <stdbool.h>
#include <stdint.h>

/** true when @p period_ms has elapsed since *last_ms; then *last_ms = now.
 *  Missed periods are not caught up (no bursts after a stall). */
static inline bool sched_due(uint32_t *last_ms, uint32_t period_ms, uint32_t now_ms)
{
  if ((now_ms - *last_ms) >= period_ms)
  {
    *last_ms = now_ms;
    return true;
  }
  return false;
}

#endif /* SCHED_H */
