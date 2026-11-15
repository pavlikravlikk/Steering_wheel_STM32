/* Safety regression checks on the firmware SOURCE TREE:
 *  - no firmware file mentions an ODrive write target or write command,
 *  - centering_law.c is not part of the firmware build.
 * If one of these fails, someone added a path that could command the motor:
 * that needs an explicit, documented decision by the owner first. */
#define _DEFAULT_SOURCE
#include "test_runner.h"

#include <dirent.h>
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

static int scan_dir(const char *dir)
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
    for (size_t i = 0; i < sizeof(k_forbidden) / sizeof(k_forbidden[0]); i++)
    {
      if (strstr(src, k_forbidden[i]) != NULL)
      {
        printf("    forbidden token '%s' in %s\n", k_forbidden[i], path);
        CHECK(0);
      }
    }
    free(src);
  }
  closedir(d);
  return files;
}

TEST(test_no_odrive_write_tokens_in_firmware)
{
  int n = scan_dir(FW_DIR "Core/Src") + scan_dir(FW_DIR "Core/Inc")
        + scan_dir(FW_DIR "USB_DEVICE/App");
  CHECK(n > 20);
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
  RUN(test_no_odrive_write_tokens_in_firmware);
  RUN(test_centering_law_not_in_firmware_build);
}
