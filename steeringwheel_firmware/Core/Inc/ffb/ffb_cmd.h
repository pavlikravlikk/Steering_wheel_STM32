/**
 * @file    ffb_cmd.h
 * @brief   The ONLY ODrive write commands the firmware can produce (pure).
 *
 * Exactly three kinds, for the configured axis N:
 *   "w axisN.controller.input_torque <value>\n"   value: [-]d.dddd, |v| <= FFB_TORQUE_CMD_ABS_MAX
 *   "w axisN.requested_state 1\n" / "w axisN.requested_state 8\n"   (IDLE / CLOSED_LOOP)
 *   "u N\n"                                        (feed the watchdog)
 *
 * ffb_cmd_is_allowed() is called by odrive_uart_send_ffb() for every byte
 * string before it is queued; anything else is refused. Configuration,
 * startup flags, limits, control mode, watchdog settings and
 * save_configuration can never be written (hard rule 3, tests/test_ffb_cmd).
 */
#ifndef FFB_CMD_H
#define FFB_CMD_H

#include <stdbool.h>
#include <stddef.h>

#define FFB_CMD_MAX 48U

/** Format the torque command. Values are clamped to +-FFB_TORQUE_CMD_ABS_MAX,
 *  non-finite values become 0. Returns the length (0 on a too small buffer). */
size_t ffb_fmt_torque(char *buf, size_t size, int axis, float torque);

/** "w axisN.requested_state 1|8\n". Any other state -> 0 (nothing). */
size_t ffb_fmt_state(char *buf, size_t size, int axis, int state);

/** "u N\n". */
size_t ffb_fmt_feed(char *buf, size_t size, int axis);

/** true only for the commands above (exact syntax, configured axis). */
bool ffb_cmd_is_allowed(const char *cmd, int axis);

#endif /* FFB_CMD_H */
