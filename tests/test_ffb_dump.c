/* The firmware's ODrive property names and pre-arm logic against the REAL
 * parameter dump docs/odrive_dump.txt (odrivetool/IPython session format):
 *  - every path the FFB precheck reads exists in the dump,
 *  - the real saved values give the expected verdicts,
 *  - applying the gentle profile (+ calibration) would make the precheck pass. */
#define _DEFAULT_SOURCE
#include "test_runner.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ffb_precheck.h"

#define DUMP_FILE "../docs/odrive_dump.txt"
#define MAX_KV    400

typedef struct { char path[96]; char value[64]; } kv_t;
static kv_t KV[MAX_KV];
static int NKV;

static void put(const char *path, const char *value)
{
  for (int i = 0; i < NKV; i++)
    if (strcmp(KV[i].path, path) == 0) { snprintf(KV[i].value, sizeof(KV[i].value), "%s", value); return; }
  if (NKV < MAX_KV)
  {
    snprintf(KV[NKV].path, sizeof(KV[NKV].path), "%s", path);
    snprintf(KV[NKV].value, sizeof(KV[NKV].value), "%s", value);
    NKV++;
  }
}

/* Minimal reader of the dump format (the Python tools/odrive_dump.py is the
 * full one): "In [n]: odrv0.x.y" sets the prefix, indentation nests,
 * "name: value (type)" is a leaf, "name: ..." and "f(...)" are skipped. */
static int load_dump(void)
{
  FILE *f = fopen(DUMP_FILE, "r");
  char line[256];
  char prefix[96] = "";
  int ind[8];
  char names[8][48];
  int depth = 0;
  bool skip = true;
  NKV = 0;
  if (f == NULL) return -1;
  while (fgets(line, sizeof(line), f) != NULL)
  {
    char *p = line, *colon;
    int indent = 0;
    line[strcspn(line, "\r\n")] = '\0';
    while (*p == ' ') { p++; indent++; }
    if (*p == '\0') continue;
    if (strncmp(p, "In [", 4) == 0)
    {
      char *e = strstr(p, "]:");
      char *x = e ? e + 2 : p + strlen(p);
      while (*x == ' ') x++;
      depth = 0;
      skip = (strncmp(x, "odrv0", 5) != 0);
      if (!skip) snprintf(prefix, sizeof(prefix), "%s", (x[5] == '.') ? x + 6 : "");
      continue;
    }
    if (strncmp(p, "Out[", 4) == 0 || skip) continue;
    while (depth > 0 && ind[depth - 1] >= indent) depth--;
    colon = strchr(p, ':');
    if (colon == NULL) continue;
    {
      char *paren = strchr(p, '(');
      if (paren != NULL && paren < colon) continue;     /* method */
    }
    *colon = '\0';
    {
      char *val = colon + 1;
      char path[96];
      size_t n;
      while (*val == ' ') val++;
      if (*val == '\0')
      {
        if (depth < 8)
        {
          ind[depth] = indent;
          names[depth][0] = '\0';
          strncat(names[depth], p, sizeof(names[depth]) - 1);
          depth++;
        }
        continue;
      }
      if (strcmp(val, "...") == 0) continue;
      snprintf(path, sizeof(path), "%s", prefix);
      for (int k = 0; k < depth; k++)
      {
        n = strlen(path);
        snprintf(path + n, sizeof(path) - n, "%s%s", n ? "." : "", names[k]);
      }
      n = strlen(path);
      snprintf(path + n, sizeof(path) - n, "%s%s", n ? "." : "", p);
      put(path, val);
    }
  }
  fclose(f);
  return NKV;
}

static const char *get(const char *path)
{
  for (int i = 0; i < NKV; i++) if (strcmp(KV[i].path, path) == 0) return KV[i].value;
  return NULL;
}

static bool num(const char *path, float *out)
{
  const char *v = get(path);
  if (v == NULL) return false;
  if (strncmp(v, "True", 4) == 0)  { *out = 1.0f; return true; }
  if (strncmp(v, "False", 5) == 0) { *out = 0.0f; return true; }
  *out = strtof(v, NULL);
  return true;
}

TEST(test_dump_parses)
{
  CHECK(load_dump() > 150);
  CHECK(get("vbus_voltage") != NULL);
  CHECK(get("axis0.config.enable_watchdog") != NULL);
  CHECK(get("axis0.controller.config.control_mode") != NULL);
}

TEST(test_dump_has_every_precheck_path)
{
  ffb_pc_table_t t;
  load_dump();
  ffb_pc_init(&t, 0, true);
  for (int i = 0; i < FFB_PC_COUNT; i++)
  {
    if (get(ffb_pc_path(&t, (ffb_pc_item_t)i)) == NULL)
    {
      printf("    precheck reads '%s' but it is not in the dump\n", ffb_pc_path(&t, (ffb_pc_item_t)i));
      CHECK(0);
    }
  }
  /* the write targets of ffb_cmd.c exist too */
  CHECK(get("axis0.controller.input_torque") != NULL);
  CHECK(get("axis0.requested_state") != NULL);
}

static void table_from_dump(ffb_pc_table_t *t, uint32_t now)
{
  ffb_pc_init(t, 0, true);
  for (int i = 0; i < FFB_PC_COUNT; i++)
  {
    float v;
    if (num(ffb_pc_path(t, (ffb_pc_item_t)i), &v)) ffb_pc_store(t, (ffb_pc_item_t)i, v, now);
  }
}

TEST(test_dump_real_values_block_arming_in_order)
{
  ffb_pc_table_t t;
  ffb_limits_t lim;
  ffb_pc_context_t c = { true, false, 0U, true, true, true };
  load_dump();
  ffb_limits_default(&lim);
  table_from_dump(&t, 10);
  CHECK(ffb_pc_complete(&t, 10, 1000));
  /* real saved state: one problem after the other, in the firmware's order */
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_WATCHDOG_DISABLED);
  ffb_pc_store(&t, FFB_PC_ENABLE_WATCHDOG, 1.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_WATCHDOG_TIMEOUT_RANGE); /* 0.0 */
  ffb_pc_store(&t, FFB_PC_WATCHDOG_TIMEOUT, 0.25f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_CONTROL_MODE);            /* 3 */
  ffb_pc_store(&t, FFB_PC_CONTROL_MODE, 1.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_CURRENT_LIM_HIGH);        /* 12 */
  ffb_pc_store(&t, FFB_PC_CURRENT_LIM, 4.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_VEL_LIMIT_HIGH);          /* 50 */
  ffb_pc_store(&t, FFB_PC_VEL_LIMIT, 2.0f, 10);
  /* torque_constant 0.04 and motor.is_calibrated True are accepted as saved */
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_ENCODER_NOT_READY);       /* no belt */
  ffb_pc_store(&t, FFB_PC_ENCODER_READY, 1.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_AXIS_ERROR);              /* 256 */
  ffb_pc_store(&t, FFB_PC_AXIS_ERROR, 0.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_MOTOR_ERROR);             /* 16 */
  ffb_pc_store(&t, FFB_PC_MOTOR_ERROR, 0.0f, 10);
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_ENCODER_ERROR);           /* 4 */
  ffb_pc_store(&t, FFB_PC_ENCODER_ERROR, 0.0f, 10);
  /* current_state 1 (IDLE) and vbus 24.19 V are fine as saved */
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_OK);
  /* ...but the profile gate still holds while the profile has TBD entries */
  c.profile_safety_tbd = 5;
  CHECK_EQ_INT(ffb_precheck_evaluate(&t, &lim, &c, 10, 1000), FFB_REASON_PROFILE_TBD);
}

TEST(test_dump_real_errors_fault_the_monitor)
{
  ffb_pc_table_t t;
  ffb_limits_t lim;
  load_dump();
  ffb_limits_default(&lim);
  table_from_dump(&t, 10);
  CHECK_EQ_INT(ffb_monitor_evaluate(&t, &lim, 10, 400, false), FFB_FAULT_ODRIVE_ERROR);
}

void run_ffb_dump_tests(void)
{
  printf("ffb vs the real ODrive dump (docs/odrive_dump.txt)\n");
  RUN(test_dump_parses);
  RUN(test_dump_has_every_precheck_path);
  RUN(test_dump_real_values_block_arming_in_order);
  RUN(test_dump_real_errors_fault_the_monitor);
}
