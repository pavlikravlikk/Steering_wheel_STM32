/* HID PID layer: report dispatcher with canned packets (incl. malformed and
 * truncated ones), the effect engine (every effect type, envelope, timing,
 * gains, device control) and the ISR queue / snapshot glue. */
#include "test_runner.h"

#include <string.h>

#include "ffb_effects.h"
#include "ffb_hid_desc.h"
#include "ffb_pid.h"
#include "ffb_config.h"

static ffb_fx_engine_t E;
static ffb_pid_stats_t S;

static void fresh(void)
{
  ffb_fx_init(&E);
  memset(&S, 0, sizeof(S));
}

static bool out(const uint8_t *b, size_t n, uint32_t now)
{
  return ffb_pid_dispatch(&E, &S, FFB_REPORT_OUTPUT, b, n, now);
}

static uint8_t create(uint8_t type)
{
  const uint8_t r[] = { FFB_RID_CREATE_EFFECT, type };
  ffb_pid_dispatch(&E, &S, FFB_REPORT_FEATURE, r, sizeof(r), 0);
  return E.bl_status == FFB_BL_SUCCESS ? E.bl_index : 0;
}

/* Set Effect: idx, type, duration, delay, gain, direction (0.01 deg) */
static void set_effect(uint8_t idx, uint8_t type, uint16_t dur, uint16_t delay, uint8_t gain, uint16_t dir)
{
  const uint8_t r[FFB_LEN_SET_EFFECT] = {
    FFB_RID_SET_EFFECT, idx, type, (uint8_t)dur, (uint8_t)(dur >> 8), 0, 0, 0, 0,
    (uint8_t)delay, (uint8_t)(delay >> 8), gain, 0, 0x03, (uint8_t)dir, (uint8_t)(dir >> 8)
  };
  CHECK(out(r, sizeof(r), 0));
}

static void set_constant(uint8_t idx, int16_t m)
{
  const uint8_t r[] = { FFB_RID_SET_CONSTANT, idx, (uint8_t)m, (uint8_t)((uint16_t)m >> 8) };
  CHECK(out(r, sizeof(r), 0));
}

static void op(uint8_t idx, uint8_t o, uint8_t loops, uint32_t now)
{
  const uint8_t r[] = { FFB_RID_EFFECT_OP, idx, o, loops };
  CHECK(out(r, sizeof(r), now));
}

static float force(uint32_t now, float pos, float vel)
{
  bool active;
  return ffb_fx_force(&E, now, pos, vel, &active);
}

/* ---------------- pool / block load ---------------- */
TEST(test_pid_create_free_full)
{
  uint8_t r[8];
  fresh();
  CHECK_EQ_INT(create(FFB_ET_CONSTANT), 1);     /* logical minimum first (no autocentre guess) */
  CHECK_EQ_INT(create(FFB_ET_SINE), 2);
  for (unsigned i = 3; i <= FFB_MAX_EFFECTS; i++) CHECK_EQ_INT(create(FFB_ET_SPRING), i);
  CHECK_EQ_INT(create(FFB_ET_SPRING), 0);
  CHECK_EQ_INT(E.bl_status, FFB_BL_FULL);
  CHECK_EQ_INT(ffb_pid_build_block_load(&E, r, sizeof(r)), FFB_LEN_BLOCK_LOAD);
  CHECK_EQ_INT(r[0], FFB_RID_BLOCK_LOAD);
  CHECK_EQ_INT(r[2], FFB_BL_FULL);
  CHECK_EQ_INT(r[3] | (r[4] << 8), 0);
  {
    const uint8_t fr[] = { FFB_RID_BLOCK_FREE, 2 };
    CHECK(out(fr, sizeof(fr), 0));
  }
  CHECK_EQ_INT(create(FFB_ET_RAMP), 2);         /* the freed slot is reused */
  CHECK_EQ_INT(create(0), 0);
  CHECK_EQ_INT(E.bl_status, FFB_BL_ERROR);
  CHECK_EQ_INT(create(FFB_ET_COUNT + 1), 0);
  CHECK_EQ_INT(E.bl_status, FFB_BL_ERROR);
}

TEST(test_pid_pool_and_state_reports)
{
  uint8_t r[8];
  fresh();
  CHECK_EQ_INT(ffb_pid_build_pool(r, sizeof(r)), FFB_LEN_POOL);
  CHECK_EQ_INT(r[0], FFB_RID_POOL);
  CHECK_EQ_INT(r[3], FFB_MAX_EFFECTS);          /* simultaneous effects max >= 2 */
  CHECK_EQ_INT(r[4] & 1, 1);                    /* device managed pool (Linux needs != 0) */
  CHECK_EQ_INT(ffb_pid_build_pool(r, 4), 0);
  create(FFB_ET_CONSTANT);
  op(1, FFB_OP_START, 1, 0);
  CHECK_EQ_INT(ffb_pid_build_state(&E, true, r, sizeof(r)), FFB_LEN_PID_STATE);
  CHECK_EQ_INT(r[0], FFB_RID_PID_STATE);
  CHECK_EQ_INT(r[1] & 0x02, 0x02);              /* actuators enabled */
  CHECK_EQ_INT(r[1] & 0x10, 0x10);              /* actuator power */
  CHECK_EQ_INT(r[2], 0x81);                     /* effect 1 playing */
}

/* ---------------- malformed input ---------------- */
TEST(test_pid_malformed_reports_are_ignored)
{
  static const uint8_t lens[][2] = {
    { FFB_RID_SET_EFFECT, FFB_LEN_SET_EFFECT }, { FFB_RID_SET_ENVELOPE, FFB_LEN_SET_ENVELOPE },
    { FFB_RID_SET_CONDITION, FFB_LEN_SET_CONDITION }, { FFB_RID_SET_PERIODIC, FFB_LEN_SET_PERIODIC },
    { FFB_RID_SET_CONSTANT, FFB_LEN_SET_CONSTANT }, { FFB_RID_SET_RAMP, FFB_LEN_SET_RAMP },
    { FFB_RID_EFFECT_OP, FFB_LEN_EFFECT_OP }, { FFB_RID_BLOCK_FREE, FFB_LEN_BLOCK_FREE },
    { FFB_RID_DEVICE_CONTROL, FFB_LEN_DEVICE_CONTROL }, { FFB_RID_DEVICE_GAIN, FFB_LEN_DEVICE_GAIN },
  };
  uint8_t b[32];
  fresh();
  CHECK_EQ_INT(create(FFB_ET_CONSTANT), 1);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);
  set_constant(1, 5000);
  /* every report, truncated by one or more bytes: ignored */
  for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++)
  {
    memset(b, 0xFF, sizeof(b));
    b[0] = lens[i][0];
    b[1] = 1;
    for (size_t n = 1; n < lens[i][1]; n++) CHECK(!out(b, n, 0));
  }
  CHECK(S.ignored_short >= 10);
  CHECK(!E.fx[0].playing);                      /* the truncated "start" did nothing */
  CHECK_EQ_INT(E.device_gain, 255);             /* the truncated gain did nothing */
  CHECK_EQ_INT(E.fx[0].constant, 5000);
  /* unknown IDs, input/feature IDs as output, output IDs as feature */
  b[0] = 0x7E; CHECK(!out(b, 16, 0));
  b[0] = FFB_RID_JOYSTICK; CHECK(!out(b, 16, 0));
  b[0] = FFB_RID_BLOCK_LOAD; CHECK(!out(b, 16, 0));
  b[0] = FFB_RID_CREATE_EFFECT; b[1] = FFB_ET_SINE; CHECK(!out(b, 2, 0));     /* create as OUTPUT */
  b[0] = FFB_RID_EFFECT_OP; b[1] = 1; b[2] = FFB_OP_START; b[3] = 1;
  CHECK(!ffb_pid_dispatch(&E, &S, FFB_REPORT_FEATURE, b, 4, 0));             /* op as FEATURE */
  CHECK(!E.fx[0].playing);
  CHECK(!ffb_pid_dispatch(&E, &S, FFB_REPORT_OUTPUT, NULL, 0, 0));
  CHECK(!ffb_pid_dispatch(&E, &S, FFB_REPORT_OUTPUT, b, 0, 0));
  /* invalid block indexes / effect types / parameter block offsets */
  set_constant(1, 1000);
  {
    const uint8_t c0[] = { FFB_RID_SET_CONSTANT, 0, 0x10, 0x27 };
    const uint8_t c9[] = { FFB_RID_SET_CONSTANT, 9, 0x10, 0x27 };       /* not allocated */
    const uint8_t cz[] = { FFB_RID_SET_CONSTANT, 200, 0x10, 0x27 };
    uint8_t se[FFB_LEN_SET_EFFECT] = { FFB_RID_SET_EFFECT, 1, 0 };
    uint8_t sc[FFB_LEN_SET_CONDITION] = { FFB_RID_SET_CONDITION, 1, 2 };
    CHECK(!out(c0, sizeof(c0), 0));
    CHECK(!out(c9, sizeof(c9), 0));
    CHECK(!out(cz, sizeof(cz), 0));
    CHECK(!out(se, sizeof(se), 0));                                        /* type 0 */
    se[2] = 12;
    CHECK(!out(se, sizeof(se), 0));                                        /* type 12 */
    CHECK(!out(sc, sizeof(sc), 0));                                        /* PBO 2 */
  }
  CHECK_EQ_INT(E.fx[0].constant, 1000);
  CHECK_EQ_INT(E.fx[0].type, FFB_ET_CONSTANT);
  /* out-of-range values are clamped, never trusted */
  {
    const uint8_t big[] = { FFB_RID_SET_CONSTANT, 1, 0xFF, 0x7F };       /* 32767 */
    CHECK(out(big, sizeof(big), 0));
    CHECK_EQ_INT(E.fx[0].constant, 10000);
  }
  /* reports longer than needed are accepted (trailing bytes ignored) */
  {
    const uint8_t longer[] = { FFB_RID_DEVICE_GAIN, 128, 1, 2, 3 };
    CHECK(out(longer, sizeof(longer), 0));
    CHECK_EQ_INT(E.device_gain, 128);
  }
}

/* ---------------- effects ---------------- */
TEST(test_fx_constant_direction_gain)
{
  fresh();
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);   /* 90 deg = +X */
  set_constant(1, 5000);
  CHECK_NEAR(force(0, 0, 0), 0.0, 0);                                 /* not started */
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(10, 0, 0), 0.5, 2e-3);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 27000);  /* 270 deg = -X */
  CHECK_NEAR(force(20, 0, 0), -0.5, 2e-3);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 0);      /* 0 deg: no X part */
  CHECK_NEAR(force(30, 0, 0), 0.0, 2e-3);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 128, 9000);   /* effect gain */
  CHECK_NEAR(force(40, 0, 0), 0.5 * 128 / 255.0, 3e-3);
  {
    const uint8_t g[] = { FFB_RID_DEVICE_GAIN, 51 };                 /* device gain 20 % */
    CHECK(out(g, sizeof(g), 0));
  }
  CHECK_NEAR(force(50, 0, 0), 0.5 * 128 / 255.0 * 51 / 255.0, 3e-3);
}

TEST(test_fx_duration_delay_loops)
{
  fresh();
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, 100, 50, 255, 9000);
  set_constant(1, 10000);
  op(1, FFB_OP_START, 2, 1000);
  CHECK_NEAR(force(1020, 0, 0), 0.0, 1e-6);       /* start delay */
  CHECK_NEAR(force(1060, 0, 0), 1.0, 2e-3);
  CHECK_NEAR(force(1240, 0, 0), 1.0, 2e-3);       /* second loop */
  CHECK_NEAR(force(1251, 0, 0), 0.0, 1e-6);       /* 50 + 2 x 100 ms: done */
  CHECK(!E.fx[0].playing);
  op(1, FFB_OP_START, 255, 2000);                 /* 255 = forever */
  CHECK_NEAR(force(2000 + 50 + 100 * 500 + 10, 0, 0), 1.0, 2e-3);
  op(1, FFB_OP_STOP, 0, 3000);
  CHECK_NEAR(force(3001, 0, 0), 0.0, 1e-6);
}

TEST(test_fx_envelope)
{
  const uint8_t env[] = { FFB_RID_SET_ENVELOPE, 1, 0x00, 0x00, 0x00, 0x00, 100, 0, 100, 0 };  /* 0 -> mag -> 0 */
  fresh();
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, 1000, 0, 255, 9000);
  set_constant(1, 8000);
  CHECK(out(env, sizeof(env), 0));
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(0, 0, 0), 0.0, 1e-3);          /* attack starts at 0 */
  CHECK_NEAR(force(50, 0, 0), 0.4, 3e-3);         /* half way */
  CHECK_NEAR(force(500, 0, 0), 0.8, 3e-3);        /* sustain */
  CHECK_NEAR(force(950, 0, 0), 0.4, 3e-3);        /* fading */
  CHECK_NEAR(force(999, 0, 0), 0.008, 3e-3);
}

TEST(test_fx_periodic_waveforms)
{
  static const struct { uint8_t type; uint32_t t; float want; } c[] = {
    { FFB_ET_SINE, 0, 0.0f }, { FFB_ET_SINE, 25, 1.0f }, { FFB_ET_SINE, 75, -1.0f },
    { FFB_ET_SQUARE, 10, 1.0f }, { FFB_ET_SQUARE, 60, -1.0f },
    { FFB_ET_TRIANGLE, 25, 1.0f }, { FFB_ET_TRIANGLE, 50, 0.0f }, { FFB_ET_TRIANGLE, 75, -1.0f },
    { FFB_ET_SAW_UP, 0, -1.0f }, { FFB_ET_SAW_UP, 50, 0.0f }, { FFB_ET_SAW_UP, 99, 0.98f },
    { FFB_ET_SAW_DOWN, 0, 1.0f }, { FFB_ET_SAW_DOWN, 75, -0.5f },
  };
  for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++)
  {
    const uint8_t per[] = { FFB_RID_SET_PERIODIC, 1, 0x10, 0x27, 0, 0, 0, 0, 100, 0 };  /* mag 10000, 100 ms */
    fresh();
    create(c[i].type);
    set_effect(1, c[i].type, FFB_TIME_INFINITE, 0, 255, 9000);
    CHECK(out(per, sizeof(per), 0));
    op(1, FFB_OP_START, 1, 0);
    CHECK_NEAR(force(c[i].t, 0, 0), c[i].want, 0.012);
  }
  /* offset and phase */
  {
    const uint8_t per[] = { FFB_RID_SET_PERIODIC, 1, 0x88, 0x13, 0xE8, 0x03, 0x28, 0x23, 100, 0 };
    /* mag 5000, offset 1000, phase 9000 (90 deg) */
    fresh();
    create(FFB_ET_SINE);
    set_effect(1, FFB_ET_SINE, FFB_TIME_INFINITE, 0, 255, 9000);
    CHECK(out(per, sizeof(per), 0));
    op(1, FFB_OP_START, 1, 0);
    CHECK_NEAR(force(0, 0, 0), 0.1 + 0.5, 0.01);    /* sin(90) = 1 */
  }
  CHECK_NEAR(ffb_sin_turns(0.125f), 0.70711, 2e-3);
  CHECK_NEAR(ffb_sin_turns(-0.25f), -1.0, 2e-3);
}

TEST(test_fx_ramp)
{
  const uint8_t ramp[] = { FFB_RID_SET_RAMP, 1, 0xF0, 0xD8, 0x10, 0x27 };   /* -10000 -> 10000 */
  fresh();
  create(FFB_ET_RAMP);
  set_effect(1, FFB_ET_RAMP, 1000, 0, 255, 9000);
  CHECK(out(ramp, sizeof(ramp), 0));
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(0, 0, 0), -1.0, 3e-3);
  CHECK_NEAR(force(500, 0, 0), 0.0, 3e-3);
  CHECK_NEAR(force(900, 0, 0), 0.8, 3e-3);
}

static void set_cond(uint8_t idx, int16_t cp, int16_t pc, int16_t nc, uint16_t ps, uint16_t ns, uint16_t db)
{
  const uint8_t r[FFB_LEN_SET_CONDITION] = {
    FFB_RID_SET_CONDITION, idx, 0,
    (uint8_t)cp, (uint8_t)((uint16_t)cp >> 8), (uint8_t)pc, (uint8_t)((uint16_t)pc >> 8),
    (uint8_t)nc, (uint8_t)((uint16_t)nc >> 8), (uint8_t)ps, (uint8_t)(ps >> 8),
    (uint8_t)ns, (uint8_t)(ns >> 8), (uint8_t)db, (uint8_t)(db >> 8)
  };
  CHECK(out(r, sizeof(r), 0));
}

TEST(test_fx_spring_condition)
{
  fresh();
  create(FFB_ET_SPRING);
  set_effect(1, FFB_ET_SPRING, FFB_TIME_INFINITE, 0, 255, 0);  /* direction ignored */
  set_cond(1, 0, 10000, 5000, 10000, 3000, 1000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(1, 0.05f, 0), 0.0, 1e-6);       /* inside the dead band */
  CHECK_NEAR(force(2, 0.5f, 0), -0.4, 2e-3);       /* pulls back */
  CHECK_NEAR(force(3, -0.5f, 0), 0.2, 2e-3);       /* negative coefficient */
  CHECK_NEAR(force(4, -1.0f, 0), 0.3, 2e-3);       /* negative saturation */
  set_cond(1, 5000, 10000, 10000, 10000, 10000, 0); /* centre point offset */
  CHECK_NEAR(force(5, 0.5f, 0), 0.0, 2e-3);
  CHECK_NEAR(force(6, 0.0f, 0), 0.5, 2e-3);
}

/* Gain per kind of effect: texture (periodic) can be boosted without touching the position/speed dependent conditions. */
TEST(test_fx_gains_per_kind)
{
  const uint8_t per[] = { FFB_RID_SET_PERIODIC, 1, 0x88, 0x13, 0, 0, 0, 0, 100, 0 };   /* magnitude 5000 (0.5), 100 ms */
  fresh();
  ffb_fx_set_gains(&E, 3.0f, 2.0f, 0.5f, 1.0f);
  create(FFB_ET_SINE);
  set_effect(1, FFB_ET_SINE, FFB_TIME_INFINITE, 0, 255, 9000);
  CHECK(out(per, sizeof(per), 0));
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(25, 0, 0), 1.0, 1e-6);                 /* 0.5 x 3 = 1.5, limited to the full scale */
  ffb_fx_set_gains(&E, 1.5f, 2.0f, 0.5f, 1.0f);
  CHECK_NEAR(force(25, 0, 0), 0.75, 0.01);                /* 0.5 x 1.5 */
  ffb_fx_set_gains(&E, 3.0f, 2.0f, 0.5f, 0.4f);           /* the periodic part alone is capped at 0.4 */
  CHECK_NEAR(force(25, 0, 0), 0.4, 1e-6);
  /* constant: x 2; the cap of the periodic part does not touch it */
  fresh();
  ffb_fx_set_gains(&E, 3.0f, 2.0f, 0.5f, 0.1f);
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);
  set_constant(1, 2000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(10, 0, 0), 0.4, 2e-3);
  /* condition: spring x 0.5 */
  fresh();
  ffb_fx_set_gains(&E, 3.0f, 2.0f, 0.5f, 1.0f);
  create(FFB_ET_SPRING);
  set_effect(1, FFB_ET_SPRING, FFB_TIME_INFINITE, 0, 255, 0);
  set_cond(1, 0, 10000, 5000, 10000, 3000, 1000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(2, 0.5f, 0), -0.4 * 0.5, 2e-3);
  /* a Device Reset from the game does not change the tuning; bad values fall back to 1 */
  ffb_fx_device_control(&E, FFB_DC_RESET);
  CHECK_NEAR(E.gains.periodic, 3.0, 1e-6);
  ffb_fx_set_gains(&E, NAN, -1.0f, 1e30f * 1e30f, 7.0f);
  CHECK_NEAR(E.gains.periodic, 1.0, 1e-6);
  CHECK_NEAR(E.gains.constant, 1.0, 1e-6);
  CHECK_NEAR(E.gains.condition, 1.0, 1e-6);
  CHECK_NEAR(E.gains.periodic_cap, 1.0, 1e-6);
}

/* Mirroring the direction touches constant / ramp / periodic only, never the conditions (they depend on the wheel's own position). */
TEST(test_fx_direction_invert_leaves_conditions_alone)
{
  const uint8_t per[] = { FFB_RID_SET_PERIODIC, 1, 0x10, 0x27, 0, 0, 0, 0, 100, 0 };
  fresh();
  ffb_fx_set_direction_invert(&E, true);
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);   /* 90 deg = +X */
  set_constant(1, 5000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(10, 0, 0), -0.5, 2e-3);                           /* mirrored */
  fresh();
  ffb_fx_set_direction_invert(&E, true);
  create(FFB_ET_SINE);
  set_effect(1, FFB_ET_SINE, FFB_TIME_INFINITE, 0, 255, 9000);
  CHECK(out(per, sizeof(per), 0));
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(25, 0, 0), -1.0, 0.012);                          /* the sine too */
  fresh();
  ffb_fx_set_direction_invert(&E, true);
  create(FFB_ET_SPRING);
  set_effect(1, FFB_ET_SPRING, FFB_TIME_INFINITE, 0, 255, 0);
  set_cond(1, 0, 10000, 5000, 10000, 3000, 1000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(2, 0.5f, 0), -0.4, 2e-3);                         /* NOT mirrored: still pulls back to the centre */
  CHECK_NEAR(force(3, -0.5f, 0), 0.2, 2e-3);
  ffb_fx_set_direction_invert(&E, false);
  fresh();
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);
  set_constant(1, 5000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(10, 0, 0), 0.5, 2e-3);                            /* back to normal */
}

TEST(test_fx_damper_friction_inertia)
{
  fresh();
  create(FFB_ET_DAMPER);
  set_effect(1, FFB_ET_DAMPER, FFB_TIME_INFINITE, 0, 255, 0);
  set_cond(1, 0, 10000, 10000, 10000, 10000, 0);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(1, 0.3f, 0.5f), -0.5, 2e-3);
  CHECK_NEAR(force(2, 0.3f, -0.2f), 0.2, 2e-3);
  fresh();
  create(FFB_ET_FRICTION);
  set_effect(1, FFB_ET_FRICTION, FFB_TIME_INFINITE, 0, 255, 0);
  set_cond(1, 0, 3000, 3000, 10000, 10000, 0);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(1, 0, 0.5f), -0.3, 2e-3);
  CHECK_NEAR(force(2, 0, -0.5f), 0.3, 2e-3);
  CHECK_NEAR(force(3, 0, 0.0f), 0.0, 2e-3);
  fresh();
  create(FFB_ET_INERTIA);
  set_effect(1, FFB_ET_INERTIA, FFB_TIME_INFINITE, 0, 255, 0);
  set_cond(1, 0, 10000, 10000, 10000, 10000, 0);
  op(1, FFB_OP_START, 1, 0);
  (void)force(100, 0, 0.0f);
  CHECK(force(110, 0, 0.05f) < 0.0f);              /* accelerating -> resists */
}

TEST(test_fx_device_control_and_safety)
{
  const uint8_t reset[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_RESET };
  const uint8_t stop[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_STOP_ALL };
  const uint8_t dis[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_DISABLE_ACTUATORS };
  const uint8_t en[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_ENABLE_ACTUATORS };
  const uint8_t pause[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_PAUSE };
  const uint8_t cont[] = { FFB_RID_DEVICE_CONTROL, FFB_DC_CONTINUE };
  bool active;
  fresh();
  create(FFB_ET_CONSTANT);
  set_effect(1, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);
  set_constant(1, 6000);
  op(1, FFB_OP_START, 1, 0);
  CHECK_NEAR(force(1, 0, 0), 0.6, 3e-3);
  CHECK(out(dis, sizeof(dis), 0));
  CHECK_NEAR(ffb_fx_force(&E, 2, 0, 0, &active), 0.0, 0);
  CHECK(!active);
  CHECK(out(en, sizeof(en), 0));
  CHECK(out(pause, sizeof(pause), 0));
  CHECK_NEAR(force(3, 0, 0), 0.0, 0);
  CHECK(out(cont, sizeof(cont), 0));
  CHECK_NEAR(force(4, 0, 0), 0.6, 3e-3);
  CHECK(out(stop, sizeof(stop), 0));
  CHECK_NEAR(force(5, 0, 0), 0.0, 0);
  CHECK(E.fx[0].allocated);                        /* stop-all keeps the effects */
  op(1, FFB_OP_START, 1, 6);
  CHECK(out(reset, sizeof(reset), 0));
  CHECK_NEAR(force(7, 0, 0), 0.0, 0);
  CHECK(!E.fx[0].allocated);                       /* reset frees everything */
  CHECK_EQ_INT(create(FFB_ET_SINE), 1);
}

TEST(test_fx_start_solo_and_total_clip)
{
  fresh();
  for (int i = 1; i <= 3; i++)
  {
    create(FFB_ET_CONSTANT);
    set_effect((uint8_t)i, FFB_ET_CONSTANT, FFB_TIME_INFINITE, 0, 255, 9000);
    set_constant((uint8_t)i, 7000);
    op((uint8_t)i, FFB_OP_START, 1, 0);
  }
  CHECK_NEAR(force(1, 0, 0), 1.0, 1e-6);           /* 2.1 clipped to full scale */
  CHECK(E.clipped >= 1);
  op(2, FFB_OP_START_SOLO, 1, 2);
  CHECK_NEAR(force(3, 0, 0), 0.7, 3e-3);
  CHECK(!E.fx[0].playing && E.fx[1].playing && !E.fx[2].playing);
}

TEST(test_fx_force_always_bounded_fuzz)
{
  unsigned seed = 99U;
  uint8_t b[FFB_LEN_MAX];
  fresh();
  for (int k = 0; k < 30000; k++)
  {
    bool active;
    float f;
    /* random (mostly invalid) reports must never break the bound */
    seed = seed * 1103515245U + 12345U;
    for (size_t j = 0; j < sizeof(b); j++) { seed = seed * 1103515245U + 12345U; b[j] = (uint8_t)(seed >> 16); }
    b[0] = (uint8_t)(0x10 + (b[0] % 0x12));
    ffb_pid_dispatch(&E, &S, (k & 1) ? FFB_REPORT_OUTPUT : FFB_REPORT_FEATURE, b, (size_t)(b[15] % 17), (uint32_t)k);
    f = ffb_fx_force(&E, (uint32_t)k, ((float)(seed % 2001) - 1000.0f) / 1000.0f, 0.1f, &active);
    CHECK(f <= 1.0f && f >= -1.0f);
    CHECK(f == f);
  }
}

/* ---------------- queue and global glue ---------------- */
TEST(test_pid_queue)
{
  ffb_pid_queue_t q;
  ffb_pid_msg_t m;
  uint8_t b[20] = { 0x15, 1, 2, 3 };
  int pushed = 0;
  ffb_pid_queue_init(&q);
  CHECK(!ffb_pid_queue_pop(&q, &m));
  while (ffb_pid_queue_push(&q, FFB_REPORT_OUTPUT, b, 4)) pushed++;
  CHECK_EQ_INT(pushed, FFB_PID_QUEUE_LEN - 1);
  CHECK_EQ_INT(q.dropped, 1);
  CHECK(ffb_pid_queue_pop(&q, &m));
  CHECK_EQ_INT(m.len, 4);
  CHECK_EQ_INT(m.data[3], 3);
  CHECK(ffb_pid_queue_push(&q, FFB_REPORT_OUTPUT, b, 20));        /* trimmed */
  CHECK(!ffb_pid_queue_push(&q, FFB_REPORT_OUTPUT, NULL, 3));
  CHECK(!ffb_pid_queue_push(&q, FFB_REPORT_OUTPUT, b, 0));
}

TEST(test_pid_glue_create_handshake)
{
  uint8_t r[16];
  const uint8_t create_rep[] = { FFB_RID_CREATE_EFFECT, FFB_ET_CONSTANT };
  ffb_pid_init();
  /* host: SET_REPORT create, then GET_REPORT block load until success */
  ffb_pid_rx_isr(FFB_REPORT_FEATURE, create_rep, sizeof(create_rep));
  CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_FEATURE, FFB_RID_BLOCK_LOAD, r, sizeof(r)), FFB_LEN_BLOCK_LOAD);
  CHECK_EQ_INT(r[2], FFB_BL_PENDING);           /* main loop has not run yet */
  ffb_pid_poll(0, false, NULL);
  ffb_pid_get_report_isr(FFB_REPORT_FEATURE, FFB_RID_BLOCK_LOAD, r, sizeof(r));
  CHECK_EQ_INT(r[2], FFB_BL_SUCCESS);
  CHECK_EQ_INT(r[1], 1);
  /* pool, status, PID state */
  CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_FEATURE, FFB_RID_POOL, r, sizeof(r)), FFB_LEN_POOL);
  {
    uint8_t st[FFB_LEN_STATUS] = { FFB_RID_STATUS, 1, 3 };
    ffb_pid_poll(1, true, st);
    CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_FEATURE, FFB_RID_STATUS, r, sizeof(r)), FFB_LEN_STATUS);
    CHECK_EQ_INT(r[2], 3);
  }
  CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_INPUT, FFB_RID_PID_STATE, r, sizeof(r)), FFB_LEN_PID_STATE);
  CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_FEATURE, 0x77, r, sizeof(r)), 0);   /* unknown */
  CHECK_EQ_INT(ffb_pid_get_report_isr(FFB_REPORT_FEATURE, FFB_RID_BLOCK_LOAD, r, 2), 0);
  /* a whole effect through the glue */
  {
    const uint8_t se[FFB_LEN_SET_EFFECT] = { FFB_RID_SET_EFFECT, 1, FFB_ET_CONSTANT, 0xFF, 0xFF, 0, 0, 0, 0,
                                             0, 0, 255, 0, 0x03, 0x28, 0x23 };
    const uint8_t sc[] = { FFB_RID_SET_CONSTANT, 1, 0x88, 0x13 };
    const uint8_t st[] = { FFB_RID_EFFECT_OP, 1, FFB_OP_START, 1 };
    bool active = false;
    ffb_pid_rx_isr(FFB_REPORT_OUTPUT, se, sizeof(se));
    ffb_pid_rx_isr(FFB_REPORT_OUTPUT, sc, sizeof(sc));
    ffb_pid_rx_isr(FFB_REPORT_OUTPUT, st, sizeof(st));
    ffb_pid_poll(10, true, NULL);
    CHECK_NEAR(ffb_pid_force(20, 0, 0, &active), ((0.5f * FFB_GAIN_CONSTANT > 1.0f) ? 1.0f : 0.5f * FFB_GAIN_CONSTANT) * ((FFB_INVERT_GAME_DIRECTION != 0) ? -1.0f : 1.0f), 3e-3);   /* the profile's gain for constant forces */
    CHECK(active);
    CHECK(ffb_pid_actuators_enabled());
    CHECK(ffb_pid_stats()->accepted >= 4);
  }
}

void run_ffb_pid_tests(void)
{
  printf("ffb HID PID layer (dispatcher, effects, glue)\n");
  RUN(test_pid_create_free_full);
  RUN(test_pid_pool_and_state_reports);
  RUN(test_pid_malformed_reports_are_ignored);
  RUN(test_fx_constant_direction_gain);
  RUN(test_fx_duration_delay_loops);
  RUN(test_fx_envelope);
  RUN(test_fx_periodic_waveforms);
  RUN(test_fx_ramp);
  RUN(test_fx_spring_condition);
  RUN(test_fx_gains_per_kind);
  RUN(test_fx_direction_invert_leaves_conditions_alone);
  RUN(test_fx_damper_friction_inertia);
  RUN(test_fx_device_control_and_safety);
  RUN(test_fx_start_solo_and_total_clip);
  RUN(test_fx_force_always_bounded_fuzz);
  RUN(test_pid_queue);
  RUN(test_pid_glue_create_handshake);
}
