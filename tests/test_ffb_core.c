/* Force feedback, pure modules: command allowlist, safety state machine
 * (every transition and fault path), precheck, torque pipeline, gesture/LEDs. */
#include "test_runner.h"

#include <string.h>

#include "ffb_cmd.h"
#include "ffb_config.h"
#include "ffb_precheck.h"
#include "ffb_safety.h"
#include "ffb_torque.h"
#include "ffb_ui.h"

/* ======================================================================= */
/* ffb_cmd                                                                  */
/* ======================================================================= */
TEST(test_cmd_formatting)
{
  char b[FFB_CMD_MAX];
  CHECK(ffb_fmt_torque(b, sizeof(b), 0, 0.0f) > 0);
  CHECK(strcmp(b, "w axis0.controller.input_torque 0.0000\n") == 0);
  ffb_fmt_torque(b, sizeof(b), 0, -0.04f);
  CHECK(strcmp(b, "w axis0.controller.input_torque -0.0400\n") == 0);
  ffb_fmt_torque(b, sizeof(b), 1, 0.123456f);
  CHECK(strcmp(b, "w axis1.controller.input_torque 0.1235\n") == 0);
  ffb_fmt_torque(b, sizeof(b), 0, -0.00001f);           /* no "-0.0000" */
  CHECK(strcmp(b, "w axis0.controller.input_torque 0.0000\n") == 0);
  ffb_fmt_torque(b, sizeof(b), 0, 1000.0f);              /* clamped */
  CHECK(strcmp(b, "w axis0.controller.input_torque 2.0000\n") == 0);
  ffb_fmt_torque(b, sizeof(b), 0, NAN);                  /* NaN -> 0 */
  CHECK(strcmp(b, "w axis0.controller.input_torque 0.0000\n") == 0);
  CHECK(ffb_fmt_state(b, sizeof(b), 0, 8) > 0);
  CHECK(strcmp(b, "w axis0.requested_state 8\n") == 0);
  CHECK(ffb_fmt_state(b, sizeof(b), 0, 1) > 0);
  CHECK(strcmp(b, "w axis0.requested_state 1\n") == 0);
  CHECK_EQ_INT(ffb_fmt_state(b, sizeof(b), 0, 3), 0);    /* calibration: never */
  CHECK_EQ_INT(ffb_fmt_state(b, sizeof(b), 0, 7), 0);
  CHECK(ffb_fmt_feed(b, sizeof(b), 0) == 4);
  CHECK(strcmp(b, "u 0\n") == 0);
  CHECK_EQ_INT(ffb_fmt_torque(b, 10, 0, 0.0f), 0);       /* buffer too small */
}

TEST(test_cmd_allowlist_accepts_only_three_kinds)
{
  char b[FFB_CMD_MAX];
  for (float t = -2.0f; t <= 2.0f; t += 0.0137f)
  {
    ffb_fmt_torque(b, sizeof(b), 0, t);
    CHECK(ffb_cmd_is_allowed(b, 0));
    CHECK(!ffb_cmd_is_allowed(b, 1));                    /* wrong axis */
  }
  CHECK(ffb_cmd_is_allowed("w axis0.requested_state 1\n", 0));
  CHECK(ffb_cmd_is_allowed("w axis0.requested_state 8\n", 0));
  CHECK(ffb_cmd_is_allowed("u 0\n", 0));
}

TEST(test_cmd_allowlist_rejects_config_and_everything_else)
{
  static const char *const bad[] = {
    "w axis0.requested_state 3\n", "w axis0.requested_state 7\n", "w axis0.requested_state 4\n",
    "w axis0.requested_state 88\n", "w axis0.requested_state 8", "w axis0.requested_state 8 \n",
    "w axis0.motor.config.current_lim 50\n", "w axis0.controller.config.vel_limit 100\n",
    "w axis0.controller.config.control_mode 3\n", "w axis0.config.enable_watchdog 0\n",
    "w axis0.config.watchdog_timeout 10\n", "w axis0.config.startup_closed_loop_control 1\n",
    "w axis0.motor.config.torque_constant 1\n", "w axis0.controller.input_pos 1.0000\n",
    "w axis0.controller.input_vel 1.0000\n", "ss\n", "se\n", "sr\n", "sc\n", "sb\n",
    "c 0 1.0\n", "p 0 1 0 0\n", "v 0 1\n", "t 0 1\n", "q 0 1\n", "u 1\n", "u 0", "u 00\n",
    "w axis0.controller.input_torque 2.1000\n",          /* above the cap */
    "w axis0.controller.input_torque 1e3\n", "w axis0.controller.input_torque nan\n",
    "w axis0.controller.input_torque 0.000\n", "w axis0.controller.input_torque 0.00000\n",
    "w axis0.controller.input_torque .5000\n", "w axis0.controller.input_torque 100.0000\n",
    "w axis0.controller.input_torque 0.1000\nw axis0.requested_state 3\n",
    "w axis0.controller.input_torque +0.1000\n", "w axis0.controller.input_torque  0.1000\n",
    "w axis0.controller.input_torque\n", "r vbus_voltage\n", "f 0\n", "", "\n",
    "w axis0.controller.input_torque 0.1000\r\n",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
  {
    if (ffb_cmd_is_allowed(bad[i], 0))
    {
      printf("    accepted: %s", bad[i]);
      CHECK(0);
    }
  }
  CHECK(!ffb_cmd_is_allowed(NULL, 0));
}

/* ======================================================================= */
/* ffb_safety: transition table                                             */
/* ======================================================================= */
static void sm_make(ffb_sm_t *sm, bool arm_allowed)
{
  ffb_sm_cfg_t c;
  ffb_sm_default_cfg(&c, arm_allowed);
  c.feed_when_disarmed = false;
  ffb_sm_init(sm, &c, 0);
}

static ffb_sm_in_t good_in(uint32_t now)
{
  ffb_sm_in_t in;
  ffb_sm_in_defaults(&in, now);
  in.position_fresh = true;
  in.precheck_reason = FFB_REASON_OK;
  return in;
}

static bool any_write(const ffb_sm_out_t *o)
{
  return o->send_torque || o->send_closed_loop || o->send_idle || o->feed_watchdog;
}

/* Drive the SM from DISARMED to RUNNING; returns the time reached. */
static uint32_t sm_to_running(ffb_sm_t *sm)
{
  ffb_sm_out_t o;
  ffb_sm_in_t in = good_in(10);
  in.arm_request = true;
  ffb_sm_step(sm, &in, &o);
  in = good_in(20);
  in.precheck_complete = true;
  ffb_sm_step(sm, &in, &o);
  in = good_in(20 + FFB_ARMING_MIN_MS);
  in.closed_loop_confirmed = true;
  ffb_sm_step(sm, &in, &o);
  return 20 + FFB_ARMING_MIN_MS;
}

TEST(test_sm_disarmed_sends_nothing)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  sm_make(&sm, true);
  for (uint32_t t = 0; t < 5000; t += 5)
  {
    ffb_sm_in_t in = good_in(t);
    in.torque_amps = 0.9f;
    in.precheck_complete = true;     /* ignored outside PRECHECK */
    in.closed_loop_confirmed = true; /* ignored outside ARMING   */
    ffb_sm_step(&sm, &in, &o);
    CHECK(!any_write(&o));
    CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  }
}

TEST(test_sm_arm_not_allowed_build_never_leaves_disarmed)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  sm_make(&sm, false);
  CHECK_EQ_INT(sm.reason, FFB_REASON_ARM_NOT_ALLOWED);
  for (uint32_t t = 0; t < 2000; t += 5)
  {
    ffb_sm_in_t in = good_in(t);
    in.arm_request = true;
    in.precheck_complete = true;
    in.closed_loop_confirmed = true;
    ffb_sm_step(&sm, &in, &o);
    CHECK(!any_write(&o));
    CHECK(!o.start_precheck);
    CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  }
}

TEST(test_sm_happy_path_and_write_order)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  sm_make(&sm, true);
  in = good_in(10);
  in.arm_request = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_PRECHECK);
  CHECK(o.start_precheck);
  CHECK(!any_write(&o));                       /* PRECHECK is read-only */
  in = good_in(15);
  ffb_sm_step(&sm, &in, &o);                   /* reads not complete yet */
  CHECK_EQ_INT(sm.state, FFB_STATE_PRECHECK);
  CHECK(!any_write(&o));
  in = good_in(20);
  in.precheck_complete = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_ARMING);
  CHECK(o.send_torque && o.torque_amps == 0.0f && o.send_closed_loop && o.feed_watchdog);
  CHECK(!o.send_idle);
  in = good_in(25);
  in.closed_loop_confirmed = true;             /* too early: min arming time */
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_ARMING);
  CHECK(o.send_torque && o.torque_amps == 0.0f && !o.send_closed_loop);
  in = good_in(20 + FFB_ARMING_MIN_MS);
  in.closed_loop_confirmed = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_RUNNING);
  in = good_in(200);
  in.torque_amps = 0.3f;
  ffb_sm_step(&sm, &in, &o);
  CHECK(o.send_torque && o.feed_watchdog);
  CHECK_NEAR(o.torque_amps, 0.3, 1e-6);
}

TEST(test_sm_precheck_failure_returns_disarmed_with_reason)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  for (int r = 1; r < (int)FFB_REASON_COUNT; r++)
  {
    sm_make(&sm, true);
    in = good_in(10);
    in.arm_request = true;
    ffb_sm_step(&sm, &in, &o);
    in = good_in(20);
    in.precheck_complete = true;
    in.precheck_reason = (ffb_reason_t)r;
    ffb_sm_step(&sm, &in, &o);
    CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
    CHECK_EQ_INT(sm.reason, r);
    CHECK(!any_write(&o));
  }
}

TEST(test_sm_precheck_timeout_and_stale_position)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  sm_make(&sm, true);
  in = good_in(0);
  in.arm_request = true;
  ffb_sm_step(&sm, &in, &o);
  in = good_in(FFB_PRECHECK_TIMEOUT_MS);
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(sm.reason, FFB_REASON_READ_TIMEOUT);

  sm_make(&sm, true);
  in = good_in(0);
  in.arm_request = true;
  ffb_sm_step(&sm, &in, &o);
  in = good_in(10);
  in.precheck_complete = true;
  in.position_fresh = false;                   /* checks OK but data gone */
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(sm.reason, FFB_REASON_POSITION_STALE);
  CHECK(!any_write(&o));
}

TEST(test_sm_disarm_during_precheck_and_arming)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  sm_make(&sm, true);
  in = good_in(0);
  in.arm_request = true;
  ffb_sm_step(&sm, &in, &o);
  in = good_in(5);
  in.disarm_request = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  CHECK(!any_write(&o));

  sm_make(&sm, true);
  in = good_in(0);
  in.arm_request = true;
  ffb_sm_step(&sm, &in, &o);
  in = good_in(5);
  in.precheck_complete = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_ARMING);
  in = good_in(10);
  in.disarm_request = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  CHECK(o.urgent && o.send_torque && o.torque_amps == 0.0f && o.send_idle);
}

TEST(test_sm_user_disarm_from_running_stops_and_resends_idle)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  uint32_t t, idles = 0;
  sm_make(&sm, true);
  t = sm_to_running(&sm);
  in = good_in(t + 5);
  in.disarm_request = true;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(sm.fault, FFB_FAULT_NONE);      /* not latched */
  CHECK(o.urgent && o.send_torque && o.torque_amps == 0.0f && o.send_idle && !o.feed_watchdog);
  for (uint32_t k = t + 10; k < t + 2000; k += 5)
  {
    in = good_in(k);
    ffb_sm_step(&sm, &in, &o);
    if (o.send_idle) { idles++; CHECK(o.send_torque && o.torque_amps == 0.0f); }
    CHECK(!o.send_closed_loop && !o.feed_watchdog);
  }
  CHECK_EQ_INT(idles, FFB_DISARM_RESEND_COUNT);
}

/* Every fault path from RUNNING: input mutation -> expected latched fault. */
typedef void (*mutate_fn)(ffb_sm_in_t *in);
static void m_stale(ffb_sm_in_t *in)    { in->position_fresh = false; }
static void m_odrive(ffb_sm_in_t *in)   { in->monitor_fault = FFB_FAULT_ODRIVE_ERROR; }
static void m_vbus(ffb_sm_in_t *in)     { in->monitor_fault = FFB_FAULT_VBUS_RANGE; }
static void m_readf(ffb_sm_in_t *in)    { in->monitor_fault = FFB_FAULT_READ_FAILED; }
static void m_uart(ffb_sm_in_t *in)     { in->consec_timeouts = FFB_MAX_CONSEC_TIMEOUTS; }
static void m_tx(ffb_sm_in_t *in)       { in->consec_tx_failures = FFB_MAX_TX_FAILURES; }
static void m_nan(ffb_sm_in_t *in)      { in->torque_amps = NAN; }
static void m_inf(ffb_sm_in_t *in)      { in->torque_amps = INFINITY; }
static void m_big(ffb_sm_in_t *in)      { in->torque_amps = 2.0f * FFB_MAX_AMPS; }
static void m_speed(ffb_sm_in_t *in)    { in->wheel_vel_turns_s = FFB_OVERSPEED_TURNS_S * 1.01f; }
static void m_speedn(ffb_sm_in_t *in)   { in->wheel_vel_turns_s = -FFB_OVERSPEED_TURNS_S * 1.01f; }
static void m_speednan(ffb_sm_in_t *in) { in->wheel_vel_turns_s = NAN; }
static void m_overrun(ffb_sm_in_t *in)  { in->cycle_overrun = true; }

static const struct { mutate_fn fn; ffb_fault_t fault; const char *name; } k_faults[] = {
  { m_stale, FFB_FAULT_POSITION_STALE, "stale" },     { m_odrive, FFB_FAULT_ODRIVE_ERROR, "odrive" },
  { m_vbus, FFB_FAULT_VBUS_RANGE, "vbus" },           { m_readf, FFB_FAULT_READ_FAILED, "read" },
  { m_uart, FFB_FAULT_UART_TIMEOUT, "uart" },         { m_tx, FFB_FAULT_WATCHDOG_FEED_FAILED, "tx" },
  { m_nan, FFB_FAULT_TORQUE_INVALID, "nan" },         { m_inf, FFB_FAULT_TORQUE_INVALID, "inf" },
  { m_big, FFB_FAULT_TORQUE_INVALID, "big" },         { m_speed, FFB_FAULT_OVERSPEED, "speed+" },
  { m_speedn, FFB_FAULT_OVERSPEED, "speed-" },        { m_speednan, FFB_FAULT_OVERSPEED, "speednan" },
  { m_overrun, FFB_FAULT_LOOP_OVERRUN, "overrun" },
};

TEST(test_sm_every_fault_path_from_running_latches)
{
  for (size_t i = 0; i < sizeof(k_faults) / sizeof(k_faults[0]); i++)
  {
    ffb_sm_t sm;
    ffb_sm_out_t o;
    ffb_sm_in_t in;
    uint32_t t, resends = 0;
    sm_make(&sm, true);
    t = sm_to_running(&sm);
    CHECK_EQ_INT(sm.state, FFB_STATE_RUNNING);
    in = good_in(t + 5);
    in.torque_amps = 0.2f;
    k_faults[i].fn(&in);
    ffb_sm_step(&sm, &in, &o);
    if (sm.state != FFB_STATE_FAULT || sm.fault != k_faults[i].fault)
    {
      printf("    fault path '%s': state %d fault %d\n", k_faults[i].name, sm.state, sm.fault);
    }
    CHECK_EQ_INT(sm.state, FFB_STATE_FAULT);
    CHECK_EQ_INT(sm.fault, k_faults[i].fault);
    /* torque 0 first, then IDLE, ahead of anything queued; no feed */
    CHECK(o.urgent && o.send_torque && o.torque_amps == 0.0f && o.send_idle);
    CHECK(!o.send_closed_loop && !o.feed_watchdog);
    /* latched: nothing brings it back, it only re-sends the stop */
    for (uint32_t k = t + 10; k < t + 3000; k += 5)
    {
      in = good_in(k);
      in.arm_request = (k % 100) == 0;
      in.disarm_request = (k % 150) == 0;
      in.precheck_complete = true;
      in.closed_loop_confirmed = true;
      in.torque_amps = 0.5f;
      ffb_sm_step(&sm, &in, &o);
      CHECK_EQ_INT(sm.state, FFB_STATE_FAULT);
      CHECK(!o.send_closed_loop && !o.feed_watchdog);
      if (o.send_torque) CHECK(o.torque_amps == 0.0f);
      if (o.send_idle) resends++;
    }
    CHECK(resends >= 5);
  }
}

TEST(test_sm_fault_paths_during_arming)
{
  for (size_t i = 0; i < sizeof(k_faults) / sizeof(k_faults[0]); i++)
  {
    ffb_sm_t sm;
    ffb_sm_out_t o;
    ffb_sm_in_t in;
    if (k_faults[i].fault == FFB_FAULT_TORQUE_INVALID) continue;  /* torque unused in ARMING */
    sm_make(&sm, true);
    in = good_in(0); in.arm_request = true; ffb_sm_step(&sm, &in, &o);
    in = good_in(5); in.precheck_complete = true; ffb_sm_step(&sm, &in, &o);
    CHECK_EQ_INT(sm.state, FFB_STATE_ARMING);
    in = good_in(10);
    k_faults[i].fn(&in);
    ffb_sm_step(&sm, &in, &o);
    CHECK_EQ_INT(sm.state, FFB_STATE_FAULT);
    CHECK_EQ_INT(sm.fault, k_faults[i].fault);
    CHECK(o.send_idle && o.send_torque && o.torque_amps == 0.0f);
  }
}

TEST(test_sm_arming_timeout)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in;
  sm_make(&sm, true);
  in = good_in(0); in.arm_request = true; ffb_sm_step(&sm, &in, &o);
  in = good_in(5); in.precheck_complete = true; ffb_sm_step(&sm, &in, &o);
  for (uint32_t t = 10; t < 5 + FFB_ARMING_TIMEOUT_MS; t += 5)
  {
    in = good_in(t);
    ffb_sm_step(&sm, &in, &o);
    CHECK_EQ_INT(sm.state, FFB_STATE_ARMING);
    CHECK(o.send_torque && o.torque_amps == 0.0f && o.feed_watchdog);
  }
  in = good_in(5 + FFB_ARMING_TIMEOUT_MS);
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_FAULT);
  CHECK_EQ_INT(sm.fault, FFB_FAULT_ARMING_TIMEOUT);
}

TEST(test_sm_watchdog_fed_every_cycle_while_running)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  uint32_t t;
  sm_make(&sm, true);
  t = sm_to_running(&sm);
  for (uint32_t k = t + 5; k < t + 1000; k += FFB_COMMAND_PERIOD_MS)
  {
    ffb_sm_in_t in = good_in(k);
    in.torque_amps = 0.1f;
    ffb_sm_step(&sm, &in, &o);
    CHECK(o.feed_watchdog);
    CHECK(o.send_torque);
  }
}

TEST(test_sm_corrupted_state_goes_to_fault)
{
  ffb_sm_t sm;
  ffb_sm_out_t o;
  ffb_sm_in_t in = good_in(10);
  sm_make(&sm, true);
  sm.state = (ffb_state_t)77;
  ffb_sm_step(&sm, &in, &o);
  CHECK_EQ_INT(sm.state, FFB_STATE_FAULT);
  CHECK_EQ_INT(sm.fault, FFB_FAULT_INTERNAL);
  CHECK(o.send_idle);
}

TEST(test_sm_names)
{
  CHECK(strcmp(ffb_state_name(FFB_STATE_RUNNING), "RUNNING") == 0);
  CHECK(strcmp(ffb_reason_name(FFB_REASON_VBUS_RANGE), "VBUS_RANGE") == 0);
  CHECK(strcmp(ffb_fault_name(FFB_FAULT_OVERSPEED), "OVERSPEED") == 0);
  CHECK(strcmp(ffb_state_name((ffb_state_t)99), "?") == 0);
}

/* ======================================================================= */
/* ffb_precheck                                                             */
/* ======================================================================= */
static void pc_fill_ok(ffb_pc_table_t *t, uint32_t now)
{
  ffb_pc_store(t, FFB_PC_ENABLE_WATCHDOG, 1.0f, now);
  ffb_pc_store(t, FFB_PC_WATCHDOG_TIMEOUT, 0.25f, now);
  ffb_pc_store(t, FFB_PC_CONTROL_MODE, 1.0f, now);
  ffb_pc_store(t, FFB_PC_CURRENT_LIM, 4.0f, now);
  ffb_pc_store(t, FFB_PC_VEL_LIMIT, 2.0f, now);
  ffb_pc_store(t, FFB_PC_TORQUE_CONSTANT, 0.04f, now);
  ffb_pc_store(t, FFB_PC_MOTOR_CALIBRATED, 1.0f, now);
  ffb_pc_store(t, FFB_PC_ENCODER_READY, 1.0f, now);
  ffb_pc_store(t, FFB_PC_AXIS_ERROR, 0.0f, now);
  ffb_pc_store(t, FFB_PC_MOTOR_ERROR, 0.0f, now);
  ffb_pc_store(t, FFB_PC_ENCODER_ERROR, 0.0f, now);
  ffb_pc_store(t, FFB_PC_VBUS, 24.0f, now);
  ffb_pc_store(t, FFB_PC_CURRENT_STATE, 1.0f, now);
}

static ffb_pc_context_t ctx_ok(void)
{
  ffb_pc_context_t c = { true, false, 0U, true, true, true };
  return c;
}

TEST(test_precheck_paths_are_readonly_and_axis_patched)
{
  ffb_pc_table_t t;
  ffb_pc_init(&t, 1, true);
  CHECK(strcmp(ffb_pc_path(&t, FFB_PC_CURRENT_LIM), "axis1.motor.config.current_lim") == 0);
  CHECK(strcmp(ffb_pc_path(&t, FFB_PC_VBUS), "vbus_voltage") == 0);
  ffb_pc_init(&t, 0, true);
  CHECK(strcmp(ffb_pc_path(&t, FFB_PC_ENABLE_WATCHDOG), "axis0.config.enable_watchdog") == 0);
}

TEST(test_precheck_all_ok)
{
  ffb_pc_table_t t;
  ffb_limits_t lim;
  ffb_pc_context_t c = ctx_ok();
  ffb_pc_init(&t, 0, true);
  ffb_limits_default(&lim);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 0, 1000), FFB_REASON_READ_TIMEOUT);
  pc_fill_ok(&t, 100);
  CHECK(ffb_pc_complete(&t, 100, 1000));
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 100, 1000), FFB_REASON_OK);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 1200, 1000), FFB_REASON_READ_TIMEOUT);  /* old */
}

/* Mutation-style: each single bad value must give its own reason. */
TEST(test_precheck_each_condition)
{
  static const struct { ffb_pc_item_t item; float bad; ffb_reason_t want; } cases[] = {
    { FFB_PC_ENABLE_WATCHDOG, 0.0f, FFB_REASON_WATCHDOG_DISABLED },
    { FFB_PC_WATCHDOG_TIMEOUT, 0.0f, FFB_REASON_WATCHDOG_TIMEOUT_RANGE },
    { FFB_PC_WATCHDOG_TIMEOUT, 2.0f, FFB_REASON_WATCHDOG_TIMEOUT_RANGE },
    { FFB_PC_CONTROL_MODE, 3.0f, FFB_REASON_CONTROL_MODE },
    { FFB_PC_CONTROL_MODE, 2.0f, FFB_REASON_CONTROL_MODE },
    { FFB_PC_CURRENT_LIM, 12.0f, FFB_REASON_CURRENT_LIM_HIGH },
    { FFB_PC_CURRENT_LIM, NAN, FFB_REASON_CURRENT_LIM_HIGH },
    { FFB_PC_VEL_LIMIT, 50.0f, FFB_REASON_VEL_LIMIT_HIGH },
    { FFB_PC_TORQUE_CONSTANT, 0.0f, FFB_REASON_TORQUE_CONSTANT_RANGE },
    { FFB_PC_TORQUE_CONSTANT, 5.0f, FFB_REASON_TORQUE_CONSTANT_RANGE },
    { FFB_PC_MOTOR_CALIBRATED, 0.0f, FFB_REASON_MOTOR_NOT_CALIBRATED },
    { FFB_PC_ENCODER_READY, 0.0f, FFB_REASON_ENCODER_NOT_READY },
    { FFB_PC_AXIS_ERROR, 2048.0f, FFB_REASON_AXIS_ERROR },
    { FFB_PC_MOTOR_ERROR, 1.0f, FFB_REASON_MOTOR_ERROR },
    { FFB_PC_ENCODER_ERROR, 2.0f, FFB_REASON_ENCODER_ERROR },
    { FFB_PC_VBUS, 31.0f, FFB_REASON_VBUS_RANGE },
    { FFB_PC_VBUS, 12.0f, FFB_REASON_VBUS_RANGE },
    { FFB_PC_CURRENT_STATE, 8.0f, FFB_REASON_NOT_IDLE },   /* already closed loop */
    { FFB_PC_CURRENT_STATE, 7.0f, FFB_REASON_NOT_IDLE },   /* calibrating         */
  };
  ffb_limits_t lim;
  ffb_pc_context_t c = ctx_ok();
  ffb_limits_default(&lim);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    ffb_pc_table_t t;
    ffb_pc_init(&t, 0, true);
    pc_fill_ok(&t, 10);
    ffb_pc_store(&t, cases[i].item, cases[i].bad, 10);
    CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), cases[i].want);
  }
}

TEST(test_precheck_context_and_failed_reads)
{
  ffb_pc_table_t t;
  ffb_limits_t lim;
  ffb_pc_context_t c;
  ffb_limits_default(&lim);
  ffb_pc_init(&t, 0, true);
  pc_fill_ok(&t, 10);
  c = ctx_ok(); c.arm_allowed_build = false;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_ARM_NOT_ALLOWED);
  c = ctx_ok(); c.simulation_build = true;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_SIMULATION_BUILD);
  c = ctx_ok(); c.profile_safety_tbd = 3;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_PROFILE_TBD);
  c.require_measured = false;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_OK);
  c = ctx_ok(); c.position_fresh = false;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_POSITION_STALE);
  c = ctx_ok(); c.wheel_in_range = false;                  /* wheel already beyond +-lock/2 */
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_WHEEL_OUT_OF_RANGE);
  c.position_fresh = false;                                /* stale position is reported first */
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_POSITION_STALE);
  c = ctx_ok();
  ffb_pc_store_failed(&t, FFB_PC_VEL_LIMIT);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_READ_ERROR);
  ffb_pc_invalidate(&t);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_READ_TIMEOUT);
  CHECK(!ffb_pc_complete(&t, 10, 1000));
}

TEST(test_precheck_read_order_and_monitor_set)
{
  ffb_pc_table_t t;
  int seen[FFB_PC_COUNT] = {0};
  ffb_pc_init(&t, 0, false);         /* without current_state */
  for (int k = 0; k < FFB_PC_COUNT - 1; k++)
  {
    ffb_pc_item_t it = ffb_pc_next(&t);
    CHECK(it != FFB_PC_CURRENT_STATE);
    ffb_pc_store(&t, it, 0.0f, 1);
    seen[it]++;
  }
  for (int k = 0; k < FFB_PC_COUNT - 1; k++) CHECK_EQ_INT(seen[k], 1);
  for (int k = 0; k < 20; k++)
  {
    ffb_pc_item_t it = ffb_pc_next_monitor(&t);
    CHECK(it == FFB_PC_AXIS_ERROR || it == FFB_PC_MOTOR_ERROR || it == FFB_PC_ENCODER_ERROR
          || it == FFB_PC_VBUS);
  }
}

TEST(test_monitor_evaluate)
{
  ffb_pc_table_t t;
  ffb_limits_t lim;
  ffb_limits_default(&lim);
  ffb_pc_init(&t, 0, true);
  pc_fill_ok(&t, 100);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, false), FFB_FAULT_NONE);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, true), FFB_FAULT_ODRIVE_ERROR); /* state 1 */
  ffb_pc_store(&t, FFB_PC_CURRENT_STATE, 8.0f, 100);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, true), FFB_FAULT_NONE);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 600, 400, true), FFB_FAULT_READ_FAILED);   /* old */
  ffb_pc_store(&t, FFB_PC_MOTOR_ERROR, 16.0f, 100);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, true), FFB_FAULT_ODRIVE_ERROR);
  pc_fill_ok(&t, 100);
  ffb_pc_store(&t, FFB_PC_CURRENT_STATE, 8.0f, 100);
  ffb_pc_store(&t, FFB_PC_VBUS, 29.0f, 100);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, true), FFB_FAULT_VBUS_RANGE);
  ffb_pc_store_failed(&t, FFB_PC_VBUS);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 100, 400, true), FFB_FAULT_READ_FAILED);
}

/* ======================================================================= */
/* ffb_torque                                                               */
/* ======================================================================= */
static ffb_torque_cfg_t tcfg_plain(void)
{
  ffb_torque_cfg_t c;
  ffb_torque_default_cfg(&c);
  return c;
}

TEST(test_torque_defaults_are_gentle)
{
  ffb_torque_cfg_t c = tcfg_plain();
  CHECK(c.max_amps <= FFB_HARD_CAP_AMPS);
  CHECK(c.max_amps <= 1.0f);
  CHECK(c.spring_kp <= 4.0f);   /* 1.0 until 2026-11-17; raised on purpose (owner): 1.5 for F5 step 2, 3.0 for more resistance */
  CHECK_NEAR(c.half_range_turns, WHEEL_LOCK_TO_LOCK_DEG / 720.0, 1e-6);  /* half of the lock-to-lock (tests pin 540 deg: 0.75 turn) */
}

TEST(test_torque_spring_damper_friction_signs)
{
  CHECK(ffb_fx_spring(0.5f, 0.2f) < 0.0f);
  CHECK(ffb_fx_spring(0.5f, -0.2f) > 0.0f);
  CHECK_NEAR(ffb_fx_spring(0.5f, 0.2f), -0.1, 1e-6);
  CHECK(ffb_fx_damper(0.05f, 1.0f) < 0.0f);
  CHECK_NEAR(ffb_fx_friction(0.03f, 0.05f, 1.0f), -0.03, 1e-6);
  CHECK_NEAR(ffb_fx_friction(0.03f, 0.05f, -1.0f), 0.03, 1e-6);
  CHECK_NEAR(ffb_fx_friction(0.03f, 0.05f, 0.025f), -0.015, 1e-6);  /* linear near 0 */
  CHECK_NEAR(ffb_fx_friction(0.03f, 0.0f, 1.0f), 0.0, 0);
}

TEST(test_torque_endstop_shape)
{
  ffb_torque_cfg_t c = tcfg_plain();
  ffb_axis_t ax = { 0.0f, 0.0f };
  float prev = 0.0f;
  /* zero inside the range */
  for (float a = -0.75f; a <= 0.75f; a += 0.01f) { ax.angle_turns = a; CHECK_NEAR(ffb_fx_endstop(&c, &ax), 0, 1e-6); }
  /* monotonic, continuous build-up beyond +half, saturating at endstop_amps */
  for (float pen = 0.0f; pen <= 2.0f * c.endstop_width_turns; pen += c.endstop_width_turns / 50.0f)
  {
    float f;
    ax.angle_turns = c.half_range_turns + pen;
    f = ffb_fx_endstop(&c, &ax);
    CHECK(f <= 0.0f);                       /* pushes back towards centre */
    CHECK(f <= prev + 1e-6f);               /* monotonic */
    CHECK(prev - f <= c.endstop_amps * 0.07f);  /* no jump (smooth) */
    prev = f;
  }
  CHECK_NEAR(prev, -c.endstop_amps, 1e-5);
  /* symmetric */
  ax.angle_turns = -(c.half_range_turns + c.endstop_width_turns * 0.5f);
  CHECK_NEAR(ffb_fx_endstop(&c, &ax), c.endstop_amps * 0.5f, 1e-5);
  /* damping only when moving further out */
  ax.angle_turns = c.half_range_turns + 0.5f * c.endstop_width_turns;
  ax.vel_turns_s = 1.0f;
  CHECK(ffb_fx_endstop(&c, &ax) < -c.endstop_amps * 0.5f);
  ax.vel_turns_s = -1.0f;
  CHECK_NEAR(ffb_fx_endstop(&c, &ax), -c.endstop_amps * 0.5f, 1e-5);
}

TEST(test_torque_clamp_and_hard_cap)
{
  ffb_torque_cfg_t c = tcfg_plain();
  ffb_torque_state_t st;
  ffb_axis_t ax = { 0.0f, 0.0f };
  float a = 0.0f;
  c.max_amps = 50.0f;                      /* a bad config cannot exceed the cap */
  c.slew_amps_per_s = 1e9f;
  ffb_torque_reset(&st);
  CHECK(ffb_torque_step(&c, &st, &ax, 100.0f, true, 0.005f, &a));
  CHECK_NEAR(a, FFB_HARD_CAP_AMPS, 1e-6);
  CHECK(ffb_torque_step(&c, &st, &ax, -100.0f, true, 0.005f, &a));
  CHECK_NEAR(a, -FFB_HARD_CAP_AMPS, 1e-6);
  CHECK_NEAR(ffb_clamp(5.0f, 1.0f), 1.0, 0);
  CHECK_NEAR(ffb_clamp(-5.0f, 1.0f), -1.0, 0);
  CHECK_NEAR(ffb_clamp(0.5f, NAN), 0.0, 0);
}

TEST(test_torque_slew_limiter)
{
  ffb_torque_cfg_t c = tcfg_plain();
  ffb_torque_state_t st;
  ffb_axis_t ax = { 0.0f, 0.0f };
  float a = 0.0f, prev = 0.0f;
  ffb_torque_reset(&st);
  for (int k = 0; k < 400; k++)
  {
    CHECK(ffb_torque_step(&c, &st, &ax, 1.0f, false, 0.005f, &a));
    CHECK(a - prev <= c.slew_amps_per_s * 0.005f + 1e-6f);
    prev = a;
  }
  CHECK_NEAR(a, c.max_amps, 1e-5);
  CHECK_NEAR(ffb_slew(0.0f, 1.0f, 10.0f, 0.01f), 0.1, 1e-6);
  CHECK_NEAR(ffb_slew(0.0f, -1.0f, 10.0f, 0.01f), -0.1, 1e-6);
  CHECK_NEAR(ffb_slew(0.0f, 0.05f, 10.0f, 0.01f), 0.05, 1e-6);
  CHECK_NEAR(ffb_slew(0.3f, 1.0f, -1.0f, 0.01f), 0.3, 1e-6);  /* bad rate: hold */
}

TEST(test_torque_invalid_inputs_fail_safe)
{
  ffb_torque_cfg_t c = tcfg_plain();
  ffb_torque_state_t st;
  ffb_axis_t ax = { 0.1f, 0.0f };
  float a = 0.5f;
  ffb_torque_reset(&st);
  st.last_amps = 0.4f;
  ax.angle_turns = NAN;
  CHECK(!ffb_torque_step(&c, &st, &ax, 0.0f, true, 0.005f, &a));
  CHECK_NEAR(a, 0.0, 0);
  CHECK_NEAR(st.last_amps, 0.0, 0);
  ax.angle_turns = 0.0f;
  CHECK(!ffb_torque_step(&c, &st, &ax, INFINITY, true, 0.005f, &a));
  CHECK(!ffb_torque_step(&c, &st, &ax, 0.0f, true, 0.0f, &a));
  CHECK(!ffb_torque_step(&c, &st, &ax, 0.0f, true, NAN, &a));
}

TEST(test_torque_units_with_several_kt)
{
  static const float kts[] = { 0.04f, 1.0f, 0.5f, 0.001f, 2.0f };
  float out = 9.0f;
  for (size_t i = 0; i < sizeof(kts) / sizeof(kts[0]); i++)
  {
    CHECK(ffb_amps_to_torque(0.8f, kts[i], FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
    CHECK_NEAR(out, 0.8 * kts[i], 1e-6);
    CHECK(ffb_amps_to_torque(-1.0f, kts[i], FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
    CHECK_NEAR(out, -kts[i], 1e-6);
  }
  CHECK(!ffb_amps_to_torque(0.5f, 0.0f, FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
  CHECK_NEAR(out, 0, 0);
  CHECK(!ffb_amps_to_torque(0.5f, 8.27f, FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
  CHECK(!ffb_amps_to_torque(0.5f, NAN, FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
  CHECK(!ffb_amps_to_torque(NAN, 0.04f, FFB_TORQUE_CONSTANT_MIN, FFB_TORQUE_CONSTANT_MAX, &out));
  /* the largest possible command still passes the allowlist */
  {
    char b[FFB_CMD_MAX];
    ffb_amps_to_torque(FFB_HARD_CAP_AMPS, FFB_TORQUE_CONSTANT_MAX, FFB_TORQUE_CONSTANT_MIN,
                       FFB_TORQUE_CONSTANT_MAX, &out);
    ffb_fmt_torque(b, sizeof(b), 0, out);
    CHECK(ffb_cmd_is_allowed(b, 0));
  }
}

TEST(test_torque_fuzz_always_bounded)
{
  ffb_torque_cfg_t c = tcfg_plain();
  ffb_torque_state_t st;
  unsigned seed = 12345U;
  ffb_torque_reset(&st);
  for (int k = 0; k < 20000; k++)
  {
    ffb_axis_t ax;
    float a = 0.0f, host;
    seed = seed * 1103515245U + 12345U; ax.angle_turns = ((float)(seed % 20001U) - 10000.0f) / 1000.0f;
    seed = seed * 1103515245U + 12345U; ax.vel_turns_s = ((float)(seed % 20001U) - 10000.0f) / 500.0f;
    seed = seed * 1103515245U + 12345U; host = ((float)(seed % 20001U) - 10000.0f) / 1000.0f;
    CHECK(ffb_torque_step(&c, &st, &ax, host, (k & 1) != 0, 0.005f, &a));
    CHECK(a <= FFB_MAX_AMPS + 1e-6f && a >= -FFB_MAX_AMPS - 1e-6f);
  }
}

/* ======================================================================= */
/* ffb_ui                                                                   */
/* ======================================================================= */
TEST(test_gesture_needs_release_first_and_full_hold)
{
  ffb_gesture_t g;
  uint32_t t = 0;
  int arms = 0;
  ffb_gesture_init(&g, 3000);
  /* held at power-up (stuck buttons): never arms */
  for (; t < 10000; t++) if (ffb_gesture_update(&g, true, true, true, t) == FFB_GESTURE_ARM) arms++;
  CHECK_EQ_INT(arms, 0);
  /* release, then hold 2.9 s: nothing */
  ffb_gesture_update(&g, false, false, false, t++);
  for (uint32_t k = 0; k < 2900; k++, t++) if (ffb_gesture_update(&g, true, true, true, t) == FFB_GESTURE_ARM) arms++;
  CHECK_EQ_INT(arms, 0);
  CHECK(ffb_gesture_active(&g));
  /* releasing one paddle cancels the hold */
  ffb_gesture_update(&g, true, true, false, t++);
  CHECK(!ffb_gesture_active(&g));
  for (uint32_t k = 0; k < 2000; k++, t++) if (ffb_gesture_update(&g, true, true, true, t) == FFB_GESTURE_ARM) arms++;
  CHECK_EQ_INT(arms, 0);
  /* full release, then a full 3 s hold: exactly one ARM */
  ffb_gesture_update(&g, false, false, false, t++);
  for (uint32_t k = 0; k < 8000; k++, t++) if (ffb_gesture_update(&g, true, true, true, t) == FFB_GESTURE_ARM) arms++;
  CHECK_EQ_INT(arms, 1);
}

TEST(test_arm_pins_merge_into_paddle_bits)
{
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0000, false, false), 0x0000);
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0000, true, false), 1U << FFB_PADDLE_LEFT_BIT);
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0000, false, true), 1U << FFB_PADDLE_RIGHT_BIT);
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0000, true, true),
               (1U << FFB_PADDLE_LEFT_BIT) | (1U << FFB_PADDLE_RIGHT_BIT));
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0003, true, true) & 0x0003U, 0x0003);   /* other bits kept */
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x0300, false, false), 0x0300);           /* MCP paddles still count */
  CHECK_EQ_INT(ffb_buttons_with_arm_pins(0x8000, true, true) & 0x8000U, 0x8000);
}

TEST(test_arm_pins_gesture_end_to_end_and_fail_safe)
{
  ffb_gesture_t g;
  uint32_t t = 0;
  int arms = 0;
  ffb_gesture_init(&g, 3000);
  /* Both pin buttons stuck pressed from power-up + B1: can never arm. */
  for (; t < 8000; t++)
  {
    const uint16_t b = ffb_buttons_with_arm_pins(0, true, true);
    if (ffb_gesture_update(&g, true, (b >> FFB_PADDLE_LEFT_BIT) & 1U, (b >> FFB_PADDLE_RIGHT_BIT) & 1U, t) == FFB_GESTURE_ARM) arms++;
  }
  CHECK_EQ_INT(arms, 0);
  /* Release everything, then press B1 + both pins for 3 s: arms once. */
  ffb_gesture_update(&g, false, false, false, t++);
  for (uint32_t k = 0; k < 3100; k++, t++)
  {
    const uint16_t b = ffb_buttons_with_arm_pins(0, true, true);
    if (ffb_gesture_update(&g, true, (b >> FFB_PADDLE_LEFT_BIT) & 1U, (b >> FFB_PADDLE_RIGHT_BIT) & 1U, t) == FFB_GESTURE_ARM) arms++;
  }
  CHECK_EQ_INT(arms, 1);
  /* Only one pin button (the other wire unplugged = released): no arm. */
  ffb_gesture_update(&g, false, false, false, t++);
  for (uint32_t k = 0; k < 8000; k++, t++)
  {
    const uint16_t b = ffb_buttons_with_arm_pins(0, true, false);
    if (ffb_gesture_update(&g, true, (b >> FFB_PADDLE_LEFT_BIT) & 1U, (b >> FFB_PADDLE_RIGHT_BIT) & 1U, t) == FFB_GESTURE_ARM) arms++;
  }
  CHECK_EQ_INT(arms, 1);
}

/* B1-only arm sequence: tap, tap, hold. Helper: hold B1 in the given state for ms. */
static uint32_t b1s_run(ffb_b1seq_t *q, uint32_t *t, bool b1, uint32_t ms, int *fires)
{
  for (uint32_t k = 0; k < ms; k++, (*t)++)
  {
    if (ffb_b1seq_update(q, b1, *t)) (*fires)++;
  }
  return *t;
}

TEST(test_b1seq_tap_tap_hold_arms_once)
{
  ffb_b1seq_t q;
  uint32_t t = 0;
  int fires = 0;
  ffb_b1seq_init(&q, 3000);
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 1000, &fires);
  CHECK(ffb_b1seq_pending(&q));                    /* final hold running */
  CHECK_EQ_INT(fires, 0);
  b1s_run(&q, &t, true, 2100, &fires);             /* total 3.1 s held */
  CHECK_EQ_INT(fires, 1);
  CHECK(!ffb_b1seq_pending(&q));
  b1s_run(&q, &t, true, 10000, &fires);            /* keeps holding: no second arm */
  CHECK_EQ_INT(fires, 1);
  /* the next arm needs a full release and the whole sequence again */
  b1s_run(&q, &t, false, 100, &fires);
  b1s_run(&q, &t, true, 5000, &fires);
  CHECK_EQ_INT(fires, 1);
}

TEST(test_b1seq_rejects_everything_else)
{
  ffb_b1seq_t q;
  uint32_t t = 0;
  int fires = 0;
  /* B1 simply held for a long time (also held from power-up): never arms */
  ffb_b1seq_init(&q, 3000);
  b1s_run(&q, &t, true, 20000, &fires);
  CHECK_EQ_INT(fires, 0);
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 20000, &fires);
  CHECK_EQ_INT(fires, 0);
  /* one tap then hold */
  ffb_b1seq_init(&q, 3000); t = 0;
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 6000, &fires);
  CHECK_EQ_INT(fires, 0);
  /* two taps, gap too long, then hold */
  ffb_b1seq_init(&q, 3000); t = 0;
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 2000, &fires);
  b1s_run(&q, &t, true, 6000, &fires);
  CHECK_EQ_INT(fires, 0);
  /* three taps then hold */
  ffb_b1seq_init(&q, 3000); t = 0;
  b1s_run(&q, &t, false, 50, &fires);
  for (int i = 0; i < 3; i++) { b1s_run(&q, &t, true, 200, &fires); b1s_run(&q, &t, false, 300, &fires); }
  b1s_run(&q, &t, true, 6000, &fires);
  CHECK_EQ_INT(fires, 0);
  /* a "tap" that is too long, twice, then hold */
  ffb_b1seq_init(&q, 3000); t = 0;
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 900, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 900, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 6000, &fires);
  CHECK_EQ_INT(fires, 0);
  /* final hold released early, then a lone short press: nothing */
  ffb_b1seq_init(&q, 3000); t = 0;
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 200, &fires);  b1s_run(&q, &t, false, 300, &fires);
  b1s_run(&q, &t, true, 2900, &fires);
  b1s_run(&q, &t, false, 50, &fires);
  b1s_run(&q, &t, true, 500, &fires);
  CHECK_EQ_INT(fires, 0);
}

TEST(test_gesture_disarm_on_b1_press)
{
  ffb_gesture_t g;
  ffb_gesture_init(&g, 3000);
  CHECK_EQ_INT(ffb_gesture_update(&g, false, false, false, 0), FFB_GESTURE_NONE);
  CHECK_EQ_INT(ffb_gesture_update(&g, true, false, false, 1), FFB_GESTURE_DISARM);
  CHECK_EQ_INT(ffb_gesture_update(&g, true, false, false, 2), FFB_GESTURE_NONE);   /* edge only */
  CHECK_EQ_INT(ffb_gesture_update(&g, false, false, false, 3), FFB_GESTURE_NONE);
  CHECK_EQ_INT(ffb_gesture_update(&g, true, true, false, 4), FFB_GESTURE_DISARM);  /* one paddle */
  ffb_gesture_update(&g, false, true, true, 5);
  CHECK_EQ_INT(ffb_gesture_update(&g, true, true, true, 6), FFB_GESTURE_NONE);     /* arm combo */
}

TEST(test_led_patterns)
{
  uint32_t on = 0, t;
  CHECK(ffb_led_pattern(FFB_STATE_RUNNING, FFB_REASON_OK, FFB_FAULT_NONE, true, false, 123) == FFB_LED_GREEN);
  CHECK((ffb_led_pattern(FFB_STATE_FAULT, FFB_REASON_OK, FFB_FAULT_OVERSPEED, false, false, 7) & FFB_LED_BLUE) != 0);
  CHECK((ffb_led_pattern(FFB_STATE_DISARMED, FFB_REASON_OK, FFB_FAULT_NONE, true, false, 100) & FFB_LED_GREEN) != 0);
  CHECK((ffb_led_pattern(FFB_STATE_DISARMED, FFB_REASON_OK, FFB_FAULT_NONE, true, false, 600) & FFB_LED_GREEN) == 0);
  /* blink code: count the flashes of code 4 in one period */
  for (t = 0; t < 4 * 500 + 1500; t++)
  {
    static bool last = false;
    bool now = ffb_blink_code(4, t);
    if (now && !last) on++;
    last = now;
  }
  CHECK_EQ_INT(on, 4);
  CHECK(!ffb_blink_code(0, 10));
}

void run_ffb_core_tests(void)
{
  printf("ffb core (cmd, safety SM, precheck, torque, ui)\n");
  RUN(test_cmd_formatting);
  RUN(test_cmd_allowlist_accepts_only_three_kinds);
  RUN(test_cmd_allowlist_rejects_config_and_everything_else);
  RUN(test_sm_disarmed_sends_nothing);
  RUN(test_sm_arm_not_allowed_build_never_leaves_disarmed);
  RUN(test_sm_happy_path_and_write_order);
  RUN(test_sm_precheck_failure_returns_disarmed_with_reason);
  RUN(test_sm_precheck_timeout_and_stale_position);
  RUN(test_sm_disarm_during_precheck_and_arming);
  RUN(test_sm_user_disarm_from_running_stops_and_resends_idle);
  RUN(test_sm_every_fault_path_from_running_latches);
  RUN(test_sm_fault_paths_during_arming);
  RUN(test_sm_arming_timeout);
  RUN(test_sm_watchdog_fed_every_cycle_while_running);
  RUN(test_sm_corrupted_state_goes_to_fault);
  RUN(test_sm_names);
  RUN(test_precheck_paths_are_readonly_and_axis_patched);
  RUN(test_precheck_all_ok);
  RUN(test_precheck_each_condition);
  RUN(test_precheck_context_and_failed_reads);
  RUN(test_precheck_read_order_and_monitor_set);
  RUN(test_monitor_evaluate);
  RUN(test_torque_defaults_are_gentle);
  RUN(test_torque_spring_damper_friction_signs);
  RUN(test_torque_endstop_shape);
  RUN(test_torque_clamp_and_hard_cap);
  RUN(test_torque_slew_limiter);
  RUN(test_torque_invalid_inputs_fail_safe);
  RUN(test_torque_units_with_several_kt);
  RUN(test_torque_fuzz_always_bounded);
  RUN(test_gesture_needs_release_first_and_full_hold);
  RUN(test_arm_pins_merge_into_paddle_bits);
  RUN(test_arm_pins_gesture_end_to_end_and_fail_safe);
  RUN(test_b1seq_tap_tap_hold_arms_once);
  RUN(test_b1seq_rejects_everything_else);
  RUN(test_gesture_disarm_on_b1_press);
  RUN(test_led_patterns);
}
