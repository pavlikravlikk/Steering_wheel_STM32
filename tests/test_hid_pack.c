#include "test_runner.h"
#include "hid_pack.h"

#include <string.h>

TEST(test_layout_little_endian)
{
  uint8_t r[HID_REPORT_SIZE];
  hid_pack_report(r, 0x1234, 0xABCD, 0x00FF, 0x0201, 16);
  CHECK_EQ_INT(r[0], 0x34);
  CHECK_EQ_INT(r[1], 0x12);
  CHECK_EQ_INT(r[2], 0xCD);
  CHECK_EQ_INT(r[3], 0xAB);
  CHECK_EQ_INT(r[4], 0xFF);
  CHECK_EQ_INT(r[5], 0x00);
  CHECK_EQ_INT(r[6], 0x01);
  CHECK_EQ_INT(r[7], 0x02);
}

TEST(test_size_is_8)
{
  CHECK_EQ_INT(HID_REPORT_SIZE, 8);
}

TEST(test_clamping)
{
  CHECK_EQ_INT(hid_clamp_axis(-1), 0);
  CHECK_EQ_INT(hid_clamp_axis(-100000), 0);
  CHECK_EQ_INT(hid_clamp_axis(0), 0);
  CHECK_EQ_INT(hid_clamp_axis(65535), 65535);
  CHECK_EQ_INT(hid_clamp_axis(65536), 65535);
  CHECK_EQ_INT(hid_clamp_axis(2147483647), 65535);

  uint8_t r[HID_REPORT_SIZE];
  hid_pack_report(r, -5, 70000, 32768, 0, 10);
  CHECK_EQ_INT(r[0] | (r[1] << 8), 0);
  CHECK_EQ_INT(r[2] | (r[3] << 8), 65535);
  CHECK_EQ_INT(r[4] | (r[5] << 8), 32768);
}

TEST(test_button_bits_masked_to_count)
{
  uint8_t r[HID_REPORT_SIZE];
  hid_pack_report(r, 0, 0, 0, 0xFFFFFFFFu, 10);
  CHECK_EQ_INT(r[6] | (r[7] << 8), 0x03FF);

  hid_pack_report(r, 0, 0, 0, 1u << 9, 10);   /* button 10 = right paddle */
  CHECK_EQ_INT(r[6] | (r[7] << 8), 0x0200);

  hid_pack_report(r, 0, 0, 0, 1u << 10, 10);  /* bit 10 not in use        */
  CHECK_EQ_INT(r[6] | (r[7] << 8), 0);

  hid_pack_report(r, 0, 0, 0, 0x1FFFFu, 16);
  CHECK_EQ_INT(r[6] | (r[7] << 8), 0xFFFF);

  hid_pack_report(r, 0, 0, 0, 0xFFFFu, 0);
  CHECK_EQ_INT(r[6] | (r[7] << 8), 0);
}

TEST(test_centre_constant)
{
  CHECK_EQ_INT(HID_AXIS_CENTRE, 32768);
}

void run_hid_pack_tests(void)
{
  printf("hid_pack\n");
  RUN(test_layout_little_endian);
  RUN(test_size_is_8);
  RUN(test_clamping);
  RUN(test_button_bits_masked_to_count);
  RUN(test_centre_constant);
}
