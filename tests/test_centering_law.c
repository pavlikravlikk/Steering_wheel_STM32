#include "test_runner.h"
#include "centering_law.h"

/* Run until the slew limiter has settled. */
static float settle(const centering_cfg_t *c, centering_state_t *s, float pos,
                    float vel)
{
  float out = 0.0f;
  for (int i = 0; i < 10000; i++)
  {
    out = centering_compute(c, s, pos, 0.0f, vel, 0.001f, true);
  }
  return out;
}

TEST(test_cl_defaults_are_safe)
{
  centering_cfg_t c;
  centering_default_cfg(&c);
  CHECK(c.max_amps <= 1.0f);
  CHECK(CENTERING_HARD_MAX_AMPS <= 1.0f);
  CHECK(c.kp > 0.0f && c.kp <= 1.0f);
  CHECK(c.kd >= 0.0f && c.kd <= 0.2f);
}

TEST(test_cl_spring_direction_and_value)
{
  centering_cfg_t c;
  centering_state_t s;
  centering_default_cfg(&c);
  c.deadband_turns = 0.0f;
  c.kd = 0.0f;
  centering_reset(&s);
  /* 0.5 turn right -> -0.25 A (pulls back left) */
  CHECK_NEAR(settle(&c, &s, 0.5f, 0.0f), -0.25, 1e-5);
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, -0.5f, 0.0f), 0.25, 1e-5);
}

TEST(test_cl_damping)
{
  centering_cfg_t c;
  centering_state_t s;
  centering_default_cfg(&c);
  c.kp = 0.0f;
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, 0.0f, 2.0f), -0.1, 1e-5);   /* opposes motion */
}

TEST(test_cl_clamped_to_max_and_hard_cap)
{
  centering_cfg_t c;
  centering_state_t s;
  centering_default_cfg(&c);
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, 100.0f, 0.0f), -1.0, 1e-6);
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, -100.0f, -50.0f), 1.0, 1e-6);
  /* a config above the hard cap is still limited to 1 A */
  c.max_amps = 30.0f;
  c.kp = 1000.0f;
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, 5.0f, 0.0f), -CENTERING_HARD_MAX_AMPS, 1e-6);
  c.max_amps = 0.3f;
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, 5.0f, 0.0f), -0.3, 1e-6);
}

TEST(test_cl_deadband)
{
  centering_cfg_t c;
  centering_state_t s;
  centering_default_cfg(&c);
  c.kd = 0.0f;
  centering_reset(&s);
  CHECK_NEAR(settle(&c, &s, 0.004f, 0.0f), 0.0, 1e-9);
  CHECK_NEAR(settle(&c, &s, -0.005f, 0.0f), 0.0, 1e-9);
  /* continuous at the edge: 0.105 turn -> 0.1 * 0.5 = 0.05 A */
  CHECK_NEAR(settle(&c, &s, 0.105f, 0.0f), -0.05, 1e-5);
}

TEST(test_cl_slew_limit)
{
  centering_cfg_t c;
  centering_state_t s;
  centering_default_cfg(&c);       /* 5 A/s -> 5 mA per 1 ms step */
  centering_reset(&s);
  float out = centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, true);
  CHECK_NEAR(out, -0.005, 1e-6);
  out = centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, true);
  CHECK_NEAR(out, -0.010, 1e-6);
  /* 0 -> 1 A takes 200 ms */
  for (int i = 0; i < 197; i++)
    out = centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, true);
  CHECK(out > -1.0f);
  for (int i = 0; i < 5; i++)
    out = centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, true);
  CHECK_NEAR(out, -1.0, 1e-5);
  /* a huge dt cannot exceed the clamp */
  centering_reset(&s);
  out = centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 1000.0f, true);
  CHECK_NEAR(out, -1.0, 1e-6);
}

TEST(test_cl_fail_safe_zero)
{
  centering_cfg_t c;
  centering_state_t s;
  volatile float zero = 0.0f;
  float nanv = zero / zero, infv = 1.0f / zero;
  centering_default_cfg(&c);
  centering_reset(&s);
  settle(&c, &s, 10.0f, 0.0f);
  CHECK_NEAR(s.last_out, -1.0, 1e-6);
  /* stale data: immediately 0 (not slewed) */
  CHECK_NEAR(centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, false), 0.0, 0);
  CHECK_NEAR(s.last_out, 0.0, 0);
  /* and ramps up again from 0 afterwards */
  CHECK_NEAR(centering_compute(&c, &s, 10.0f, 0.0f, 0.0f, 0.001f, true), -0.005, 1e-6);

  CHECK_NEAR(centering_compute(&c, &s, nanv, 0.0f, 0.0f, 0.001f, true), 0.0, 0);
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, infv, 0.0f, 0.001f, true), 0.0, 0);
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, nanv, 0.001f, true), 0.0, 0);
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, 0.0f, true), 0.0, 0);
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, -1.0f, true), 0.0, 0);
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, nanv, true), 0.0, 0);
  CHECK_NEAR(centering_compute(NULL, &s, 1.0f, 0.0f, 0.0f, 0.001f, true), 0.0, 0);

  /* invalid config */
  c.kp = -1.0f;
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, 0.001f, true), 0.0, 0);
  centering_default_cfg(&c);
  c.slew_amps_per_s = 0.0f;
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, 0.001f, true), 0.0, 0);
  centering_default_cfg(&c);
  c.max_amps = nanv;
  CHECK_NEAR(centering_compute(&c, &s, 1.0f, 0.0f, 0.0f, 0.001f, true), 0.0, 0);
}

TEST(test_cl_output_always_bounded_fuzz)
{
  centering_cfg_t c;
  centering_state_t s;
  unsigned seed = 12345;
  centering_default_cfg(&c);
  centering_reset(&s);
  for (int i = 0; i < 100000; i++)
  {
    seed = seed * 1103515245u + 12345u;
    float pos = ((float)(seed % 20001u) - 10000.0f) * 0.01f;
    seed = seed * 1103515245u + 12345u;
    float vel = ((float)(seed % 20001u) - 10000.0f) * 0.01f;
    float out = centering_compute(&c, &s, pos, 0.0f, vel, 0.001f, (seed & 7u) != 0u);
    if (out > 1.0f || out < -1.0f) { CHECK(0); break; }
  }
}

void run_centering_law_tests(void)
{
  printf("centering_law (not wired to the ODrive)\n");
  RUN(test_cl_defaults_are_safe);
  RUN(test_cl_spring_direction_and_value);
  RUN(test_cl_damping);
  RUN(test_cl_clamped_to_max_and_hard_cap);
  RUN(test_cl_deadband);
  RUN(test_cl_slew_limit);
  RUN(test_cl_fail_safe_zero);
  RUN(test_cl_output_always_bounded_fuzz);
}
