#include "test_runner.h"

TEST_RUNNER_GLOBALS

void run_hid_pack_tests(void);
void run_hid_descriptor_tests(void);

int main(void)
{
  run_hid_pack_tests();
  run_hid_descriptor_tests();
  return test_summary();
}
