/* ffb_service against a simulated ODrive (mock ffb_io_t): the complete
 * DISARMED -> PRECHECK -> ARMING -> RUNNING -> FAULT flow, "no write before
 * arm", write ordering, watchdog cadence, units and every run-time fault. */
#include "test_runner.h"

#include <stdlib.h>
#include <string.h>

#include "ffb_cmd.h"
#include "ffb_config.h"
#include "ffb_service.h"

/* ---------------- mock ODrive ---------------- */
#define MAX_LOG 4096
typedef struct
{
  /* property values */
  float enable_watchdog, watchdog_timeout, control_mode, current_lim, vel_limit, torque_constant;
  float motor_calibrated, encoder_ready, axis_error, motor_error, encoder_error, vbus, current_state;
  bool  refuse_closed_loop;     /* requested_state 8 is ignored            */
  bool  fail_reads;             /* every read answers "invalid property"   */
  bool  drop_replies;           /* reads and f requests time out           */
  bool  refuse_writes;          /* the UART queue is full                  */
  /* link */
  char  queued[64];
  bool  have_queued, read_sent;
  uint32_t timeouts, replies;
  /* wheel */
  bool  have_wheel;
  float angle, vel;
  uint32_t age;                 /* base age (injected faults raise it)      */
  bool  model_age;              /* age also grows since the last REAL "f" request (a read replaces that request) */
  bool  f_seen;
  uint32_t last_f_ms;
  float host;
  bool  host_on;
  /* logs */
  char  writes[MAX_LOG][FFB_CMD_MAX];
  bool  urgent[MAX_LOG];
  int   nwrites;
  int   nreads, nrequests;
} mock_t;

static mock_t M;

static void mock_ready(void)
{
  memset(&M, 0, sizeof(M));
  M.enable_watchdog = 1; M.watchdog_timeout = 0.25f; M.control_mode = 1; M.current_lim = 4;
  M.vel_limit = 2; M.torque_constant = 0.04f; M.motor_calibrated = 1; M.encoder_ready = 1;
  M.vbus = 24.0f; M.current_state = 1;
  M.have_wheel = true; M.age = 2;
  M.model_age = true;           /* bench 2026-11-17: a queued read displaces the position request */
}

static bool prop(const char *path, float *v)
{
  static const struct { const char *p; size_t off; } map[] = {
#define E(path, field) { path, offsetof(mock_t, field) }
    E("axis0.config.enable_watchdog", enable_watchdog), E("axis0.config.watchdog_timeout", watchdog_timeout),
    E("axis0.controller.config.control_mode", control_mode), E("axis0.motor.config.current_lim", current_lim),
    E("axis0.controller.config.vel_limit", vel_limit), E("axis0.motor.config.torque_constant", torque_constant),
    E("axis0.motor.is_calibrated", motor_calibrated), E("axis0.encoder.is_ready", encoder_ready),
    E("axis0.error", axis_error), E("axis0.motor.error", motor_error), E("axis0.encoder.error", encoder_error),
    E("vbus_voltage", vbus), E("axis0.current_state", current_state),
#undef E
  };
  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
  {
    if (strcmp(map[i].p, path) == 0)
    {
      *v = *(float *)((char *)&M + map[i].off);
      return true;
    }
  }
  return false;
}

static bool io_queue_read(const char *path)
{
  if (M.have_queued) return false;
  snprintf(M.queued, sizeof(M.queued), "%s", path);
  M.have_queued = true;
  M.read_sent = false;
  return true;
}

static odr_read_state_t io_read_poll(float *value)
{
  if (!M.have_queued) return ODR_READ_IDLE;
  if (!M.read_sent) return ODR_READ_QUEUED;
  M.have_queued = false;
  if (M.drop_replies) { M.timeouts++; return ODR_READ_FAILED; }
  if (M.fail_reads || !prop(M.queued, value)) return ODR_READ_FAILED;
  return ODR_READ_DONE;
}

static void io_link_request(uint32_t now)
{
  (void)now;
  M.nrequests++;
  if (M.have_queued && !M.read_sent) { M.read_sent = true; M.nreads++; return; }
  M.f_seen = true; M.last_f_ms = now;      /* a real position request went out */
  if (M.drop_replies) M.timeouts++; else M.replies++;
}

static bool io_send_write(const char *cmd, bool urgent)
{
  if (!ffb_cmd_is_allowed(cmd, 0)) { printf("    NOT ALLOWED: %s", cmd); CHECK(0); return false; }
  if (M.refuse_writes) return false;
  if (M.nwrites < MAX_LOG)
  {
    snprintf(M.writes[M.nwrites], FFB_CMD_MAX, "%s", cmd);
    M.urgent[M.nwrites] = urgent;
    M.nwrites++;
  }
  if (strcmp(cmd, "w axis0.requested_state 8\n") == 0 && !M.refuse_closed_loop) M.current_state = 8;
  if (strcmp(cmd, "w axis0.requested_state 1\n") == 0) M.current_state = 1;
  return true;
}

static bool io_wheel(uint32_t now, float *a, float *v, uint32_t *age)
{
  *a = M.angle; *v = M.vel; *age = M.age;
  if (M.model_age && M.f_seen) *age = M.age + (now - M.last_f_ms);
  return M.have_wheel;
}
static uint32_t io_timeouts(void) { return M.timeouts; }
static uint32_t io_replies(void) { return M.replies; }
static float io_host(uint32_t now, float p, float v, bool *active)
{
  (void)now; (void)p; (void)v;
  *active = M.host_on;
  return M.host_on ? M.host : 0.0f;
}

static const ffb_io_t k_io = { io_queue_read, io_read_poll, io_send_write, io_link_request, io_wheel,
                               io_timeouts, io_replies, io_host, NULL };

static ffb_service_cfg_t cfg_armable(void)
{
  ffb_service_cfg_t c;
  ffb_service_default_cfg(&c);
  c.arm_allowed = true;
  c.simulation = false;
  c.profile_safety_tbd = 0;        /* pretend the profile was measured */
  c.verify_closed_loop = true;
  c.invert = false;
  c.invert_host = false;
  return c;
}

/* helpers */
static uint32_t T;
static void run_ms(ffb_service_t *s, uint32_t ms, bool b1, uint16_t buttons)
{
  for (uint32_t k = 0; k < ms; k++, T++)
  {
    ffb_service_buttons(s, b1, buttons, T);
    ffb_service_poll(s, T);
  }
}
#define PADDLES ((uint16_t)((1U << FFB_PADDLE_LEFT_BIT) | (1U << FFB_PADDLE_RIGHT_BIT)))

static void do_arm_gesture(ffb_service_t *s)
{
  run_ms(s, 20, false, 0);
  run_ms(s, FFB_ARM_HOLD_MS + 20, true, PADDLES);
  run_ms(s, 20, false, 0);
}

/* bench without paddles (FFB_ARM_B1_ONLY): tap, tap, hold B1 */
static void do_b1_sequence(ffb_service_t *s, uint32_t hold_ms)
{
  run_ms(s, 20, false, 0);
  run_ms(s, 200, true, 0);  run_ms(s, 300, false, 0);
  run_ms(s, 200, true, 0);  run_ms(s, 300, false, 0);
  run_ms(s, hold_ms, true, 0);
  run_ms(s, 20, false, 0);
}

static int count_writes(const char *prefix, int from)
{
  int n = 0;
  for (int i = from; i < M.nwrites; i++) if (strncmp(M.writes[i], prefix, strlen(prefix)) == 0) n++;
  return n;
}

static float torque_of(int i)
{
  return strtof(M.writes[i] + strlen("w axis0.controller.input_torque "), NULL);
}

static void to_running(ffb_service_t *s)
{
  do_arm_gesture(s);
  run_ms(s, 600, false, 0);
}

/* ---------------- tests ---------------- */
TEST(test_svc_does_not_arm_with_wheel_outside_the_range)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  /* half range of the profile lock-to-lock, plus a margin: already inside the end-stop wall */
  M.angle = s.tcfg.half_range_turns + 0.05f;
  run_ms(&s, 10000, false, 0);
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_WHEEL_OUT_OF_RANGE);
  do_arm_gesture(&s);
  run_ms(&s, 600, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(M.nwrites, 0);
  /* the other side too */
  M.angle = -(s.tcfg.half_range_turns + 0.05f);
  run_ms(&s, 2000, false, 0);
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_WHEEL_OUT_OF_RANGE);
  /* back inside the range: ready, and the gesture arms */
  M.angle = s.tcfg.half_range_turns - 0.05f;
  run_ms(&s, 2000, false, 0);
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_OK);
  do_arm_gesture(&s);
  run_ms(&s, 600, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
}

TEST(test_svc_b1_only_mode_arms_and_default_mode_does_not)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  c.arm_b1_only = true;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  run_ms(&s, 10000, false, 0);                       /* background precheck: ready */
  CHECK(ffb_service_centre_allowed(&s, T));
  /* B1 alone, held or tapped, never arms */
  run_ms(&s, 6000, true, 0);
  run_ms(&s, 100, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(M.nwrites, 0);
  /* during the final hold of the sequence set-centre is blocked and the gesture LED is active */
  run_ms(&s, 20, false, 0);
  run_ms(&s, 200, true, 0);  run_ms(&s, 300, false, 0);
  run_ms(&s, 200, true, 0);  run_ms(&s, 300, false, 0);
  run_ms(&s, 1500, true, 0);
  CHECK(!ffb_service_centre_allowed(&s, T));
  CHECK(ffb_b1seq_pending(&s.b1seq));
  run_ms(&s, FFB_ARM_HOLD_MS, true, 0);
  run_ms(&s, 20, false, 0);
  run_ms(&s, 600, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
  CHECK(count_writes("w axis0.requested_state 8", 0) >= 1);
  /* default mode (arm_b1_only = false): the same sequence does nothing */
  {
    ffb_service_t s2;
    ffb_service_cfg_t c2 = cfg_armable();
    mock_ready();
    T = 0;
    ffb_service_init(&s2, &c2, &k_io, T);
    run_ms(&s2, 10000, false, 0);
    do_b1_sequence(&s2, FFB_ARM_HOLD_MS + 20);
    run_ms(&s2, 600, false, 0);
    CHECK_EQ_INT(s2.sm.state, FFB_STATE_DISARMED);
    CHECK_EQ_INT(M.nwrites, 0);
  }
}

TEST(test_svc_no_write_before_arm)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  run_ms(&s, 10000, false, 0);
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
  CHECK(M.nreads > 10);                       /* background reads only */
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_OK);   /* ...and they show "ready" */
  /* buttons that are not the full gesture never arm */
  run_ms(&s, 5000, true, 0);
  run_ms(&s, 100, false, 0);
  run_ms(&s, 5000, false, PADDLES);
  run_ms(&s, 5000, true, (uint16_t)(1U << FFB_PADDLE_LEFT_BIT));
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
}

TEST(test_svc_arm_not_allowed_build)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  c.arm_allowed = false;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  run_ms(&s, 3000, false, 0);
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_ARM_NOT_ALLOWED);
}

TEST(test_svc_profile_tbd_blocks)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  c.profile_safety_tbd = 5;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  run_ms(&s, 3000, false, 0);
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(s.sm.reason, FFB_REASON_PROFILE_TBD);
}

TEST(test_svc_precheck_failure_no_write)
{
  static const struct { float *field; float bad; ffb_reason_t want; } cases[] = {
    { &M.current_lim, 12.0f, FFB_REASON_CURRENT_LIM_HIGH }, { &M.vel_limit, 50.0f, FFB_REASON_VEL_LIMIT_HIGH },
    { &M.control_mode, 3.0f, FFB_REASON_CONTROL_MODE },     { &M.enable_watchdog, 0.0f, FFB_REASON_WATCHDOG_DISABLED },
    { &M.encoder_ready, 0.0f, FFB_REASON_ENCODER_NOT_READY }, { &M.axis_error, 2048.0f, FFB_REASON_AXIS_ERROR },
    { &M.vbus, 40.0f, FFB_REASON_VBUS_RANGE },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    ffb_service_t s;
    ffb_service_cfg_t c = cfg_armable();
    mock_ready();
    *cases[i].field = cases[i].bad;
    T = 0;
    ffb_service_init(&s, &c, &k_io, T);
    do_arm_gesture(&s);
    run_ms(&s, 2500, false, 0);
    CHECK_EQ_INT(M.nwrites, 0);
    CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
    CHECK_EQ_INT(s.sm.reason, cases[i].want);
  }
}

TEST(test_svc_read_errors_block)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  mock_ready();
  M.fail_reads = true;
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  run_ms(&s, 2500, false, 0);
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(s.sm.reason, FFB_REASON_READ_ERROR);
}

TEST(test_svc_full_arm_sequence_and_order)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  int first_running_write;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  /* the gesture alone writes nothing (PRECHECK is read-only) */
  CHECK(s.sm.state == FFB_STATE_PRECHECK || s.sm.state == FFB_STATE_ARMING);
  run_ms(&s, 600, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
  /* first three writes: torque 0, closed loop, watchdog feed */
  CHECK(M.nwrites >= 3);
  CHECK(strcmp(M.writes[0], "w axis0.controller.input_torque 0.0000\n") == 0);
  CHECK(strcmp(M.writes[1], "w axis0.requested_state 8\n") == 0);
  CHECK(strcmp(M.writes[2], "u 0\n") == 0);
  CHECK_EQ_INT(count_writes("w axis0.requested_state 8", 0), 1);
  CHECK_EQ_INT(count_writes("w axis0.requested_state 1", 0), 0);
  /* running: one torque + one feed per command cycle */
  first_running_write = M.nwrites;
  run_ms(&s, 1000, false, 0);
  {
    const int cycles = 1000 / (int)FFB_COMMAND_PERIOD_MS;
    const int nt = count_writes("w axis0.controller.input_torque", first_running_write);
    const int nf = count_writes("u 0", first_running_write);
    CHECK(nt >= cycles - 1 && nt <= cycles + 1);
    CHECK(nf >= cycles - 1 && nf <= cycles + 1);
  }
  CHECK(s.rate_hz >= 190 && s.rate_hz <= 210);
}

TEST(test_svc_spring_sign_units_and_slew)
{
  static const float kts[] = { 0.04f, 1.0f };
  for (size_t k = 0; k < 2; k++)
  {
    ffb_service_t s;
    ffb_service_cfg_t c = cfg_armable();
    int from;
    float last = 0.0f;
    mock_ready();
    M.torque_constant = kts[k];
    T = 0;
    ffb_service_init(&s, &c, &k_io, T);
    to_running(&s);
    CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
    M.angle = 0.4f;                         /* 0.4 wheel turn to one side */
    from = M.nwrites;
    run_ms(&s, 2000, false, 0);
    for (int i = from; i < M.nwrites; i++)
    {
      if (strncmp(M.writes[i], "w axis0.controller.input_torque", 31) == 0)
      {
        const float t = torque_of(i);
        CHECK(t <= 0.0f);                    /* spring pulls back */
        CHECK(t - last >= -(FFB_SLEW_AMPS_PER_S * kts[k] * 0.005f) - 1e-3f);  /* slew */
        last = t;
      }
    }
    /* settled value = -kp * angle * kt (inside the end stops, no motion) */
    CHECK_NEAR(last, -FFB_SPRING_KP * 0.4f * kts[k], 1e-3 + 1e-4 * kts[k]);
    CHECK_NEAR(s.last_amps, -FFB_SPRING_KP * 0.4f, 1e-3);
  }
}

TEST(test_svc_host_force_direction_and_actuators)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  to_running(&s);
  M.host_on = true;
  M.host = 0.5f;
  run_ms(&s, 1000, false, 0);
  CHECK_NEAR(s.last_amps, 0.5f * FFB_MAX_AMPS - FFB_FRICTION_AMPS * 0.0f, 1e-3);  /* spring off */
  ffb_service_set_actuators(&s, false);
  run_ms(&s, 1000, false, 0);
  CHECK_NEAR(s.last_amps, 0.0, 1e-3);
  ffb_service_set_actuators(&s, true);
  /* inverted steering flips the game force in the motor frame */
  c.invert = true;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  to_running(&s);
  M.host_on = true;
  M.host = 0.5f;
  run_ms(&s, 1000, false, 0);
  CHECK_NEAR(s.last_amps, -0.5f * FFB_MAX_AMPS, 1e-3);
  /* a huge/NaN host value is clamped / faults */
  M.host = 1e9f;
  run_ms(&s, 500, false, 0);
  CHECK(s.last_amps >= -FFB_MAX_AMPS - 1e-6f && s.last_amps <= FFB_MAX_AMPS + 1e-6f);
  M.host = NAN;
  run_ms(&s, 50, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_FAULT);
  CHECK_EQ_INT(s.sm.fault, FFB_FAULT_TORQUE_INVALID);
}

/* After a fault: urgent torque 0 then IDLE, and never again a feed or a
 * non-zero torque or closed-loop request. */
static void check_stopped_after(int from)
{
  bool seen_stop = false;
  CHECK(from < M.nwrites);
  CHECK(strcmp(M.writes[from], "w axis0.controller.input_torque 0.0000\n") == 0);
  CHECK(M.urgent[from]);
  CHECK(from + 1 < M.nwrites && strcmp(M.writes[from + 1], "w axis0.requested_state 1\n") == 0);
  for (int i = from; i < M.nwrites; i++)
  {
    if (strcmp(M.writes[i], "w axis0.requested_state 1\n") == 0) seen_stop = true;
    CHECK(strcmp(M.writes[i], "u 0\n") != 0);
    CHECK(strcmp(M.writes[i], "w axis0.requested_state 8\n") != 0);
    if (strncmp(M.writes[i], "w axis0.controller.input_torque", 31) == 0) CHECK(torque_of(i) == 0.0f);
  }
  CHECK(seen_stop);
}

typedef void (*inject_fn)(void);
static void inj_stale(void)    { M.age = 1000; }
static void inj_nodata(void)   { M.have_wheel = false; }
static void inj_axis_err(void) { M.axis_error = 0x800; }
static void inj_motor_err(void){ M.motor_error = 0x1000; }
static void inj_leave_cl(void) { M.current_state = 1; }
static void inj_vbus(void)     { M.vbus = 33.0f; }
static void inj_timeouts(void) { M.drop_replies = true; }
static void inj_tx(void)       { M.refuse_writes = true; }
static void inj_speed(void)    { M.vel = FFB_OVERSPEED_TURNS_S * 1.2f; }
static void inj_readfail(void) { M.fail_reads = true; }

TEST(test_svc_every_runtime_fault)
{
  static const struct { inject_fn fn; ffb_fault_t want; const char *name; uint32_t within_ms; } cases[] = {
    { inj_stale, FFB_FAULT_POSITION_STALE, "stale", 10 },
    { inj_nodata, FFB_FAULT_POSITION_STALE, "nodata", 10 },
    { inj_axis_err, FFB_FAULT_ODRIVE_ERROR, "axis_error", 120 },
    { inj_motor_err, FFB_FAULT_ODRIVE_ERROR, "motor_error", 120 },
    { inj_leave_cl, FFB_FAULT_ODRIVE_ERROR, "left closed loop", 120 },
    { inj_vbus, FFB_FAULT_VBUS_RANGE, "vbus", 120 },
    { inj_timeouts, FFB_FAULT_UART_TIMEOUT, "timeouts", 100 },
    { inj_tx, FFB_FAULT_WATCHDOG_FEED_FAILED, "tx refused", 30 },
    { inj_speed, FFB_FAULT_OVERSPEED, "overspeed", 10 },
    { inj_readfail, FFB_FAULT_READ_FAILED, "read failed", FFB_MONITOR_MAX_AGE_MS + 100 },
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    ffb_service_t s;
    ffb_service_cfg_t c = cfg_armable();
    int from;
    uint32_t t0;
    mock_ready();
    T = 0;
    ffb_service_init(&s, &c, &k_io, T);
    to_running(&s);
    CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
    from = M.nwrites;
    cases[i].fn();
    t0 = T;
    while (s.sm.state == FFB_STATE_RUNNING && T - t0 < 2000) run_ms(&s, 1, false, 0);
    if (s.sm.state != FFB_STATE_FAULT || s.sm.fault != cases[i].want || (T - t0) > cases[i].within_ms)
    {
      printf("    %s: state %d fault %d after %u ms\n", cases[i].name, s.sm.state, s.sm.fault, T - t0);
    }
    CHECK_EQ_INT(s.sm.state, FFB_STATE_FAULT);
    CHECK_EQ_INT(s.sm.fault, cases[i].want);
    CHECK(T - t0 <= cases[i].within_ms);
    if (cases[i].fn == inj_tx)
    {
      M.refuse_writes = false;          /* let the stop commands through */
      from = M.nwrites;
      run_ms(&s, 600, false, 0);
      CHECK(count_writes("w axis0.requested_state 1", from) >= 1);
    }
    else
    {
      int stop = from;
      run_ms(&s, 1200, false, 0);
      while (stop < M.nwrites && !M.urgent[stop]) stop++;   /* first urgent write */
      check_stopped_after(stop);
    }
    /* latched: even a new arm gesture does nothing */
    do_arm_gesture(&s);
    run_ms(&s, 1000, false, 0);
    CHECK_EQ_INT(s.sm.state, FFB_STATE_FAULT);
  }
}

TEST(test_svc_closed_loop_refused_gives_arming_timeout)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  mock_ready();
  M.refuse_closed_loop = true;
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  run_ms(&s, FFB_ARMING_TIMEOUT_MS + 800, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_FAULT);
  CHECK_EQ_INT(s.sm.fault, FFB_FAULT_ARMING_TIMEOUT);
  CHECK(count_writes("w axis0.requested_state 1", 0) >= 1);
}

TEST(test_svc_user_disarm_with_b1)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  int from;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  to_running(&s);
  CHECK(!ffb_service_centre_allowed(&s, T));
  from = M.nwrites;
  run_ms(&s, 10, true, 0);                   /* B1 press */
  run_ms(&s, 10, false, 0);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_DISARMED);
  CHECK_EQ_INT(s.sm.fault, FFB_FAULT_NONE);
  CHECK(strcmp(M.writes[from], "w axis0.controller.input_torque 0.0000\n") == 0 && M.urgent[from]);
  CHECK(strcmp(M.writes[from + 1], "w axis0.requested_state 1\n") == 0);
  CHECK(!ffb_service_centre_allowed(&s, T));  /* lockout right after disarm */
  run_ms(&s, FFB_CENTRE_LOCKOUT_MS + 10, false, 0);
  CHECK(ffb_service_centre_allowed(&s, T));
  from = M.nwrites;
  run_ms(&s, 5000, false, 0);
  CHECK_EQ_INT(M.nwrites, from);             /* quiet again */
  /* can be re-armed */
  to_running(&s);
  CHECK_EQ_INT(s.sm.state, FFB_STATE_RUNNING);
}

TEST(test_svc_simulation_never_touches_odrive)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  c.simulation = true;
  mock_ready();
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  do_arm_gesture(&s);
  run_ms(&s, 3000, false, 0);
  CHECK_EQ_INT(M.nwrites, 0);
  CHECK_EQ_INT(M.nrequests, 0);
  CHECK_EQ_INT(s.bg_reason, FFB_REASON_SIMULATION_BUILD);
}

TEST(test_svc_status_report_layout)
{
  ffb_service_t s;
  ffb_service_cfg_t c = cfg_armable();
  uint8_t r[FFB_STATUS_REPORT_LEN];
  mock_ready();
  M.current_lim = 12.0f;
  T = 0;
  ffb_service_init(&s, &c, &k_io, T);
  run_ms(&s, 4000, false, 0);
  ffb_service_status_report(&s, r);
  CHECK_EQ_INT(r[0], 0x40);
  CHECK_EQ_INT(r[1], 1);
  CHECK_EQ_INT(r[2], FFB_STATE_DISARMED);
  CHECK_EQ_INT(r[3], FFB_REASON_CURRENT_LIM_HIGH);
  CHECK_EQ_INT(r[4], FFB_FAULT_NONE);
  CHECK((r[5] & FFB_FLAG_ARM_ALLOWED_BUILD) != 0);
  CHECK((r[5] & FFB_FLAG_PRECHECK_OK) == 0);
  CHECK_EQ_INT(r[14] | (r[15] << 8), 240);   /* 24.0 V */
  CHECK(ffb_service_leds(&s, T) == 0 || ffb_service_leds(&s, T) == FFB_LED_ORANGE);
}

void run_ffb_service_tests(void)
{
  printf("ffb service (mock ODrive)\n");
  RUN(test_svc_does_not_arm_with_wheel_outside_the_range);
  RUN(test_svc_b1_only_mode_arms_and_default_mode_does_not);
  RUN(test_svc_no_write_before_arm);
  RUN(test_svc_arm_not_allowed_build);
  RUN(test_svc_profile_tbd_blocks);
  RUN(test_svc_precheck_failure_no_write);
  RUN(test_svc_read_errors_block);
  RUN(test_svc_full_arm_sequence_and_order);
  RUN(test_svc_spring_sign_units_and_slew);
  RUN(test_svc_host_force_direction_and_actuators);
  RUN(test_svc_every_runtime_fault);
  RUN(test_svc_closed_loop_refused_gives_arming_timeout);
  RUN(test_svc_user_disarm_with_b1);
  RUN(test_svc_simulation_never_touches_odrive);
  RUN(test_svc_status_report_layout);
}
