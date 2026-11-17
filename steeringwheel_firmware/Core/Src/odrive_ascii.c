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
    if (r->read_state == ODR_READ_PENDING)
    {
      r->read_state = ODR_READ_FAILED;
      r->stats.read_timeouts++;
    }
    return true;
  }
  return false;
}

void odr_note_sent(odr_t *r, uint32_t now_ms)
{
  r->outstanding = true;
  r->sent_ms = now_ms;
  r->stats.requests++;
  if (r->read_state == ODR_READ_QUEUED)
  {
    r->read_state = ODR_READ_PENDING;   /* what was sent was the read */
  }
}

/* ---------------------------------------------------------------------- */
/* One-shot reads                                                         */
/* ---------------------------------------------------------------------- */
static bool word_is(const char *p, const char *word)
{
  const size_t n = strlen(word);
  return (strncmp(p, word, n) == 0) && (*skip_blanks(p + n) == '\0');
}

bool odr_parse_single_line(const char *line, float *out)
{
  const char *p = skip_blanks(line);
  float v;

  if (word_is(p, "True") || word_is(p, "true"))
  {
    *out = 1.0f;
    return true;
  }
  if (word_is(p, "False") || word_is(p, "false"))
  {
    *out = 0.0f;
    return true;
  }
  {
    const char *start = p;
    if (!odr_parse_float(p, &p, &v))
    {
      return false;
    }
    /* Bench 2026-11-17 (ODrive 0.5.x dev build, "r axis0.error"): this build prints the axis error as a decimal
     * integer followed by ONE letter 'd' ("0d", "2048d" = 0x800). Accepted only for a plain unsigned integer
     * (digits only) with the 'd' directly after it; every other trailing character is still rejected. */
    if (*p == 'd')
    {
      for (const char *q = start; q < p; q++)
      {
        if ((*q < '0') || (*q > '9'))
        {
          return false;
        }
      }
      p++;
    }
    if (*skip_blanks(p) != '\0')
    {
      return false;
    }
  }
  *out = v;
  return true;
}

bool odr_queue_read(odr_t *r, const char *path)
{
  size_t n;

  if ((path == NULL) || (r->read_state != ODR_READ_IDLE))
  {
    return false;
  }
  n = strlen(path);
  if ((n == 0U) || (n + 4U > ODR_CMD_MAX))
  {
    return false;
  }
  r->read_cmd[0] = 'r';
  r->read_cmd[1] = ' ';
  memcpy(&r->read_cmd[2], path, n);
  r->read_cmd[2U + n] = '\n';
  r->read_cmd[3U + n] = '\0';
  if (!odrive_cmd_is_readonly(r->read_cmd))
  {
    r->read_cmd[0] = '\0';
    return false;
  }
  r->read_state = ODR_READ_QUEUED;
  return true;
}

const char *odr_next_request(const odr_t *r)
{
  return (r->read_state == ODR_READ_QUEUED) ? r->read_cmd : r->cmd;
}

odr_read_state_t odr_read_poll(odr_t *r, float *value)
{
  const odr_read_state_t s = r->read_state;

  if (s == ODR_READ_DONE)
  {
    *value = r->read_value;
    r->read_state = ODR_READ_IDLE;
  }
  else if (s == ODR_READ_FAILED)
  {
    r->read_state = ODR_READ_IDLE;
  }
  return s;
}

static void strip_newline(char *dst, const char *src)
{
  size_t n = strlen(src);

  memcpy(dst, src, n);
  if (n > 0U && dst[n - 1U] == '\n')
  {
    n--;
  }
  dst[n] = '\0';
}

/** A line while a read is pending: single value -> DONE, feedback line ->
 *  late position reply (used, read stays pending), echo/blank -> ignored,
 *  anything else -> the read FAILED. Returns false if not consumed. */
static bool handle_read_line(odr_t *r, const char *echo_f, uint32_t now_ms)
{
  char echo_r[ODR_CMD_MAX];
  float a = 0.0f;
  float b = 0.0f;

  strip_newline(echo_r, r->read_cmd);
  switch (odr_parse_feedback_line(r->line, echo_r, &a, &b))
  {
    case ODR_LINE_OK:
      r->pos_turns = a;          /* late reply to an earlier "f" request */
      r->vel_turns_s = b;
      r->have_data = true;
      r->last_ok_ms = now_ms;
      r->stats.replies_ok++;
      return true;
    case ODR_LINE_ECHO:
      r->stats.echo_lines++;
      return true;
    case ODR_LINE_EMPTY:
      return true;
    default:
      break;
  }
  if ((echo_f != NULL) && word_is(skip_blanks(r->line), echo_f))
  {
    r->stats.echo_lines++;
    return true;
  }
  if (odr_parse_single_line(r->line, &a))
  {
    r->read_value = a;
    r->read_state = ODR_READ_DONE;
    r->stats.reads_ok++;
  }
  else
  {
    r->read_state = ODR_READ_FAILED;
    r->stats.read_errors++;
  }
  r->outstanding = false;
  return true;
}

static void handle_line(odr_t *r, uint32_t now_ms)
{
  char echo[ODR_CMD_MAX];
  float pos = 0.0f;
  float vel = 0.0f;

  strip_newline(echo, r->cmd);

  r->line[r->len] = '\0';
  if ((r->read_state == ODR_READ_PENDING) && handle_read_line(r, echo, now_ms))
  {
    return;
  }
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
