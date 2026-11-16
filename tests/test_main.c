#include "test_runner.h"

TEST_RUNNER_GLOBALS

void run_hid_pack_tests(void);
void run_hid_descriptor_tests(void);
void run_odrive_ascii_tests(void);
void run_steering_map_tests(void);
void run_odrive_link_tests(void);
void run_debounce_tests(void);
void run_mcp23017_tests(void);
void run_centering_law_tests(void);
void run_safety_tests(void);
void run_ffb_core_tests(void);
void run_ffb_service_tests(void);
void run_ffb_desc_tests(void);
void run_ffb_pid_tests(void);

int main(void)
{
  run_ffb_core_tests();
  run_ffb_service_tests();
  run_ffb_desc_tests();
  run_ffb_pid_tests();
  run_hid_pack_tests();
  run_hid_descriptor_tests();
  run_odrive_ascii_tests();
  run_steering_map_tests();
  run_odrive_link_tests();
  run_debounce_tests();
  run_mcp23017_tests();
  run_centering_law_tests();
  run_safety_tests();
  return test_summary();
}
