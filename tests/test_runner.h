/**
 * @file    test_runner.h
 * @brief   Tiny dependency-free unit test runner (host gcc only).
 *
 * Usage:
 *   TEST(name) { CHECK(cond); CHECK_EQ_INT(a, b); CHECK_NEAR(a, b, tol); }
 *   int main(void) { RUN(name); return test_summary(); }
 */
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <math.h>
#include <stdio.h>

extern int g_tests_run;
extern int g_tests_failed;
extern int g_checks_failed;
extern int g_current_failed;

#define TEST(name) static void name(void)

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
      g_checks_failed++;                                                     \
      g_current_failed = 1;                                                  \
    }                                                                        \
  } while (0)

#define CHECK_EQ_INT(a, b)                                                   \
  do {                                                                       \
    long long va_ = (long long)(a), vb_ = (long long)(b);                    \
    if (va_ != vb_) {                                                        \
      printf("    FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
             #a, #b, va_, vb_);                                              \
      g_checks_failed++;                                                     \
      g_current_failed = 1;                                                  \
    }                                                                        \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                \
  do {                                                                       \
    double va_ = (double)(a), vb_ = (double)(b);                             \
    if (!(fabs(va_ - vb_) <= (double)(tol))) {                               \
      printf("    FAIL %s:%d: %s ~= %s (%g vs %g, tol %g)\n", __FILE__,      \
             __LINE__, #a, #b, va_, vb_, (double)(tol));                     \
      g_checks_failed++;                                                     \
      g_current_failed = 1;                                                  \
    }                                                                        \
  } while (0)

#define RUN(name)                                                            \
  do {                                                                       \
    g_current_failed = 0;                                                    \
    g_tests_run++;                                                           \
    name();                                                                  \
    if (g_current_failed) {                                                  \
      g_tests_failed++;                                                      \
      printf("  [FAIL] %s\n", #name);                                        \
    } else {                                                                 \
      printf("  [ ok ] %s\n", #name);                                        \
    }                                                                        \
  } while (0)

/* Define the counters exactly once (in the file that has main()). */
#define TEST_RUNNER_GLOBALS                                                  \
  int g_tests_run, g_tests_failed, g_checks_failed, g_current_failed;        \
  static int test_summary(void)                                              \
  {                                                                          \
    printf("%d tests, %d failed (%d failed checks)\n", g_tests_run,          \
           g_tests_failed, g_checks_failed);                                 \
    return g_tests_failed == 0 ? 0 : 1;                                      \
  }

#endif /* TEST_RUNNER_H */
