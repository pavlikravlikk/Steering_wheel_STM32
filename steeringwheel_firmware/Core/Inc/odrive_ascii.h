/**
 * @file    odrive_ascii.h
 * @brief   READ-ONLY ODrive ASCII protocol client logic (pure, host-tested).
 *
 * Request "f <axis>\n", the ODrive answers "<pos_turns> <vel_turns_s>\r\n".
 * Bytes are fed in as they arrive (any chunking); complete lines are parsed
 * strictly. Partial lines, garbage, echo of our own command, over-long lines
 * and timeouts are all tolerated and counted.
 *
 * SAFETY: this module only ever produces read requests. The transmit path
 * must pass every string through odrive_cmd_is_readonly(), which allows
 * nothing but "f <digit>\n" and "r <path>\n".
 */
#ifndef ODRIVE_ASCII_H
#define ODRIVE_ASCII_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ODR_LINE_MAX   48U   /* longest accepted reply line, without CR/LF */
#define ODR_CMD_MAX    48U   /* longest allowed request incl. '\n'         */

typedef enum
{
  ODR_LINE_OK = 0,     /* two valid numbers                                */
  ODR_LINE_ECHO,       /* our own command echoed back: ignore silently     */
  ODR_LINE_EMPTY,      /* blank line: ignore silently                      */
  ODR_LINE_ERROR       /* anything else (e.g. "invalid command format")    */
} odr_line_result_t;

typedef struct
{
  uint32_t requests;      /* requests sent                                 */
  uint32_t replies_ok;    /* valid position lines                          */
  uint32_t parse_errors;  /* non-numeric / malformed lines                 */
  uint32_t echo_lines;    /* echoed commands (ignored)                     */
  uint32_t overflows;     /* lines longer than ODR_LINE_MAX (discarded)    */
  uint32_t timeouts;      /* request without a reply in time               */
  uint32_t reads_ok;      /* one-shot "r <path>" reads answered            */
  uint32_t read_errors;   /* ...answered with something non-numeric        */
  uint32_t read_timeouts; /* ...not answered in time                       */
} odr_stats_t;

/* One-shot property read ("r <path>"), used only by the FFB precheck. */
typedef enum
{
  ODR_READ_IDLE = 0,   /* nothing queued                                   */
  ODR_READ_QUEUED,     /* next request will be the read                    */
  ODR_READ_PENDING,    /* sent, waiting for the single-value reply         */
  ODR_READ_DONE,       /* value available (odr_read_poll)                  */
  ODR_READ_FAILED      /* error reply or timeout (odr_read_poll)           */
} odr_read_state_t;

typedef struct
{
  /* configuration */
  char     cmd[ODR_CMD_MAX];     /* request incl. '\n', e.g. "f 0\n"      */
  uint32_t reply_timeout_ms;
  uint32_t stale_ms;

  /* line assembly */
  char     line[ODR_LINE_MAX + 1U];
  uint32_t len;
  bool     discarding;           /* overflow: skip until the next '\n'     */

  /* request bookkeeping */
  bool     outstanding;
  uint32_t sent_ms;

  /* latest data */
  bool     have_data;
  float    pos_turns;
  float    vel_turns_s;
  uint32_t last_ok_ms;

  /* one-shot read */
  char             read_cmd[ODR_CMD_MAX];   /* "r <path>\n"                */
  odr_read_state_t read_state;
  float            read_value;

  odr_stats_t stats;
} odr_t;

/** true only for "f <digit>\n" or "r <path>\n" (path = [a-z0-9_.], at
 *  least one char). Everything else, notably any write/state/torque
 *  command, is refused. */
bool odrive_cmd_is_readonly(const char *cmd);

/** Strict number parser: [+-]digits[.digits][e[+-]digits]. Rejects nan/inf,
 *  empty input and trailing junk. Leading/trailing blanks must be stripped
 *  by the caller; *end receives the first unparsed char. */
bool odr_parse_float(const char *s, const char **end, float *out);

/** Classify one complete line (without CR/LF). On ODR_LINE_OK fills pos and
 *  vel. @p echo_cmd is our request without '\n' (may be NULL). */
odr_line_result_t odr_parse_feedback_line(const char *line, const char *echo_cmd,
                                          float *pos, float *vel);

/** Initialise; @p axis 0 or 1. */
void odr_init(odr_t *r, int axis, uint32_t reply_timeout_ms, uint32_t stale_ms);

/** The request string to transmit ("f 0\n"). Always read-only. */
const char *odr_request(const odr_t *r);

/** true if a new request may go out now (none outstanding, or the
 *  outstanding one has timed out; the timeout is counted here). */
bool odr_may_send(odr_t *r, uint32_t now_ms);

/** Record that the request was handed to the UART. */
void odr_note_sent(odr_t *r, uint32_t now_ms);

/** Feed received bytes (any chunking). */
void odr_feed(odr_t *r, const uint8_t *data, size_t n, uint32_t now_ms);

/** true once at least one valid reply arrived and none for stale_ms. Also
 *  true before the first valid reply. */
bool odr_is_stale(const odr_t *r, uint32_t now_ms);

/* ---- one-shot reads ("r <path>"); still strictly read-only ------------- */

/** Parse a single-value reply: a number, or True/False (-> 1/0). */
bool odr_parse_single_line(const char *line, float *out);

/** Queue "r <path>\n" as the next request. Refused (false) while another
 *  read is queued/pending/unconsumed or if the result would not pass
 *  odrive_cmd_is_readonly(). */
bool odr_queue_read(odr_t *r, const char *path);

/** The next request to transmit: the queued read if any, else "f <axis>\n".
 *  Pair every transmission with odr_note_sent(). */
const char *odr_next_request(const odr_t *r);

/** Returns ODR_READ_DONE (value in *value) or ODR_READ_FAILED exactly once
 *  and then goes back to ODR_READ_IDLE; otherwise the current state. */
odr_read_state_t odr_read_poll(odr_t *r, float *value);

#endif /* ODRIVE_ASCII_H */
