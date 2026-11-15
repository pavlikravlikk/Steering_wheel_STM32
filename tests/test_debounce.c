#include "test_runner.h"
#include "debounce.h"

TEST(test_debounce_press_needs_5ms)
{
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  CHECK_EQ_INT(debounce_update(&d, 0x001, 1), 0);   /* raw changes at t=1 */
  CHECK_EQ_INT(debounce_update(&d, 0x001, 2), 0);
  CHECK_EQ_INT(debounce_update(&d, 0x001, 5), 0);   /* 4 ms */
  CHECK_EQ_INT(debounce_update(&d, 0x001, 6), 1);   /* 5 ms: accepted */
}

TEST(test_debounce_bounce_rejected)
{
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  /* contact bounce: toggles every ms for 10 ms -> never stable */
  for (uint32_t t = 1; t <= 10; t++)
  {
    CHECK_EQ_INT(debounce_update(&d, (uint16_t)(t & 1U), t), 0);
  }
  /* then settles pressed */
  uint16_t s = 0;
  for (uint32_t t = 11; t <= 16; t++)
  {
    s = debounce_update(&d, 1, t);
  }
  CHECK_EQ_INT(s, 1);
}

TEST(test_debounce_release_also_debounced)
{
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  for (uint32_t t = 1; t <= 10; t++) debounce_update(&d, 0x200, t);
  CHECK_EQ_INT(d.stable, 0x200);
  CHECK_EQ_INT(debounce_update(&d, 0, 11), 0x200);
  CHECK_EQ_INT(debounce_update(&d, 0, 15), 0x200);
  CHECK_EQ_INT(debounce_update(&d, 0x200, 16), 0x200);  /* glitch back */
  CHECK_EQ_INT(debounce_update(&d, 0, 17), 0x200);
  CHECK_EQ_INT(debounce_update(&d, 0, 22), 0);
}

TEST(test_debounce_independent_bits_and_mask)
{
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  debounce_update(&d, 0x001, 0);
  debounce_update(&d, 0x003, 3);
  CHECK_EQ_INT(debounce_update(&d, 0x003, 5), 0x001);
  CHECK_EQ_INT(debounce_update(&d, 0x003, 8), 0x003);
  /* bits above nbits are ignored */
  CHECK_EQ_INT(debounce_update(&d, 0xFC03, 100), 0x003);
}

TEST(test_debounce_sparse_sampling)
{
  /* samples arrive irregularly (slow bus): still wall-time based */
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  CHECK_EQ_INT(debounce_update(&d, 1, 10), 0);
  CHECK_EQ_INT(debounce_update(&d, 1, 20), 1);
}

TEST(test_debounce_reset)
{
  debounce_t d;
  debounce_init(&d, 10, 5, 0);
  for (uint32_t t = 0; t < 10; t++) debounce_update(&d, 0x3FF, t);
  CHECK_EQ_INT(d.stable, 0x3FF);
  debounce_reset(&d, 10);
  CHECK_EQ_INT(d.stable, 0);
}

TEST(test_debounce_tick_wrap)
{
  debounce_t d;
  debounce_init(&d, 1, 5, 0xFFFFFFFEu);
  debounce_update(&d, 1, 0xFFFFFFFEu);
  CHECK_EQ_INT(debounce_update(&d, 1, 2), 0);
  CHECK_EQ_INT(debounce_update(&d, 1, 3), 1);
}

TEST(test_hold_detector)
{
  hold_detector_t h;
  hold_init(&h, 1000);
  CHECK(!hold_update(&h, true, 100));
  CHECK(!hold_update(&h, true, 1099));
  CHECK(hold_update(&h, true, 1100));      /* fires once */
  CHECK(!hold_update(&h, true, 1200));
  CHECK(!hold_update(&h, true, 5000));
  CHECK(!hold_update(&h, false, 5001));
  CHECK(!hold_update(&h, true, 6000));     /* new press */
  CHECK(!hold_update(&h, false, 6500));    /* released early */
  CHECK(!hold_update(&h, true, 7000));
  CHECK(hold_update(&h, true, 8000));
}

void run_debounce_tests(void)
{
  printf("debounce\n");
  RUN(test_debounce_press_needs_5ms);
  RUN(test_debounce_bounce_rejected);
  RUN(test_debounce_release_also_debounced);
  RUN(test_debounce_independent_bits_and_mask);
  RUN(test_debounce_sparse_sampling);
  RUN(test_debounce_reset);
  RUN(test_debounce_tick_wrap);
  RUN(test_hold_detector);
}
