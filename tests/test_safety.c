/* Safety regression checks on the firmware SOURCE TREE.
 *
 * Rule (docs/FFB_SAFETY.md, hard rule 1): everything that can write to the
 * ODrive lives in Core/Src/ffb + Core/Inc/ffb and is compiled only with
 * -DWHEEL_FFB=ON. Therefore:
 *  - no firmware file OUTSIDE the ffb/ directories mentions an ODrive write
 *    target or write command;
 *  - outside ffb/, every reference to the FFB code sits inside an
 *    "#if WHEEL_FFB" branch (and never in its "#else");
 *  - CMake compiles ffb/ only under if(WHEEL_FFB), WHEEL_FFB defaults OFF and
 *    WHEEL_FFB_ARM_ALLOWED defaults 0;
 *  - inside ffb/, write command literals exist only in the allowlist file
 *    ffb_cmd.c, and nothing there can write configuration or save it;
 *  - centering_law.c is not part of the firmware build.
 * If one of these fails, someone added a path that could command the motor
 * in the default build: that needs an explicit, documented decision first. */
#define _DEFAULT_SOURCE
#include "test_runner.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define FW_DIR "../steeringwheel_firmware/"

static char *slurp(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (buf != NULL)
  {
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
  }
  fclose(f);
  return buf;
}

static const char *const k_forbidden[] = {
  "requested_state", "input_torque", "input_pos", "input_vel", ".startup_",
  "save_configuration", "enable_watchdog", "watchdog_timeout", "control_mode",
  "current_lim", "torque_lim", "vel_limit",
  /* ASCII write / motion / system commands as string literals */
  "\"w ", "\"c ", "\"q ", "\"p ", "\"v ", "\"t ", "\"u ",
  "\"sc\\n", "\"ss\\n", "\"sb\\n", "\"se\\n", "\"sr\\n", "\"es\\n",
};
#define NFORBIDDEN (sizeof(k_forbidden) / sizeof(k_forbidden[0]))

typedef void (*file_fn)(const char *path, const char *src, void *ctx);

static int for_each_source(const char *dir, file_fn fn, void *ctx)
{
  int files = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  CHECK(d != NULL);
  if (d == NULL) return 0;
  while ((e = readdir(d)) != NULL)
  {
    size_t n = strlen(e->d_name);
    if (n < 3 || (strcmp(e->d_name + n - 2, ".c") != 0 && strcmp(e->d_name + n - 2, ".h") != 0))
      continue;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    char *src = slurp(path);
    CHECK(src != NULL);
    if (src == NULL) continue;
    files++;
    fn(path, src, ctx);
    free(src);
  }
  closedir(d);
  return files;
}

/* ---------------------------------------------------------------------- */
static void no_tokens(const char *path, const char *src, void *ctx)
{
  (void)ctx;
  for (size_t i = 0; i < NFORBIDDEN; i++)
  {
    if (strstr(src, k_forbidden[i]) != NULL)
    {
      printf("    forbidden token '%s' in %s\n", k_forbidden[i], path);
      CHECK(0);
    }
  }
}

TEST(test_no_odrive_write_tokens_outside_ffb)
{
  /* readdir is not recursive: Core/Src/ffb and Core/Inc/ffb are skipped here
   * and checked separately below. */
  int n = for_each_source(FW_DIR "Core/Src", no_tokens, NULL)
        + for_each_source(FW_DIR "Core/Inc", no_tokens, NULL)
        + for_each_source(FW_DIR "USB_DEVICE/App", no_tokens, NULL)
        + for_each_source(FW_DIR "USB_DEVICE/Target", no_tokens, NULL);
  CHECK(n > 20);
}

/* ---------------------------------------------------------------------- */
/* Outside ffb/: FFB references only in the true branch of #if WHEEL_FFB.   */
/* Tracks #if/#ifdef/#ifndef/#elif/#else/#endif nesting line by line.       */
static const char *const k_ffb_refs[] = { "ffb_", "usbd_ffb", "_send_ffb", "ffb/" };

static void ffb_refs_guarded(const char *path, const char *src, void *ctx)
{
  int depth = 0;
  int state[32];             /* per level: 1 = FFB branch, -1 = non-FFB branch, 0 = other */
  int lineno = 0;
  const char *p = src;
  (void)ctx;
  while (*p != '\0')
  {
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char line[512];
    const char *t;
    lineno++;
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    memcpy(line, p, len);
    line[len] = '\0';
    t = line;
    while (*t == ' ' || *t == '\t') t++;
    if (*t == '#')
    {
      t++;
      while (*t == ' ') t++;
      if (strncmp(t, "if", 2) == 0)                 /* #if, #ifdef, #ifndef */
      {
        int s = 0;
        if (strstr(t, "WHEEL_FFB") != NULL && strstr(t, "WHEEL_FFB_ARM") == NULL)
          s = (strstr(t, "!WHEEL_FFB") != NULL || strncmp(t, "ifndef", 6) == 0) ? -1 : 1;
        if (depth < 32) state[depth] = s;
        depth++;
      }
      else if (strncmp(t, "else", 4) == 0 && depth > 0 && depth <= 32)
      {
        state[depth - 1] = -state[depth - 1];
      }
      else if (strncmp(t, "elif", 4) == 0 && depth > 0 && depth <= 32)
      {
        state[depth - 1] = (state[depth - 1] != 0) ? -1 : 0;   /* conservative */
      }
      else if (strncmp(t, "endif", 5) == 0 && depth > 0)
      {
        depth--;
      }
    }
    {
      bool in_ffb = false, in_non_ffb = false;
      for (int k = 0; k < depth && k < 32; k++)
      {
        if (state[k] == 1) in_ffb = true;
        if (state[k] == -1) in_non_ffb = true;
      }
      for (size_t i = 0; i < sizeof(k_ffb_refs) / sizeof(k_ffb_refs[0]); i++)
      {
        if (strstr(line, k_ffb_refs[i]) != NULL && (!in_ffb || in_non_ffb))
        {
          printf("    %s:%d: '%s' outside #if WHEEL_FFB\n", path, lineno, k_ffb_refs[i]);
          CHECK(0);
        }
      }
    }
    p = eol ? eol + 1 : p + strlen(p);
  }
  CHECK_EQ_INT(depth, 0);
}

TEST(test_ffb_references_only_under_wheel_ffb)
{
  for_each_source(FW_DIR "Core/Src", ffb_refs_guarded, NULL);
  for_each_source(FW_DIR "Core/Inc", ffb_refs_guarded, NULL);
  for_each_source(FW_DIR "USB_DEVICE/App", ffb_refs_guarded, NULL);
}

/* ---------------------------------------------------------------------- */
/* Inside ffb/: the write path exists (sanity) but only in the allowlist.  */
typedef struct { int files; int literal_files; bool saw_allowlist; } ffb_scan_t;

/* Copy of @p src without C comments (string literals are kept). */
static char *strip_comments(const char *src)
{
  char *out = malloc(strlen(src) + 1);
  char *o = out;
  bool in_str = false;
  if (out == NULL) return NULL;
  while (*src != '\0')
  {
    if (in_str)
    {
      if (*src == '\\' && src[1] != '\0') { *o++ = *src++; *o++ = *src++; continue; }
      if (*src == '"') in_str = false;
      *o++ = *src++;
    }
    else if (src[0] == '/' && src[1] == '*')
    {
      const char *e = strstr(src + 2, "*/");
      src = e ? e + 2 : src + strlen(src);
      *o++ = ' ';
    }
    else if (src[0] == '/' && src[1] == '/')
    {
      while (*src != '\0' && *src != '\n') src++;
    }
    else
    {
      if (*src == '"') in_str = true;
      *o++ = *src++;
    }
  }
  *o = '\0';
  return out;
}

static void ffb_dir_rules_code(const char *path, const char *src, void *ctx);

static void ffb_dir_rules(const char *path, const char *src, void *ctx)
{
  char *code = strip_comments(src);
  CHECK(code != NULL);
  if (code != NULL)
  {
    ffb_dir_rules_code(path, code, ctx);
    free(code);
  }
}

static void ffb_dir_rules_code(const char *path, const char *src, void *ctx)
{
  ffb_scan_t *s = (ffb_scan_t *)ctx;
  const bool is_cmd = strstr(path, "/ffb_cmd.") != NULL;
  static const char *const never[] = { "save_configuration", ".startup_", "erase_configuration",
                                       "\"ss", "\"se", "\"sr", "\"sc", "reboot" };
  s->files++;
  for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++)
  {
    if (strstr(src, never[i]) != NULL)
    {
      printf("    '%s' must never appear in %s\n", never[i], path);
      CHECK(0);
    }
  }
  /* write command literals only in ffb_cmd.c */
  if (strstr(src, "\"w ") != NULL || strstr(src, "\"w axis") != NULL || strstr(src, "\"u ") != NULL)
  {
    s->literal_files++;
    if (!is_cmd)
    {
      printf("    write command literal outside ffb_cmd.c: %s\n", path);
      CHECK(0);
    }
  }
  /* config property names may appear only as READ paths (ffb_precheck.c) */
  if (!strstr(path, "ffb_precheck.c"))
  {
    static const char *const cfg[] = { "current_lim", "vel_limit", "control_mode", "enable_watchdog",
                                        "watchdog_timeout", "torque_constant" };
    for (size_t i = 0; i < sizeof(cfg) / sizeof(cfg[0]); i++)
    {
      if (strstr(src, cfg[i]) != NULL && strstr(src, "\"axis") != NULL)
      {
        printf("    ODrive config path '%s' outside ffb_precheck.c: %s\n", cfg[i], path);
        CHECK(0);
      }
    }
  }
  if (is_cmd && strstr(src, "ffb_cmd_is_allowed") != NULL) s->saw_allowlist = true;
}

TEST(test_ffb_dir_write_path_is_confined)
{
  ffb_scan_t s = { 0, 0, false };
  for_each_source(FW_DIR "Core/Src/ffb", ffb_dir_rules, &s);
  for_each_source(FW_DIR "Core/Inc/ffb", ffb_dir_rules, &s);
  CHECK(s.files >= 20);
  CHECK(s.literal_files >= 1);       /* the scanner does see the write path */
  CHECK(s.saw_allowlist);
}

TEST(test_uart_ffb_entry_point_checks_allowlist)
{
  char *src = slurp(FW_DIR "Core/Src/odrive_uart.c");
  const char *fn;
  CHECK(src != NULL);
  if (src == NULL) return;
  fn = strstr(src, "bool odrive_uart_send_ffb(");
  CHECK(fn != NULL);
  if (fn != NULL)
  {
    const char *body_end = strstr(fn, "\n}\n");
    const char *chk = strstr(fn, "ffb_cmd_is_allowed(");
    CHECK(chk != NULL && body_end != NULL && chk < body_end);
  }
  free(src);
}

/* ---------------------------------------------------------------------- */
TEST(test_build_defaults_are_safe)
{
  char *cm = slurp(FW_DIR "CMakeLists.txt");
  char *wc = slurp(FW_DIR "Core/Inc/wheel_config.h");
  CHECK(cm != NULL && wc != NULL);
  if (cm == NULL || wc == NULL) { free(cm); free(wc); return; }
  CHECK(strstr(cm, "option(WHEEL_FFB \"Build the force-feedback module (can command the motor)\" OFF)") != NULL);
  CHECK(strstr(cm, "set(WHEEL_FFB_ARM_ALLOWED \"0\" CACHE STRING") != NULL);
  CHECK(strstr(wc, "#define WHEEL_FFB                0\n") != NULL);
  CHECK(strstr(wc, "#define WHEEL_FFB_ARM_ALLOWED    0\n") != NULL);
  /* ffb sources: listed only in WHEEL_FFB_SOURCES, used only under if(WHEEL_FFB) */
  {
    const char *set = strstr(cm, "set(WHEEL_FFB_SOURCES");
    const char *set_end = set ? strstr(set, ")") : NULL;
    const char *use = strstr(cm, "${WHEEL_FFB_SOURCES}");
    const char *guard = strstr(cm, "if(WHEEL_FFB)");
    CHECK(set != NULL && set_end != NULL && use != NULL && guard != NULL);
    for (const char *p = strstr(cm, "Core/Src/ffb/"); p != NULL; p = strstr(p + 1, "Core/Src/ffb/"))
    {
      if (!(set && set_end && p > set && p < set_end))
      {
        printf("    Core/Src/ffb/ referenced outside set(WHEEL_FFB_SOURCES ...)\n");
        CHECK(0);
      }
    }
    if (use && guard)
    {
      const char *endif = strstr(guard, "endif()");
      CHECK(use > guard && endif != NULL && use < endif);
      CHECK(strstr(use + 1, "${WHEEL_FFB_SOURCES}") == NULL || strstr(use + 1, "${WHEEL_FFB_SOURCES}") < endif);
    }
  }
  free(cm);
  free(wc);
}

TEST(test_centering_law_not_in_firmware_build)
{
  char *cm = slurp(FW_DIR "CMakeLists.txt");
  CHECK(cm != NULL);
  if (cm == NULL) return;
  /* allowed only inside a comment line */
  for (char *p = strstr(cm, "centering_law"); p != NULL; p = strstr(p + 1, "centering_law"))
  {
    char *line = p;
    while (line > cm && line[-1] != '\n') line--;
    while (*line == ' ' || *line == '\t') line++;
    if (*line != '#')
    {
      printf("    centering_law referenced outside a comment in CMakeLists.txt\n");
      CHECK(0);
    }
  }
  free(cm);
}

void run_safety_tests(void)
{
  printf("safety (source tree)\n");
  RUN(test_no_odrive_write_tokens_outside_ffb);
  RUN(test_ffb_references_only_under_wheel_ffb);
  RUN(test_ffb_dir_write_path_is_confined);
  RUN(test_uart_ffb_entry_point_checks_allowlist);
  RUN(test_build_defaults_are_safe);
  RUN(test_centering_law_not_in_firmware_build);
}
