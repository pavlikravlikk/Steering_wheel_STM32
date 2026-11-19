#include "test_runner.h"
#include "odrive_ascii.h"

#include <string.h>

static void feed_str(odr_t *r, const char *s, uint32_t now)
{
  odr_feed(r, (const uint8_t *)s, strlen(s), now);
}

/* ---------------- read-only allowlist ---------------- */
TEST(test_allowlist_accepts_reads)
{
  CHECK(odrive_cmd_is_readonly("f 0\n"));
  CHECK(odrive_cmd_is_readonly("f 1\n"));
  CHECK(odrive_cmd_is_readonly("r axis0.encoder.pos_estimate\n"));
  CHECK(odrive_cmd_is_readonly("r axis0.encoder.shadow_count\n"));
  CHECK(odrive_cmd_is_readonly("r axis0.error\n"));
  CHECK(odrive_cmd_is_readonly("r vbus_voltage\n"));
}

TEST(test_allowlist_rejects_everything_else)
{
  static const char *bad[] = {
    NULL, "", "\n", "f\n", "f 0", "f 0 \n", "f 00\n", "f x\n", "F 0\n",
    "r \n", "r axis0.error", "r axis0.error 1\n", "r axis0.error\n\n",
    "r AXIS0.error\n", "r axis0.requested_state;w x\n",
    "w axis0.requested_state 8\n", "w axis0.controller.input_torque 1\n",
    "w axis0.config.startup_closed_loop_control 1\n",
    "c 0 0.5\n", "c 0 0\n", "q 0 1 2 3\n", "p 0 1\n", "v 0 1\n", "t 0 1\n",
    "u 0\n", "sc\n", "ss\n", "sb\n", "se\n", "sr\n", "es\n", "h\n", "i\n",
    " f 0\n", "ff 0\n", "rr axis0\n",
    "r axis0.error\r\n",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
  {
    if (odrive_cmd_is_readonly(bad[i]))
    {
      printf("    accepted: \"%s\"\n", bad[i] ? bad[i] : "(null)");
    }
    CHECK(!odrive_cmd_is_readonly(bad[i]));
  }
}

TEST(test_request_is_readonly)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  CHECK(strcmp(odr_request(&r), "f 0\n") == 0);
  CHECK(odrive_cmd_is_readonly(odr_request(&r)));
  odr_init(&r, 1, 8, 100);
  CHECK(strcmp(odr_request(&r), "f 1\n") == 0);
  odr_init(&r, 7, 8, 100);           /* invalid axis -> axis 0 */
  CHECK(strcmp(odr_request(&r), "f 0\n") == 0);
}

/* ---------------- number / line parsing ---------------- */
TEST(test_parse_float)
{
  float v = 0;
  const char *end;
  CHECK(odr_parse_float("1.5", &end, &v)); CHECK_NEAR(v, 1.5, 1e-6); CHECK(*end == '\0');
  CHECK(odr_parse_float("-0.000000", &end, &v)); CHECK_NEAR(v, 0, 1e-9);
  CHECK(odr_parse_float("+12", &end, &v)); CHECK_NEAR(v, 12, 1e-6);
  CHECK(odr_parse_float(".25", &end, &v)); CHECK_NEAR(v, 0.25, 1e-6);
  CHECK(odr_parse_float("3.", &end, &v)); CHECK_NEAR(v, 3, 1e-6);
  CHECK(odr_parse_float("1.25e2", &end, &v)); CHECK_NEAR(v, 125, 1e-4);
  CHECK(odr_parse_float("-2.5E-3", &end, &v)); CHECK_NEAR(v, -0.0025, 1e-7);
  CHECK(odr_parse_float("123456.789012", &end, &v)); CHECK_NEAR(v, 123456.789, 0.02);
  CHECK(!odr_parse_float("", &end, &v));
  CHECK(!odr_parse_float("-", &end, &v));
  CHECK(!odr_parse_float(".", &end, &v));
  CHECK(!odr_parse_float("nan", &end, &v));
  CHECK(!odr_parse_float("inf", &end, &v));
  CHECK(!odr_parse_float("-inf", &end, &v));
  CHECK(!odr_parse_float("1e", &end, &v));
  CHECK(!odr_parse_float("1e99", &end, &v));
}

TEST(test_parse_good_lines)
{
  float p = 0, v = 0;
  CHECK_EQ_INT(odr_parse_feedback_line("0.123456 -1.500000", "f 0", &p, &v), ODR_LINE_OK);
  CHECK_NEAR(p, 0.123456, 1e-6); CHECK_NEAR(v, -1.5, 1e-6);
  CHECK_EQ_INT(odr_parse_feedback_line("  -12.5\t0  ", "f 0", &p, &v), ODR_LINE_OK);
  CHECK_NEAR(p, -12.5, 1e-6); CHECK_NEAR(v, 0, 1e-9);
}

TEST(test_parse_bad_lines)
{
  float p = 7, v = 7;
  CHECK_EQ_INT(odr_parse_feedback_line("invalid command format", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("invalid motor", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("1.0", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("1.0 2.0 3.0", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("1.0x 2.0", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("1.0 2.0x", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("nan 0", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_EQ_INT(odr_parse_feedback_line("1.0 2.0*12", "f 0", &p, &v), ODR_LINE_ERROR);
  CHECK_NEAR(p, 7, 0); CHECK_NEAR(v, 7, 0);   /* untouched on error */
  CHECK_EQ_INT(odr_parse_feedback_line("f 0", "f 0", &p, &v), ODR_LINE_ECHO);
  CHECK_EQ_INT(odr_parse_feedback_line(" f 0 ", "f 0", &p, &v), ODR_LINE_ECHO);
  CHECK_EQ_INT(odr_parse_feedback_line("", "f 0", &p, &v), ODR_LINE_EMPTY);
  CHECK_EQ_INT(odr_parse_feedback_line("   ", "f 0", &p, &v), ODR_LINE_EMPTY);
}

/* ---------------- stream handling ---------------- */
TEST(test_stream_whole_line)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  CHECK(odr_is_stale(&r, 0));        /* no data yet */
  feed_str(&r, "0.250000 0.000000\r\n", 5);
  CHECK(r.have_data);
  CHECK_NEAR(r.pos_turns, 0.25, 1e-6);
  CHECK_EQ_INT(r.stats.replies_ok, 1);
  CHECK(!odr_is_stale(&r, 5));
}

TEST(test_stream_partial_chunks)
{
  odr_t r;
  const char *msg = "-1.750000 0.500000\r\n";
  odr_init(&r, 0, 8, 100);
  for (size_t i = 0; i < strlen(msg); i++)
  {
    odr_feed(&r, (const uint8_t *)&msg[i], 1, (uint32_t)i);
    if (i + 1 < strlen(msg)) CHECK(!r.have_data);
  }
  CHECK(r.have_data);
  CHECK_NEAR(r.pos_turns, -1.75, 1e-6);
  CHECK_NEAR(r.vel_turns_s, 0.5, 1e-6);

  /* split in two arbitrary chunks */
  feed_str(&r, "2.0 ", 10);
  CHECK_NEAR(r.pos_turns, -1.75, 1e-6);
  feed_str(&r, "1.0\n", 11);
  CHECK_NEAR(r.pos_turns, 2.0, 1e-6);
  CHECK_EQ_INT(r.stats.replies_ok, 2);
}

TEST(test_stream_garbage_and_recovery)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  feed_str(&r, "xx#!garbage\r\n", 1);
  CHECK_EQ_INT(r.stats.parse_errors, 1);
  CHECK(!r.have_data);
  feed_str(&r, "1.0 0.0\n", 2);
  CHECK(r.have_data);
  CHECK_NEAR(r.pos_turns, 1.0, 1e-6);

  /* binary noise inside a line poisons only that line */
  const uint8_t noisy[] = { '2', '.', 0xFF, '0', ' ', '0', '\n' };
  odr_feed(&r, noisy, sizeof(noisy), 3);
  CHECK_NEAR(r.pos_turns, 1.0, 1e-6);
  CHECK_EQ_INT(r.stats.parse_errors, 2);
  const uint8_t nul[] = { 0x00, '\n' };
  odr_feed(&r, nul, sizeof(nul), 4);
  CHECK_EQ_INT(r.stats.parse_errors, 3);
  feed_str(&r, "3.0 0.0\n", 5);
  CHECK_NEAR(r.pos_turns, 3.0, 1e-6);
}

TEST(test_stream_echo_ignored)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  odr_note_sent(&r, 0);
  feed_str(&r, "f 0\r\n", 1);
  CHECK_EQ_INT(r.stats.echo_lines, 1);
  CHECK_EQ_INT(r.stats.parse_errors, 0);
  CHECK(r.outstanding);              /* echo is not the reply */
  feed_str(&r, "0.5 0\r\n", 2);
  CHECK(!r.outstanding);
  CHECK_NEAR(r.pos_turns, 0.5, 1e-6);
}

TEST(test_stream_overflow)
{
  odr_t r;
  char longline[200];
  odr_init(&r, 0, 8, 100);
  memset(longline, '1', sizeof(longline) - 2);
  longline[sizeof(longline) - 2] = '\n';
  longline[sizeof(longline) - 1] = '\0';
  feed_str(&r, longline, 1);
  CHECK_EQ_INT(r.stats.overflows, 1);
  CHECK(!r.have_data);
  CHECK_EQ_INT(r.len, 0);
  feed_str(&r, "4.0 0.0\n", 2);       /* next line is parsed normally */
  CHECK(r.have_data);
  CHECK_NEAR(r.pos_turns, 4.0, 1e-6);

  /* a line of exactly ODR_LINE_MAX chars is still accepted */
  char exact[ODR_LINE_MAX + 2];
  memset(exact, ' ', sizeof(exact));
  memcpy(exact, "5.0 1.0", 7);
  exact[ODR_LINE_MAX] = '\n';
  exact[ODR_LINE_MAX + 1] = '\0';
  feed_str(&r, exact, 3);
  CHECK_EQ_INT(r.stats.overflows, 1);
  CHECK_NEAR(r.pos_turns, 5.0, 1e-6);
}

/* ---------------- request / timeout / stale ---------------- */
TEST(test_timeout_and_stale)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  CHECK(odr_may_send(&r, 0));
  odr_note_sent(&r, 0);
  CHECK(!odr_may_send(&r, 3));       /* waiting for the reply */
  CHECK(odr_may_send(&r, 8));        /* timed out -> may resend */
  CHECK_EQ_INT(r.stats.timeouts, 1);
  odr_note_sent(&r, 10);
  feed_str(&r, "1.0 0.0\n", 12);
  CHECK(odr_may_send(&r, 13));       /* answered */
  CHECK_EQ_INT(r.stats.timeouts, 1);
  CHECK(!odr_is_stale(&r, 12 + 99));
  CHECK(odr_is_stale(&r, 12 + 100));
  CHECK_NEAR(r.pos_turns, 1.0, 1e-6); /* last value is held while stale */
  /* tick wrap-around */
  odr_note_sent(&r, 0xFFFFFFFCu);
  CHECK(!odr_may_send(&r, 0xFFFFFFFEu));
  CHECK(odr_may_send(&r, 4));
}

TEST(test_error_reply_clears_outstanding)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  odr_note_sent(&r, 0);
  feed_str(&r, "invalid command format\r\n", 1);
  CHECK(!r.outstanding);
  CHECK_EQ_INT(r.stats.parse_errors, 1);
  CHECK(odr_is_stale(&r, 1));
}

/* ---------------- one-shot reads (FFB precheck) ---------------- */
TEST(test_single_line_parser)
{
  float v = -1.0f;
  CHECK(odr_parse_single_line("24.087744", &v)); CHECK_NEAR(v, 24.087744, 1e-4);
  CHECK(odr_parse_single_line(" 1 ", &v));       CHECK_NEAR(v, 1, 0);
  CHECK(odr_parse_single_line("True", &v));      CHECK_NEAR(v, 1, 0);
  CHECK(odr_parse_single_line("false", &v));     CHECK_NEAR(v, 0, 0);
  CHECK(!odr_parse_single_line("1.0 2.0", &v));
  CHECK(!odr_parse_single_line("invalid property", &v));
  CHECK(!odr_parse_single_line("Truex", &v));
  CHECK(!odr_parse_single_line("nan", &v));
  CHECK(!odr_parse_single_line("", &v));
}

/* Real replies of "r axis0.error" on the bench (2026-11-17): a decimal integer plus ONE trailing 'd'. */
TEST(test_single_line_parser_axis_error_suffix)
{
  float v = -1.0f;
  CHECK(odr_parse_single_line("0d", &v));        CHECK_NEAR(v, 0, 0);
  CHECK(odr_parse_single_line("2048d", &v));     CHECK_NEAR(v, 2048, 0);
  CHECK(odr_parse_single_line(" 2048d ", &v));   CHECK_NEAR(v, 2048, 0);
  CHECK(odr_parse_single_line("524288d", &v));   CHECK_NEAR(v, 524288, 0);
  /* everything else after a number is still rejected */
  CHECK(!odr_parse_single_line("d", &v));
  CHECK(!odr_parse_single_line("2048dd", &v));
  CHECK(!odr_parse_single_line("2048d1", &v));
  CHECK(!odr_parse_single_line("2048 d", &v));
  CHECK(!odr_parse_single_line("1.5d", &v));
  CHECK(!odr_parse_single_line("-5d", &v));
  CHECK(!odr_parse_single_line("1e3d", &v));
  CHECK(!odr_parse_single_line("2048x", &v));
  CHECK(!odr_parse_single_line("nand", &v));
}

TEST(test_queue_read_builds_readonly_request)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  CHECK(strcmp(odr_next_request(&r), "f 0\n") == 0);
  CHECK(odr_queue_read(&r, "axis0.motor.is_calibrated"));
  CHECK(strcmp(odr_next_request(&r), "r axis0.motor.is_calibrated\n") == 0);
  CHECK(odrive_cmd_is_readonly(odr_next_request(&r)));
  CHECK(!odr_queue_read(&r, "vbus_voltage"));          /* one at a time */
  /* nothing that is not a pure path can be queued */
  odr_init(&r, 0, 8, 100);
  CHECK(!odr_queue_read(&r, "axis0.x 1"));
  CHECK(!odr_queue_read(&r, "a\nw b 1"));
  CHECK(!odr_queue_read(&r, ""));
  CHECK(!odr_queue_read(&r, NULL));
  CHECK(!odr_queue_read(&r, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  CHECK_EQ_INT(r.read_state, ODR_READ_IDLE);
  CHECK(strcmp(odr_next_request(&r), "f 0\n") == 0);
}

TEST(test_read_roundtrip_and_feedback_resumes)
{
  odr_t r;
  float v = 0.0f;
  odr_init(&r, 0, 8, 100);
  CHECK(odr_queue_read(&r, "vbus_voltage"));
  CHECK(odr_may_send(&r, 0));
  odr_note_sent(&r, 0);
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_PENDING);
  CHECK(!odr_may_send(&r, 1));
  feed_str(&r, "24.1\r\n", 2);
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_DONE);
  CHECK_NEAR(v, 24.1, 1e-4);
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_IDLE);  /* reported once */
  CHECK_EQ_INT(r.stats.reads_ok, 1);
  CHECK_EQ_INT(r.stats.parse_errors, 0);
  CHECK(odr_may_send(&r, 3));
  CHECK(strcmp(odr_next_request(&r), "f 0\n") == 0);
  odr_note_sent(&r, 3);
  feed_str(&r, "0.5 0.0\r\n", 4);
  CHECK_NEAR(r.pos_turns, 0.5, 1e-6);
}

TEST(test_read_error_reply_and_timeout_fail)
{
  odr_t r;
  float v = 0.0f;
  odr_init(&r, 0, 8, 100);
  CHECK(odr_queue_read(&r, "axis0.does_not_exist"));
  odr_note_sent(&r, 0);
  feed_str(&r, "invalid property\r\n", 1);
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_FAILED);
  CHECK_EQ_INT(r.stats.read_errors, 1);
  CHECK(odr_may_send(&r, 2));                     /* answered: not outstanding */

  CHECK(odr_queue_read(&r, "vbus_voltage"));
  odr_note_sent(&r, 10);
  CHECK(!odr_may_send(&r, 17));
  CHECK(odr_may_send(&r, 18));                    /* timeout */
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_FAILED);
  CHECK_EQ_INT(r.stats.read_timeouts, 1);
}

TEST(test_late_feedback_during_read_is_used_not_taken_as_value)
{
  odr_t r;
  float v = 0.0f;
  odr_init(&r, 0, 8, 100);
  CHECK(odr_queue_read(&r, "axis0.encoder.is_ready"));
  odr_note_sent(&r, 0);
  feed_str(&r, "1.25 0.0\r\n", 1);                /* late "f" reply */
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_PENDING);
  CHECK_NEAR(r.pos_turns, 1.25, 1e-6);
  feed_str(&r, "r axis0.encoder.is_ready\r\n", 2); /* echo of the read */
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_PENDING);
  feed_str(&r, "1\r\n", 3);
  CHECK_EQ_INT(odr_read_poll(&r, &v), ODR_READ_DONE);
  CHECK_NEAR(v, 1.0, 0);
}

TEST(test_single_value_without_read_is_still_a_parse_error)
{
  odr_t r;
  odr_init(&r, 0, 8, 100);
  odr_note_sent(&r, 0);
  feed_str(&r, "24.1\r\n", 1);
  CHECK_EQ_INT(r.stats.parse_errors, 1);
  CHECK_EQ_INT(r.stats.reads_ok, 0);
  CHECK(!r.have_data);
}

void run_odrive_ascii_tests(void)
{
  printf("odrive_ascii\n");
  RUN(test_allowlist_accepts_reads);
  RUN(test_allowlist_rejects_everything_else);
  RUN(test_request_is_readonly);
  RUN(test_parse_float);
  RUN(test_parse_good_lines);
  RUN(test_parse_bad_lines);
  RUN(test_stream_whole_line);
  RUN(test_stream_partial_chunks);
  RUN(test_stream_garbage_and_recovery);
  RUN(test_stream_echo_ignored);
  RUN(test_stream_overflow);
  RUN(test_timeout_and_stale);
  RUN(test_error_reply_clears_outstanding);
  RUN(test_single_line_parser);
  RUN(test_single_line_parser_axis_error_suffix);
  RUN(test_queue_read_builds_readonly_request);
  RUN(test_read_roundtrip_and_feedback_resumes);
  RUN(test_read_error_reply_and_timeout_fail);
  RUN(test_late_feedback_during_read_is_used_not_taken_as_value);
  RUN(test_single_value_without_read_is_still_a_parse_error);
}
