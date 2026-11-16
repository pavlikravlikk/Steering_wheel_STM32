/**
 * @file    ffb_ui.c
 * @brief   Arm gesture detector and LED patterns. Pure.
 */
#include "ffb_ui.h"

#include <string.h>

void ffb_gesture_init(ffb_gesture_t *g, uint32_t hold_ms)
{
  memset(g, 0, sizeof(*g));
  g->hold_ms = hold_ms;
  g->released_seen = false;      /* must see everything released first */
}

ffb_gesture_event_t ffb_gesture_update(ffb_gesture_t *g, bool b1, bool paddle_l, bool paddle_r,
                                       uint32_t now_ms)
{
  const bool all = b1 && paddle_l && paddle_r;
  const bool none = !b1 && !paddle_l && !paddle_r;
  const bool b1_edge = b1 && !g->last_b1;
  ffb_gesture_event_t ev = FFB_GESTURE_NONE;

  g->last_b1 = b1;
  if (none)
  {
    g->released_seen = true;
    g->holding = false;
    g->fired = false;
  }
  if (b1_edge && !(paddle_l && paddle_r))
  {
    ev = FFB_GESTURE_DISARM;
  }
  if (all && g->released_seen && !g->fired)
  {
    if (!g->holding)
    {
      g->holding = true;
      g->since_ms = now_ms;
    }
    else if ((now_ms - g->since_ms) >= g->hold_ms)
    {
      g->fired = true;
      g->holding = false;
      g->released_seen = false;  /* next arm needs a full release first */
      ev = FFB_GESTURE_ARM;
    }
  }
  else if (!all)
  {
    g->holding = false;          /* releasing any one cancels the hold */
  }
  return ev;
}

bool ffb_gesture_active(const ffb_gesture_t *g)
{
  return g->holding;
}

bool ffb_blink_code(uint32_t count, uint32_t now_ms)
{
  uint32_t period;
  uint32_t t;

  if (count == 0U)
  {
    return false;
  }
  if (count > 20U)
  {
    count = 20U;
  }
  period = (count * 500U) + 1500U;
  t = now_ms % period;
  if (t >= count * 500U)
  {
    return false;                /* pause */
  }
  return (t % 500U) < 200U;      /* 200 ms on, 300 ms off */
}

uint8_t ffb_led_pattern(ffb_state_t state, ffb_reason_t reason, ffb_fault_t fault, bool ready,
                        bool gesture_active, uint32_t now_ms)
{
  uint8_t leds = 0U;

  switch (state)
  {
    case FFB_STATE_DISARMED:
      if (ready && ((now_ms % 1000U) < 500U))
      {
        leds |= FFB_LED_GREEN;
      }
      if (!ready && ffb_blink_code((uint32_t)reason, now_ms))
      {
        leds |= FFB_LED_ORANGE;
      }
      break;
    case FFB_STATE_PRECHECK:
    case FFB_STATE_ARMING:
      if ((now_ms % 200U) < 100U)
      {
        leds |= FFB_LED_GREEN;
      }
      break;
    case FFB_STATE_RUNNING:
      leds |= FFB_LED_GREEN;
      break;
    case FFB_STATE_FAULT:
    default:
      leds |= FFB_LED_BLUE;
      if (ffb_blink_code((uint32_t)((fault == FFB_FAULT_NONE) ? FFB_FAULT_INTERNAL : fault), now_ms))
      {
        leds |= FFB_LED_ORANGE;
      }
      return leds;
  }
  if (gesture_active && ((now_ms % 100U) < 50U))
  {
    leds |= FFB_LED_BLUE;
  }
  return leds;
}
