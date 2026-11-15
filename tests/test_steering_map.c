#include "test_runner.h"
#include "steering_map.h"

TEST(test_not_centred_reports_centre)
{
  steering_map_t m;
  steering_map_init(&m, 540.0f, 1.0f, false);
  CHECK_EQ_INT(steering_map_axis16(&m, 0.3f), 32768);
}

TEST(test_centre_and_ends_540)
{
  steering_map_t m;
  steering_map_init(&m, 540.0f, 1.0f, false);
  CHECK(steering_map_set_centre(&m, 10.0f));        /* arbitrary power-up pos */
  CHECK_EQ_INT(steering_map_axis16(&m, 10.0f), 32768);
  /* +270 deg = +0.75 turn = right lock */
  CHECK_EQ_INT(steering_map_axis16(&m, 10.75f), 65535);
  CHECK_EQ_INT(steering_map_axis16(&m, 9.25f), 0);
  /* +135 deg = half way right */
  CHECK_EQ_INT(steering_map_axis16(&m, 10.375f), 49152);
  CHECK_EQ_INT(steering_map_axis16(&m, 9.625f), 16384);
  CHECK_NEAR(steering_map_angle_deg(&m, 10.375f), 135.0, 1e-3);
}

TEST(test_clamping_beyond_lock)
{
  steering_map_t m;
  steering_map_init(&m, 540.0f, 1.0f, false);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 5.0f), 65535);
  CHECK_EQ_INT(steering_map_axis16(&m, -5.0f), 0);
  CHECK_EQ_INT(steering_map_axis16(&m, 1e30f), 65535);
}

TEST(test_lock_to_lock_and_ratio_configurable)
{
  steering_map_t m;
  steering_map_init(&m, 900.0f, 1.0f, false);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 1.25f), 65535);   /* 450 deg */
  CHECK_EQ_INT(steering_map_axis16(&m, 0.625f), 49152);

  /* belt: 3 encoder turns per wheel turn */
  steering_map_init(&m, 540.0f, 3.0f, false);
  steering_map_set_centre(&m, 0.0f);
  CHECK_NEAR(steering_map_angle_deg(&m, 3.0f), 360.0, 1e-3);
  CHECK_EQ_INT(steering_map_axis16(&m, 1.125f), 49152);  /* 135 deg */
}

TEST(test_invert)
{
  steering_map_t m;
  steering_map_init(&m, 540.0f, 1.0f, true);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 0.375f), 16384);
  CHECK_EQ_INT(steering_map_axis16(&m, -0.375f), 49152);
}

TEST(test_recentre)
{
  steering_map_t m;
  steering_map_init(&m, 540.0f, 1.0f, false);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 0.5f), 54613);
  CHECK(steering_map_set_centre(&m, 0.5f));
  CHECK_EQ_INT(steering_map_axis16(&m, 0.5f), 32768);
}

TEST(test_invalid_inputs)
{
  steering_map_t m;
  volatile float zero = 0.0f;
  float nanv = zero / zero;
  float infv = 1.0f / zero;
  steering_map_init(&m, 540.0f, 1.0f, false);
  CHECK(!steering_map_set_centre(&m, nanv));
  CHECK(!steering_map_set_centre(&m, infv));
  CHECK(!m.centred);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, nanv), 32768);
  steering_map_init(&m, 0.0f, 1.0f, false);             /* bad config */
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 0.1f), 32768);
  steering_map_init(&m, 540.0f, 0.0f, false);
  steering_map_set_centre(&m, 0.0f);
  CHECK_EQ_INT(steering_map_axis16(&m, 0.1f), 32768);
}

TEST(test_monotonic)
{
  steering_map_t m;
  uint16_t prev = 0;
  steering_map_init(&m, 540.0f, 1.0f, false);
  steering_map_set_centre(&m, 0.0f);
  for (int i = -1000; i <= 1000; i++)
  {
    uint16_t x = steering_map_axis16(&m, (float)i * 0.001f);
    CHECK(x >= prev);
    prev = x;
  }
}

void run_steering_map_tests(void)
{
  printf("steering_map\n");
  RUN(test_not_centred_reports_centre);
  RUN(test_centre_and_ends_540);
  RUN(test_clamping_beyond_lock);
  RUN(test_lock_to_lock_and_ratio_configurable);
  RUN(test_invert);
  RUN(test_recentre);
  RUN(test_invalid_inputs);
  RUN(test_monotonic);
}
