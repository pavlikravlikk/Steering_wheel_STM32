/**
 * @file    ffb_cmd.c
 * @brief   ODrive write-command allowlist and formatters. Pure, no printf.
 */
#include "ffb_cmd.h"

#include <string.h>

#include "ffb_config.h"

#define TORQUE_PREFIX_TAIL ".controller.input_torque "
#define STATE_PREFIX_TAIL  ".requested_state "
#define STATE_IDLE         1
#define STATE_CLOSED_LOOP  8

static char axis_char(int axis)
{
  return (axis == 1) ? '1' : '0';
}

/* "w axisN" + tail */
static size_t put_prefix(char *buf, int axis, const char *tail)
{
  size_t n = 0U;
  memcpy(buf, "w axis", 6U);
  n = 6U;
  buf[n++] = axis_char(axis);
  memcpy(&buf[n], tail, strlen(tail));
  n += strlen(tail);
  return n;
}

size_t ffb_fmt_torque(char *buf, size_t size, int axis, float torque)
{
  char num[16];
  size_t k = 0U;
  size_t n;
  unsigned long scaled;
  bool neg;

  if (!((torque == torque) && (torque < 3.0e38f) && (torque > -3.0e38f)))
  {
    torque = 0.0f;
  }
  if (torque > FFB_TORQUE_CMD_ABS_MAX)  torque = FFB_TORQUE_CMD_ABS_MAX;
  if (torque < -FFB_TORQUE_CMD_ABS_MAX) torque = -FFB_TORQUE_CMD_ABS_MAX;
  neg = (torque < 0.0f);
  scaled = (unsigned long)(((neg ? -torque : torque) * 10000.0f) + 0.5f);   /* 4 decimals */
  if (scaled == 0UL)
  {
    neg = false;                                     /* never "-0.0000" */
  }
  if (neg)
  {
    num[k++] = '-';
  }
  {
    const unsigned long ip = scaled / 10000UL;
    unsigned long fp = scaled % 10000UL;
    if (ip >= 10UL)
    {
      num[k++] = (char)('0' + (ip / 10UL));
    }
    num[k++] = (char)('0' + (ip % 10UL));
    num[k++] = '.';
    num[k++] = (char)('0' + (fp / 1000UL)); fp %= 1000UL;
    num[k++] = (char)('0' + (fp / 100UL));  fp %= 100UL;
    num[k++] = (char)('0' + (fp / 10UL));
    num[k++] = (char)('0' + (fp % 10UL));
  }
  n = 6U + 1U + strlen(TORQUE_PREFIX_TAIL) + k + 1U;
  if (size < n + 1U)
  {
    return 0U;
  }
  n = put_prefix(buf, axis, TORQUE_PREFIX_TAIL);
  memcpy(&buf[n], num, k);
  n += k;
  buf[n++] = '\n';
  buf[n] = '\0';
  return n;
}

size_t ffb_fmt_state(char *buf, size_t size, int axis, int state)
{
  size_t n;

  if ((state != STATE_IDLE) && (state != STATE_CLOSED_LOOP))
  {
    return 0U;
  }
  if (size < 6U + 1U + strlen(STATE_PREFIX_TAIL) + 2U + 1U)
  {
    return 0U;
  }
  n = put_prefix(buf, axis, STATE_PREFIX_TAIL);
  buf[n++] = (char)('0' + state);
  buf[n++] = '\n';
  buf[n] = '\0';
  return n;
}

size_t ffb_fmt_feed(char *buf, size_t size, int axis)
{
  if (size < 5U)
  {
    return 0U;
  }
  buf[0] = 'u';
  buf[1] = ' ';
  buf[2] = axis_char(axis);
  buf[3] = '\n';
  buf[4] = '\0';
  return 4U;
}

static bool is_digit(char c)
{
  return (c >= '0') && (c <= '9');
}

/* "[-]d.dddd" or "[-]dd.dddd", value within the allowed magnitude. */
static bool torque_value_ok(const char *p, size_t n)
{
  size_t i = 0U;
  size_t int_digits = 0U;
  unsigned long scaled = 0UL;

  if ((n > 0U) && (p[0] == '-'))
  {
    i = 1U;
  }
  while ((i < n) && is_digit(p[i]))
  {
    scaled = (scaled * 10UL) + (unsigned long)(p[i] - '0');
    int_digits++;
    i++;
  }
  if ((int_digits < 1U) || (int_digits > 2U) || (i >= n) || (p[i] != '.'))
  {
    return false;
  }
  i++;
  if (n - i != 4U)
  {
    return false;
  }
  for (; i < n; i++)
  {
    if (!is_digit(p[i]))
    {
      return false;
    }
    scaled = (scaled * 10UL) + (unsigned long)(p[i] - '0');
  }
  return ((float)scaled / 10000.0f) <= (FFB_TORQUE_CMD_ABS_MAX + 0.00005f);
}

bool ffb_cmd_is_allowed(const char *cmd, int axis)
{
  char want[FFB_CMD_MAX];
  size_t n;
  size_t p;

  if (cmd == NULL)
  {
    return false;
  }
  n = strlen(cmd);
  if ((n < 4U) || (n >= FFB_CMD_MAX) || (cmd[n - 1U] != '\n'))
  {
    return false;
  }
  /* watchdog feed */
  (void)ffb_fmt_feed(want, sizeof(want), axis);
  if (strcmp(cmd, want) == 0)
  {
    return true;
  }
  /* state requests: IDLE or CLOSED_LOOP only */
  (void)ffb_fmt_state(want, sizeof(want), axis, STATE_IDLE);
  if (strcmp(cmd, want) == 0)
  {
    return true;
  }
  (void)ffb_fmt_state(want, sizeof(want), axis, STATE_CLOSED_LOOP);
  if (strcmp(cmd, want) == 0)
  {
    return true;
  }
  /* torque setpoint */
  p = put_prefix(want, axis, TORQUE_PREFIX_TAIL);
  if ((n > p + 1U) && (strncmp(cmd, want, p) == 0))
  {
    return torque_value_ok(&cmd[p], n - p - 1U);
  }
  return false;
}
