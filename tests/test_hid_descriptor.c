/* Parses the REAL report descriptor out of usbd_custom_hid_if.c (between
 * "USER CODE BEGIN 0" and "USER CODE END 0") and checks it against the
 * size constant in usbd_conf.h and the 8-byte report packed by hid_pack. */
#include "test_runner.h"
#include "hid_pack.h"

#include <stdlib.h>
#include <string.h>

#define DESC_FILE "../steeringwheel_firmware/USB_DEVICE/App/usbd_custom_hid_if.c"
#define CONF_FILE "../steeringwheel_firmware/USB_DEVICE/Target/usbd_conf.h"

static char *slurp(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (buf != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n)
  {
    buf[n] = '\0';
  }
  fclose(f);
  return buf;
}

/* Extract 0xNN tokens, skipping // comments. Returns the byte count. */
static int extract_desc(uint8_t *out, int max)
{
  char *src = slurp(DESC_FILE);
  if (src == NULL)
  {
    return -1;
  }
  char *p = strstr(src, "USER CODE BEGIN 0");
  char *end = (p != NULL) ? strstr(p, "USER CODE END 0") : NULL;
  int n = 0;
  if (p == NULL || end == NULL)
  {
    free(src);
    return -1;
  }
  p = strchr(p, '\n');
  while (p != NULL && p < end)
  {
    if (p[0] == '/' && p[1] == '/')
    {
      p = strchr(p, '\n');
      continue;
    }
    if (p[0] == '/' && p[1] == '*')
    {
      p = strstr(p + 2, "*/");
      if (p != NULL) p += 2;
      continue;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
      if (n < max) out[n] = (uint8_t)strtoul(p, &p, 16);
      n++;
      continue;
    }
    p++;
  }
  free(src);
  return n;
}

static long conf_value(const char *name)
{
  char *src = slurp(CONF_FILE);
  long v = -1;
  if (src != NULL)
  {
    char *p = strstr(src, name);
    while (p != NULL)
    {
      /* find the "#define NAME value" occurrence */
      if (p > src && (p[-1] == ' ' || p[-1] == '\t'))
      {
        char *q = p + strlen(name);
        if (*q == ' ' || *q == '\t')
        {
          v = strtol(q, NULL, 0);
          break;
        }
      }
      p = strstr(p + 1, name);
    }
    free(src);
  }
  return v;
}

TEST(test_descriptor_size_matches_conf)
{
  uint8_t d[256];
  int n = extract_desc(d, (int)sizeof(d));
  CHECK(n > 0);
  CHECK_EQ_INT(n, conf_value("USBD_CUSTOM_HID_REPORT_DESC_SIZE"));
  CHECK_EQ_INT(n, 42);
}

TEST(test_binterval_is_1ms)
{
  CHECK_EQ_INT(conf_value("CUSTOM_HID_FS_BINTERVAL"), 1);
}

/* Minimal HID short-item walker: sums input bits, checks axis range. */
TEST(test_descriptor_input_report_is_8_bytes_no_report_id)
{
  uint8_t d[256];
  int n = extract_desc(d, (int)sizeof(d));
  long report_size = 0, report_count = 0, input_bits = 0;
  long logical_max = 0;
  int report_id_seen = 0, axes = 0, depth = 0;
  int i = 0;

  CHECK(n > 0);
  while (i < n)
  {
    uint8_t prefix = d[i];
    int size = prefix & 0x03;
    if (size == 3) size = 4;
    uint8_t tag = prefix & 0xFC;
    unsigned long val = 0;
    for (int k = 0; k < size && (i + 1 + k) < n; k++)
    {
      val |= (unsigned long)d[i + 1 + k] << (8 * k);
    }
    switch (tag)
    {
      case 0x74: report_size = (long)val; break;
      case 0x94: report_count = (long)val; break;
      case 0x84: report_id_seen = 1; break;
      case 0x24: logical_max = (long)val; break;
      case 0x08: if (val >= 0x30 && val <= 0x32) axes++; break;
      case 0x80: input_bits += report_size * report_count;
                 if (report_size == 16) CHECK_EQ_INT(logical_max, 65535);
                 break;
      case 0xA0: depth++; break;
      case 0xC0: depth--; break;
      default: break;
    }
    i += 1 + size;
  }
  CHECK_EQ_INT(i, n);               /* items end exactly at the last byte */
  CHECK_EQ_INT(depth, 0);           /* collections balanced               */
  CHECK_EQ_INT(report_id_seen, 0);
  CHECK_EQ_INT(axes, 3);            /* X, Y, Z                            */
  CHECK_EQ_INT(input_bits, HID_REPORT_SIZE * 8);
}

void run_hid_descriptor_tests(void)
{
  printf("hid_descriptor\n");
  RUN(test_descriptor_size_matches_conf);
  RUN(test_binterval_is_1ms);
  RUN(test_descriptor_input_report_is_8_bytes_no_report_id);
}
