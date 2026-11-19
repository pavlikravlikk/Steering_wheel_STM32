/**
 * @file    ffb_ui.h
 * @brief   Arm gesture and FFB status LEDs (pure, host-tested).
 *
 * Arm gesture: hold B1 (blue user button) AND both paddles (HID buttons 9 and
 * 10) together for FFB_ARM_HOLD_MS. With FFB_ARM_PINS=1 (default) two push buttons on PD0 / PD1 (to GND) count as
 * the two paddles, so the MCP23017 is not needed. It must start from "all three released"
 * (a button stuck at power-up can never arm), and fires once per hold.
 * Disarm: any new press of B1 (rising edge) while the paddles are not both
 * held. In RUNNING this stops the motor immediately.
 *
 * LEDs (free LEDs of the Discovery board):
 *   LD6 green : off = not ready, 1 Hz blink = ready to arm (precheck OK),
 *               5 Hz = PRECHECK/ARMING, solid = RUNNING
 *   LD8 orange: blink code = reason (DISARMED) or fault (FAULT):
 *               N short flashes, then a pause
 *   LD4 blue  : solid = FAULT latched (power-cycle), fast blink = gesture held
 */
#ifndef FFB_UI_H
#define FFB_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "ffb_safety.h"

typedef enum
{
  FFB_GESTURE_NONE = 0,
  FFB_GESTURE_ARM,
  FFB_GESTURE_DISARM
} ffb_gesture_event_t;

typedef struct
{
  uint32_t hold_ms;
  uint32_t since_ms;
  bool     released_seen;   /* all three released since the last fire      */
  bool     holding;
  bool     fired;
  bool     last_b1;
} ffb_gesture_t;

void ffb_gesture_init(ffb_gesture_t *g, uint32_t hold_ms);
ffb_gesture_event_t ffb_gesture_update(ffb_gesture_t *g, bool b1, bool paddle_l, bool paddle_r,
                                       uint32_t now_ms);
/* ---- B1-only arm sequence (FFB_ARM_B1_ONLY=1): tap, tap, then hold B1 ---------------------------------
 * Pure. B1 must be seen released first (a B1 held at power-up never arms); exactly FFB_B1SEQ_TAPS short taps
 * (press <= TAP_MAX, gap between presses <= GAP_MAX), then the next press held for hold_ms fires ONCE. Any
 * violation (long first press, too slow, too many taps, early release) starts over and needs a release. */
typedef enum
{
  FFB_B1SEQ_WAIT_RELEASE = 0,
  FFB_B1SEQ_IDLE,
  FFB_B1SEQ_DOWN,
  FFB_B1SEQ_GAP,
  FFB_B1SEQ_HOLD
} ffb_b1seq_state_t;

typedef struct
{
  uint32_t          hold_ms;
  ffb_b1seq_state_t state;
  uint32_t          taps;
  uint32_t          press_ms;     /* start of the current press            */
  uint32_t          release_ms;   /* start of the current gap              */
} ffb_b1seq_t;

void ffb_b1seq_init(ffb_b1seq_t *q, uint32_t hold_ms);
/** Call every tick with the debounced B1. true exactly once, when the final hold completes. */
bool ffb_b1seq_update(ffb_b1seq_t *q, bool b1, uint32_t now_ms);
/** true while the final hold is running (LD4 blinks fast; set-centre is blocked meanwhile). */
bool ffb_b1seq_pending(const ffb_b1seq_t *q);

/** Two dedicated arm buttons (push buttons from PD0 / PD1 to GND, no MCP23017 needed) act as the left /
 *  right paddle for the arm gesture: their "pressed" state is OR-ed into the paddle bits of the HID
 *  button word. Pure. Nothing else in @p buttons changes; with no pin pressed the word is unchanged,
 *  so a missing or unplugged button can never arm (the gesture needs ALL of B1 + both). */
uint16_t ffb_buttons_with_arm_pins(uint16_t buttons, bool left_pressed, bool right_pressed);

/** true while the full combination is being held (before it fires). */
bool ffb_gesture_active(const ffb_gesture_t *g);

#define FFB_LED_GREEN  0x1U
#define FFB_LED_ORANGE 0x2U
#define FFB_LED_BLUE   0x4U

/** LED bitmask for the current status. @p ready = DISARMED precheck OK. */
uint8_t ffb_led_pattern(ffb_state_t state, ffb_reason_t reason, ffb_fault_t fault, bool ready,
                        bool gesture_active, uint32_t now_ms);

/** Blink code helper: true while flash k (1..count) is lit. Period:
 *  count * 500 ms + 1500 ms pause. */
bool ffb_blink_code(uint32_t count, uint32_t now_ms);

#endif /* FFB_UI_H */
