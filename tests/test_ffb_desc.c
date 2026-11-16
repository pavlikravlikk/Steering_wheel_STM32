/* Self-check of the FFB report descriptor with a small HID item parser:
 * structure, the Linux hid-pidff requirements (required reports, field
 * usages, nested arrays with Logical Minimum 1, time units) and every report
 * size / field offset against ffb_hid_desc.h and the parser in ffb_pid.c. */
#include "test_runner.h"

#include <string.h>

#include "ffb_hid_desc.h"

#define MAX_FIELDS 160
#define MAX_USAGES 16
#define MAX_DEPTH  8

typedef struct
{
  uint8_t  type;                 /* 1 input, 2 output, 3 feature */
  uint8_t  rid;
  uint32_t bit_offset;           /* from the first byte after the Report ID */
  uint32_t size, count;
  int32_t  lmin, lmax;
  uint8_t  flags;
  uint32_t usages[MAX_USAGES];   /* (page << 16) | id */
  int      nusages;
  uint32_t coll[MAX_DEPTH];      /* collection usages, outermost first */
  int      depth;
  uint32_t unit;
  int      unit_exp;
} field_t;

static field_t F[MAX_FIELDS];
static int NF;
static uint32_t BITS[4][256];
static int PARSE_END, DEPTH_END;

static int32_t sval(uint32_t v, int size)
{
  if (size == 1) return (int8_t)v;
  if (size == 2) return (int16_t)v;
  return (int32_t)v;
}

static void parse(const uint8_t *d, int n)
{
  uint32_t page = 0, unit = 0, size = 0, count = 0;
  int32_t lmin = 0, lmax = 0;
  int unit_exp = 0;
  uint8_t rid = 0;
  uint32_t usages[MAX_USAGES];
  int nus = 0;
  uint32_t umin = 0;
  uint32_t coll[MAX_DEPTH];
  int depth = 0;
  int i = 0;

  NF = 0;
  memset(BITS, 0, sizeof(BITS));
  while (i < n)
  {
    const uint8_t prefix = d[i];
    int sz = prefix & 3;
    uint32_t v = 0;
    if (sz == 3) sz = 4;
    for (int k = 0; k < sz && i + 1 + k < n; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
    switch (prefix & 0xFC)
    {
      /* global */
      case 0x04: page = v; break;
      case 0x14: lmin = sval(v, sz); break;
      case 0x24: lmax = sval(v, sz); break;
      case 0x54: unit_exp = (v & 0x8) ? (int)v - 16 : (int)v; break;
      case 0x64: unit = v; break;
      case 0x74: size = v; break;
      case 0x84: rid = (uint8_t)v; break;
      case 0x94: count = v; break;
      /* local */
      case 0x08:
        if (nus < MAX_USAGES) usages[nus++] = (sz == 4) ? v : ((page << 16) | v);
        break;
      case 0x18: umin = (page << 16) | v; break;
      case 0x28:
        for (uint32_t u = umin; u <= ((page << 16) | v) && nus < MAX_USAGES; u++) usages[nus++] = u;
        break;
      /* main */
      case 0xA0:
        if (depth < MAX_DEPTH) coll[depth++] = nus ? usages[0] : 0;
        nus = 0;
        break;
      case 0xC0:
        depth--;
        nus = 0;
        break;
      case 0x80: case 0x90: case 0xB0:
      {
        const uint8_t type = (prefix & 0xFC) == 0x80 ? 1 : ((prefix & 0xFC) == 0x90 ? 2 : 3);
        if (NF < MAX_FIELDS)
        {
          field_t *f = &F[NF++];
          memset(f, 0, sizeof(*f));
          f->type = type; f->rid = rid; f->bit_offset = BITS[type][rid];
          f->size = size; f->count = count; f->lmin = lmin; f->lmax = lmax; f->flags = (uint8_t)v;
          memcpy(f->usages, usages, sizeof(uint32_t) * (size_t)nus); f->nusages = nus;
          memcpy(f->coll, coll, sizeof(uint32_t) * (size_t)depth); f->depth = depth;
          f->unit = unit; f->unit_exp = unit_exp;
        }
        BITS[type][rid] += size * count;
        nus = 0;
        break;
      }
      default: break;
    }
    i += 1 + sz;
  }
  PARSE_END = i;
  DEPTH_END = depth;
}

#define PID(x) (0x000F0000U | (x))

/* the field of report (type,rid) that carries usage u (value or array) */
static const field_t *find_usage(uint8_t type, uint8_t rid, uint32_t u, int *index)
{
  for (int k = 0; k < NF; k++)
  {
    if (F[k].type != type || F[k].rid != rid) continue;
    for (int j = 0; j < F[k].nusages; j++)
      if (F[k].usages[j] == u) { if (index) *index = j; return &F[k]; }
  }
  return NULL;
}

/* rid of the report whose fields sit in a collection named u */
static int report_of_collection(uint8_t type, uint32_t u)
{
  for (int k = 0; k < NF; k++)
    if (F[k].type == type)
      for (int c = 0; c < F[k].depth; c++)
        if (F[k].coll[c] == u) return F[k].rid;
  return -1;
}

/* the array field directly inside a nested collection named u, in report rid */
static const field_t *array_in(uint8_t type, uint8_t rid, uint32_t u)
{
  for (int k = 0; k < NF; k++)
    if (F[k].type == type && F[k].rid == rid && F[k].depth > 0 && F[k].coll[F[k].depth - 1] == u)
      return &F[k];
  return NULL;
}

/* byte offset (incl. Report ID) of usage u, for a value field */
static long byte_of(uint8_t type, uint8_t rid, uint32_t u)
{
  int j = 0;
  const field_t *f = find_usage(type, rid, u, &j);
  if (f == NULL) return -1;
  if ((f->flags & 0x02) == 0) j = 0;             /* array: one element */
  return 1 + (long)(f->bit_offset + (uint32_t)j * f->size) / 8;
}

static void setup(void)
{
  parse(ffb_hid_report_desc, ffb_hid_report_desc_size);
}

TEST(test_desc_structure)
{
  setup();
  CHECK_EQ_INT(PARSE_END, ffb_hid_report_desc_size);
  CHECK_EQ_INT(DEPTH_END, 0);
  CHECK(ffb_hid_report_desc_size > 42 && ffb_hid_report_desc_size < 1024);
  CHECK(NF > 40);
}

TEST(test_desc_report_sizes_match_header)
{
  static const struct { uint8_t type, rid; unsigned len; } r[] = {
    { 1, FFB_RID_JOYSTICK, FFB_LEN_JOYSTICK }, { 1, FFB_RID_PID_STATE, FFB_LEN_PID_STATE },
    { 2, FFB_RID_SET_EFFECT, FFB_LEN_SET_EFFECT }, { 2, FFB_RID_SET_ENVELOPE, FFB_LEN_SET_ENVELOPE },
    { 2, FFB_RID_SET_CONDITION, FFB_LEN_SET_CONDITION }, { 2, FFB_RID_SET_PERIODIC, FFB_LEN_SET_PERIODIC },
    { 2, FFB_RID_SET_CONSTANT, FFB_LEN_SET_CONSTANT }, { 2, FFB_RID_SET_RAMP, FFB_LEN_SET_RAMP },
    { 2, FFB_RID_EFFECT_OP, FFB_LEN_EFFECT_OP }, { 2, FFB_RID_BLOCK_FREE, FFB_LEN_BLOCK_FREE },
    { 2, FFB_RID_DEVICE_CONTROL, FFB_LEN_DEVICE_CONTROL }, { 2, FFB_RID_DEVICE_GAIN, FFB_LEN_DEVICE_GAIN },
    { 3, FFB_RID_CREATE_EFFECT, FFB_LEN_CREATE_EFFECT }, { 3, FFB_RID_BLOCK_LOAD, FFB_LEN_BLOCK_LOAD },
    { 3, FFB_RID_POOL, FFB_LEN_POOL }, { 3, FFB_RID_STATUS, FFB_LEN_STATUS },
  };
  setup();
  for (size_t i = 0; i < sizeof(r) / sizeof(r[0]); i++)
  {
    const uint32_t bits = BITS[r[i].type][r[i].rid];
    if (bits % 8 != 0 || 1 + bits / 8 != r[i].len)
      printf("    report type %u id 0x%02X: %u bits, header says %u bytes\n", r[i].type, r[i].rid, bits, r[i].len);
    CHECK_EQ_INT(bits % 8, 0);
    CHECK_EQ_INT(1 + bits / 8, r[i].len);
    CHECK(r[i].len <= FFB_LEN_MAX);
  }
  CHECK(BITS[0][0] == 0 && BITS[1][0] == 0 && BITS[2][0] == 0 && BITS[3][0] == 0);  /* all have IDs */
}

TEST(test_desc_joystick_report)
{
  const field_t *x;
  setup();
  x = find_usage(1, FFB_RID_JOYSTICK, 0x00010030, NULL);
  CHECK(x != NULL);
  if (x == NULL) return;
  CHECK_EQ_INT(x->size, 16);
  CHECK_EQ_INT(x->count, 3);
  CHECK_EQ_INT(x->lmax, 65535);
  CHECK(find_usage(1, FFB_RID_JOYSTICK, 0x00010031, NULL) == x);
  CHECK(find_usage(1, FFB_RID_JOYSTICK, 0x00010032, NULL) == x);
  CHECK(find_usage(1, FFB_RID_JOYSTICK, 0x00090010, NULL) != NULL);   /* button 16 */
  CHECK_EQ_INT(byte_of(1, FFB_RID_JOYSTICK, 0x00010030), 1);          /* same payload as the */
  CHECK_EQ_INT(byte_of(1, FFB_RID_JOYSTICK, 0x00090001), 7);          /* default 8-byte report */
}

TEST(test_desc_linux_required_reports)
{
  static const struct { uint8_t type; uint32_t u; uint8_t rid; } req[] = {
    { 2, 0x21, FFB_RID_SET_EFFECT }, { 2, 0x77, FFB_RID_EFFECT_OP }, { 2, 0x7D, FFB_RID_DEVICE_GAIN },
    { 3, 0x7F, FFB_RID_POOL }, { 3, 0x89, FFB_RID_BLOCK_LOAD }, { 2, 0x90, FFB_RID_BLOCK_FREE },
    { 2, 0x96, FFB_RID_DEVICE_CONTROL }, { 3, 0xAB, FFB_RID_CREATE_EFFECT },
    /* optional ones, all present */
    { 2, 0x5A, FFB_RID_SET_ENVELOPE }, { 2, 0x5F, FFB_RID_SET_CONDITION }, { 2, 0x6E, FFB_RID_SET_PERIODIC },
    { 2, 0x73, FFB_RID_SET_CONSTANT }, { 2, 0x74, FFB_RID_SET_RAMP },
  };
  setup();
  for (size_t i = 0; i < sizeof(req) / sizeof(req[0]); i++)
  {
    const int rid = report_of_collection(req[i].type, PID(req[i].u));
    if (rid != req[i].rid) printf("    report usage 0x%02X: rid %d, want 0x%02X\n", req[i].u, rid, req[i].rid);
    CHECK_EQ_INT(rid, req[i].rid);
  }
}

/* value fields: one usage per element (Linux checks maxusage == report_count) */
static void check_values(uint8_t type, uint8_t rid, const uint32_t *us, size_t n)
{
  for (size_t i = 0; i < n; i++)
  {
    const field_t *f = find_usage(type, rid, PID(us[i]), NULL);
    if (f == NULL) { printf("    rid 0x%02X: usage 0x%02X missing\n", rid, us[i]); CHECK(0); continue; }
    CHECK((f->flags & 0x02) != 0);                    /* variable */
    CHECK_EQ_INT(f->nusages, (int)f->count);
  }
}

TEST(test_desc_linux_field_usages)
{
  static const uint32_t set_effect[] = { 0x22, 0x50, 0x52, 0x53, 0x54, 0x56, 0xA7 };
  static const uint32_t env[] = { 0x22, 0x5B, 0x5C, 0x5D, 0x5E };
  static const uint32_t cond[] = { 0x22, 0x23, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65 };
  static const uint32_t per[] = { 0x22, 0x70, 0x6F, 0x71, 0x72 };
  static const uint32_t con[] = { 0x22, 0x70 };
  static const uint32_t ramp[] = { 0x22, 0x75, 0x76 };
  static const uint32_t op[] = { 0x22, 0x7C };
  static const uint32_t bfree[] = { 0x22 };
  static const uint32_t gain[] = { 0x7E };
  static const uint32_t bload[] = { 0x22, 0xAC };
  static const uint32_t pool[] = { 0x80, 0x83, 0xA9, 0xAA };
  setup();
  check_values(2, FFB_RID_SET_EFFECT, set_effect, sizeof(set_effect) / 4);
  check_values(2, FFB_RID_SET_ENVELOPE, env, sizeof(env) / 4);
  check_values(2, FFB_RID_SET_CONDITION, cond, sizeof(cond) / 4);
  check_values(2, FFB_RID_SET_PERIODIC, per, sizeof(per) / 4);
  check_values(2, FFB_RID_SET_CONSTANT, con, sizeof(con) / 4);
  check_values(2, FFB_RID_SET_RAMP, ramp, sizeof(ramp) / 4);
  check_values(2, FFB_RID_EFFECT_OP, op, sizeof(op) / 4);
  check_values(2, FFB_RID_BLOCK_FREE, bfree, sizeof(bfree) / 4);
  check_values(2, FFB_RID_DEVICE_GAIN, gain, sizeof(gain) / 4);
  check_values(3, FFB_RID_BLOCK_LOAD, bload, sizeof(bload) / 4);
  check_values(3, FFB_RID_POOL, pool, sizeof(pool) / 4);
}

static void check_array(uint8_t type, uint8_t rid, uint32_t coll, const uint32_t *us, int n)
{
  const field_t *f = array_in(type, rid, PID(coll));
  CHECK(f != NULL);
  if (f == NULL) { printf("    array 0x%02X in rid 0x%02X missing\n", coll, rid); return; }
  CHECK_EQ_INT(f->flags & 0x02, 0);                   /* array */
  CHECK_EQ_INT(f->lmin, 1);
  CHECK_EQ_INT(f->lmax, n);
  CHECK_EQ_INT(f->nusages, n);
  for (int i = 0; i < n && i < f->nusages; i++) CHECK_EQ_INT(f->usages[i], PID(us[i]));
}

TEST(test_desc_linux_special_arrays)
{
  static const uint32_t types[] = { 0x26, 0x27, 0x30, 0x31, 0x32, 0x33, 0x34, 0x40, 0x41, 0x42, 0x43 };
  static const uint32_t ops[] = { 0x79, 0x7A, 0x7B };
  static const uint32_t dc[] = { 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C };
  static const uint32_t bl[] = { 0x8C, 0x8D, 0x8E };
  const field_t *dir;
  setup();
  /* same order in Set Effect and Create New Effect, and = FFB_ET_* values */
  check_array(2, FFB_RID_SET_EFFECT, 0x25, types, 11);
  check_array(3, FFB_RID_CREATE_EFFECT, 0x25, types, 11);
  CHECK_EQ_INT(FFB_ET_COUNT, 11);
  CHECK_EQ_INT(types[FFB_ET_CONSTANT - 1], 0x26);
  CHECK_EQ_INT(types[FFB_ET_SPRING - 1], 0x40);
  CHECK_EQ_INT(types[FFB_ET_FRICTION - 1], 0x43);
  check_array(2, FFB_RID_EFFECT_OP, 0x78, ops, 3);
  CHECK_EQ_INT(FFB_OP_STOP, 3);
  check_array(2, FFB_RID_DEVICE_CONTROL, 0x96, dc, 6);
  CHECK_EQ_INT(FFB_DC_RESET, 4);
  check_array(3, FFB_RID_BLOCK_LOAD, 0x8B, bl, 3);
  CHECK_EQ_INT(FFB_BL_SUCCESS, 1);
  /* direction: a field inside a 0x57 collection of Set Effect */
  dir = array_in(2, FFB_RID_SET_EFFECT, PID(0x57));
  CHECK(dir != NULL);
  if (dir != NULL)
  {
    CHECK_EQ_INT(dir->lmax, FFB_DIR_MAX);
    CHECK_EQ_INT(dir->size, 16);
  }
}

TEST(test_desc_time_fields_are_milliseconds)
{
  static const struct { uint8_t rid; uint32_t u; } t[] = {
    { FFB_RID_SET_EFFECT, 0x50 }, { FFB_RID_SET_EFFECT, 0x54 }, { FFB_RID_SET_EFFECT, 0xA7 },
    { FFB_RID_SET_ENVELOPE, 0x5C }, { FFB_RID_SET_ENVELOPE, 0x5E }, { FFB_RID_SET_PERIODIC, 0x72 },
  };
  setup();
  for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
  {
    const field_t *f = find_usage(2, t[i].rid, PID(t[i].u), NULL);
    CHECK(f != NULL);
    if (f == NULL) continue;
    CHECK_EQ_INT(f->unit, 0x1001);
    CHECK_EQ_INT(f->unit_exp, -3);
  }
  /* magnitudes are not time-scaled */
  CHECK_EQ_INT(find_usage(2, FFB_RID_SET_CONSTANT, PID(0x70), NULL)->unit_exp, 0);
}

TEST(test_desc_block_index_range_everywhere)
{
  setup();
  for (int k = 0; k < NF; k++)
  {
    for (int j = 0; j < F[k].nusages; j++)
    {
      if (F[k].usages[j] == PID(0x22))
      {
        CHECK_EQ_INT(F[k].lmin, 1);
        CHECK_EQ_INT(F[k].lmax, FFB_MAX_EFFECTS);
      }
    }
  }
  CHECK(FFB_MAX_EFFECTS <= 64);   /* Linux caps the number of effects at 64 */
}

/* The byte offsets used by ffb_pid.c must be the descriptor's offsets. */
TEST(test_desc_offsets_match_parser)
{
  static const struct { uint8_t type, rid; uint32_t u; long byte; } o[] = {
    { 2, FFB_RID_SET_EFFECT, 0x22, 1 }, { 2, FFB_RID_SET_EFFECT, 0x26, 2 },
    { 2, FFB_RID_SET_EFFECT, 0x50, 3 }, { 2, FFB_RID_SET_EFFECT, 0x54, 5 },
    { 2, FFB_RID_SET_EFFECT, 0x51, 7 }, { 2, FFB_RID_SET_EFFECT, 0xA7, 9 },
    { 2, FFB_RID_SET_EFFECT, 0x52, 11 }, { 2, FFB_RID_SET_EFFECT, 0x53, 12 },
    { 2, FFB_RID_SET_EFFECT, 0x56, 13 },
    { 2, FFB_RID_SET_ENVELOPE, 0x5B, 2 }, { 2, FFB_RID_SET_ENVELOPE, 0x5D, 4 },
    { 2, FFB_RID_SET_ENVELOPE, 0x5C, 6 }, { 2, FFB_RID_SET_ENVELOPE, 0x5E, 8 },
    { 2, FFB_RID_SET_CONDITION, 0x23, 2 }, { 2, FFB_RID_SET_CONDITION, 0x60, 3 },
    { 2, FFB_RID_SET_CONDITION, 0x61, 5 }, { 2, FFB_RID_SET_CONDITION, 0x62, 7 },
    { 2, FFB_RID_SET_CONDITION, 0x63, 9 }, { 2, FFB_RID_SET_CONDITION, 0x64, 11 },
    { 2, FFB_RID_SET_CONDITION, 0x65, 13 },
    { 2, FFB_RID_SET_PERIODIC, 0x70, 2 }, { 2, FFB_RID_SET_PERIODIC, 0x6F, 4 },
    { 2, FFB_RID_SET_PERIODIC, 0x71, 6 }, { 2, FFB_RID_SET_PERIODIC, 0x72, 8 },
    { 2, FFB_RID_SET_CONSTANT, 0x70, 2 },
    { 2, FFB_RID_SET_RAMP, 0x75, 2 }, { 2, FFB_RID_SET_RAMP, 0x76, 4 },
    { 2, FFB_RID_EFFECT_OP, 0x22, 1 }, { 2, FFB_RID_EFFECT_OP, 0x79, 2 }, { 2, FFB_RID_EFFECT_OP, 0x7C, 3 },
    { 2, FFB_RID_BLOCK_FREE, 0x22, 1 }, { 2, FFB_RID_DEVICE_CONTROL, 0x97, 1 },
    { 2, FFB_RID_DEVICE_GAIN, 0x7E, 1 }, { 3, FFB_RID_CREATE_EFFECT, 0x26, 1 },
    { 3, FFB_RID_BLOCK_LOAD, 0x22, 1 }, { 3, FFB_RID_BLOCK_LOAD, 0x8C, 2 }, { 3, FFB_RID_BLOCK_LOAD, 0xAC, 3 },
    { 3, FFB_RID_POOL, 0x80, 1 }, { 3, FFB_RID_POOL, 0x83, 3 }, { 3, FFB_RID_POOL, 0xA9, 4 },
    { 1, FFB_RID_PID_STATE, 0x9F, 1 }, { 1, FFB_RID_PID_STATE, 0x94, 2 },
  };
  setup();
  for (size_t i = 0; i < sizeof(o) / sizeof(o[0]); i++)
  {
    const long b = byte_of(o[i].type, o[i].rid, PID(o[i].u));
    if (b != o[i].byte) printf("    rid 0x%02X usage 0x%02X at byte %ld, parser uses %ld\n",
                               o[i].rid, o[i].u, b, o[i].byte);
    CHECK_EQ_INT(b, o[i].byte);
  }
  /* direction enable is bit 1 of byte 13 (axes enable X = bit 0) */
  {
    const field_t *de = find_usage(2, FFB_RID_SET_EFFECT, PID(0x56), NULL);
    CHECK(de != NULL);
    if (de != NULL) CHECK_EQ_INT(de->bit_offset % 8, 1);
  }
  /* direction (Ordinal 1 in the 0x57 collection) at byte 14 */
  CHECK_EQ_INT(byte_of(2, FFB_RID_SET_EFFECT, 0x000A0001), 14);
}

TEST(test_desc_vendor_status_feature)
{
  const field_t *f;
  setup();
  f = find_usage(3, FFB_RID_STATUS, 0xFF000002, NULL);
  CHECK(f != NULL);
  if (f != NULL) CHECK_EQ_INT(f->count, FFB_LEN_STATUS - 1);
}

void run_ffb_desc_tests(void)
{
  printf("ffb HID PID descriptor\n");
  RUN(test_desc_structure);
  RUN(test_desc_report_sizes_match_header);
  RUN(test_desc_joystick_report);
  RUN(test_desc_linux_required_reports);
  RUN(test_desc_linux_field_usages);
  RUN(test_desc_linux_special_arrays);
  RUN(test_desc_time_fields_are_milliseconds);
  RUN(test_desc_block_index_range_everywhere);
  RUN(test_desc_offsets_match_parser);
  RUN(test_desc_vendor_status_feature);
}
