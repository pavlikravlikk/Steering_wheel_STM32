/**
 * @file    odrive_ascii.c
 * @brief   READ-ONLY ODrive ASCII client logic. Must not include any HAL header.
 */
#include "odrive_ascii.h"

#include <string.h>

/* ---------------------------------------------------------------------- */
/* Read-only command allowlist                                            */
/* ---------------------------------------------------------------------- */
static bool is_path_char(char c)
{
  return ((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9'))
      || (c == '_') || (c == '.');
}

bool odrive_cmd_is_readonly(const char *cmd)
{
  size_t n;

  if (cmd == NULL)
  {
    return false;
  }
  n = strlen(cmd);
  if ((n < 4U) || (n > ODR_CMD_MAX - 1U) || (cmd[n - 1U] != '\n') || (cmd[1] != ' '))
  {
    return false;
  }

  if (cmd[0] == 'f')
  {
    /* exactly "f <digit>\n" */
    return (n == 4U) && (cmd[2] >= '0') && (cmd[2] <= '9');
  }

  if (cmd[0] == 'r')
  {
    /* "r <path>\n", path made only of [a-z0-9_.] (no spaces, so no value
     * can be appended to turn it into anything else). */
    for (size_t i = 2U; i < (n - 1U); i++)
    {
      if (!is_path_char(cmd[i]))
      {
        return false;
      }
    }
    return true;
  }

  return false;
}

/* ---------------------------------------------------------------------- */
/* Parsing                                                                */
/* ---------------------------------------------------------------------- */
static bool is_digit(char c)
{
  return (c >= '0') && (c <= '9');
}

bool odr_parse_float(const char *s, const char **end, float *out)
{
  const char *p = s;
  double mant = 0.0;
  double scale = 1.0;
  bool neg = false;
  bool any_digit = false;
  int exp10 = 0;

  if (*p == '+' || *p == '-')
  {
    neg = (*p == '-');
    p++;
  }
  while (is_digit(*p))
  {
    mant = (mant * 10.0) + (double)(*p - '0');
    any_digit = true;
    p++;
  }
  if (*p == '.')
  {
    p++;
    while (is_digit(*p))
    {
      scale *= 0.1;
      mant += (double)(*p - '0') * scale;
      any_digit = true;
      p++;
    }
  }
  if (!any_digit)
  {
    return false;
  }
  if (*p == 'e' || *p == 'E')
  {
    bool eneg = false;
    int e = 0;
    p++;
    if (*p == '+' || *p == '-')
    {
      eneg = (*p == '-');
      p++;
    }
    if (!is_digit(*p))
    {
      return false;
    }
    while (is_digit(*p))
    {
      if (e < 1000)
      {
        e = (e * 10) + (*p - '0');
      }
      p++;
    }
    exp10 = eneg ? -e : e;
  }
  if (exp10 > 30 || exp10 < -30)
  {
    return false;          /* outside any plausible ODrive value */
  }
  while (exp10 > 0)
  {
    mant *= 10.0;
    exp10--;
  }
  while (exp10 < 0)
  {
    mant *= 0.1;
    exp10++;
  }
  if (mant > 1.0e30)
  {
    return false;
  }
  *out = (float)(neg ? -mant : mant);
  if (end != NULL)
  {
    *end = p;
  }
  return true;
}

static const char *skip_blanks(const char *p)
{
  while (*p == ' ' || *p == '\t')
  {
    p++;
  }
  return p;
}

odr_line_result_t odr_parse_feedback_line(const char *line, const char *echo_cmd,
                                          float *pos, float *vel)
{
  const char *p = skip_blanks(line);
  float a;
  float b;

  if (*p == '\0')
  {
    return ODR_LINE_EMPTY;
  }
  if ((echo_cmd != NULL) && (echo_cmd[0] != '\0'))
  {
    const size_t n = strlen(echo_cmd);
    if ((strncmp(p, echo_cmd, n) == 0) && (*skip_blanks(p + n) == '\0'))
    {
      return ODR_LINE_ECHO;
    }
  }
  if (!odr_parse_float(p, &p, &a))
  {
    return ODR_LINE_ERROR;
  }
  if (*p != ' ' && *p != '\t')
  {
    return ODR_LINE_ERROR;   /* "1.0x 2" or a single number */
  }
  p = skip_blanks(p);
  if (!odr_parse_float(p, &p, &b))
  {
    return ODR_LINE_ERROR;
  }
  if (*skip_blanks(p) != '\0')
  {
    return ODR_LINE_ERROR;   /* trailing junk / a third field */
  }
  *pos = a;
  *vel = b;
  return ODR_LINE_OK;
}

/* ---------------------------------------------------------------------- */
/* Client state machine                                                   */
/* ---------------------------------------------------------------------- */
void odr_init(odr_t *r, int axis, uint32_t reply_timeout_ms, uint32_t stale_ms)
{
  memset(r, 0, sizeof(*r));
  r->cmd[0] = 'f';
  r->cmd[1] = ' ';
  r->cmd[2] = (char)('0' + ((axis == 1) ? 1 : 0));
  r->cmd[3] = '\n';
  r->cmd[4] = '\0';
  r->reply_timeout_ms = reply_timeout_ms;
  r->stale_ms = stale_ms;
}

const char *odr_request(const odr_t *r)
{
  return r->cmd;
}

bool odr_may_send(odr_t *r, uint32_t now_ms)
{
  if (!r->outstanding)
  {
    return true;
  }
  if ((now_ms - r->sent_ms) >= r->reply_timeout_ms)
  {
    r->outstanding = false;
    r->stats.timeouts++;
    return true;
  }
  return false;
}

void odr_note_sent(odr_t *r, uint32_t now_ms)
{
  r->outstanding = true;
  r->sent_ms = now_ms;
  r->stats.requests++;
}

static void handle_line(odr_t *r, uint32_t now_ms)
{
  char echo[ODR_CMD_MAX];
  float pos = 0.0f;
  float vel = 0.0f;
  size_t n = strlen(r->cmd);

  memcpy(echo, r->cmd, n);
  if (n > 0U && echo[n - 1U] == '\n')
  {
    n--;
  }
  echo[n] = '\0';

  r->line[r->len] = '\0';
  switch (odr_parse_feedback_line(r->line, echo, &pos, &vel))
  {
    case ODR_LINE_OK:
      r->pos_turns = pos;
      r->vel_turns_s = vel;
      r->have_data = true;
      r->last_ok_ms = now_ms;
      r->outstanding = false;
      r->stats.replies_ok++;
      break;
    case ODR_LINE_ECHO:
      r->stats.echo_lines++;
      break;
    case ODR_LINE_EMPTY:
      break;
    case ODR_LINE_ERROR:
    default:
      r->stats.parse_errors++;
      r->outstanding = false;   /* the ODrive did answer, just not usefully */
      break;
  }
}

void odr_feed(odr_t *r, const uint8_t *data, size_t n, uint32_t now_ms)
{
  for (size_t i = 0U; i < n; i++)
  {
    const char c = (char)data[i];

    if (c == '\n')
    {
      if (r->discarding)
      {
        r->discarding = false;
      }
      else
      {
        handle_line(r, now_ms);
      }
      r->len = 0U;
    }
    else if (c == '\r')
    {
      /* ignore; '\n' terminates */
    }
    else if (r->discarding)
    {
      /* skip the rest of an over-long line */
    }
    else if ((c == '\0') || ((uint8_t)c >= 0x80U))
    {
      /* line noise (e.g. a framing glitch): poison the current line */
      r->line[0] = '?';
      r->len = (r->len == 0U) ? 1U : r->len;
    }
    else if (r->len >= ODR_LINE_MAX)
    {
      r->discarding = true;
      r->len = 0U;
      r->stats.overflows++;
    }
    else
    {
      r->line[r->len++] = c;
    }
  }
}

bool odr_is_stale(const odr_t *r, uint32_t now_ms)
{
  if (!r->have_data)
  {
    return true;
  }
  return (now_ms - r->last_ok_ms) >= r->stale_ms;
}
