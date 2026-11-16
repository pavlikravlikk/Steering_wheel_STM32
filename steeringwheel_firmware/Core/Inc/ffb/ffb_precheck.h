/**
 * @file    ffb_precheck.h
 * @brief   Pre-arm and run-time monitoring of ODrive values (pure, host-tested).
 *
 * A table of READ-ONLY ODrive properties, each with its last value and the
 * time it was read. ffb_service.c reads them one at a time ("r <path>"),
 * ffb_precheck_evaluate() turns the table into a reason code, and
 * ffb_monitor_evaluate() into a run-time fault.
 *
 * The firmware NEVER writes any of these properties; it only compares them
 * with the limits from config/hardware_profile.toml.
 */
#ifndef FFB_PRECHECK_H
#define FFB_PRECHECK_H

#include <stdbool.h>
#include <stdint.h>

#include "ffb_safety.h"

typedef enum
{
  FFB_PC_ENABLE_WATCHDOG = 0,
  FFB_PC_WATCHDOG_TIMEOUT,
  FFB_PC_CONTROL_MODE,
  FFB_PC_CURRENT_LIM,
  FFB_PC_VEL_LIMIT,
  FFB_PC_TORQUE_CONSTANT,
  FFB_PC_MOTOR_CALIBRATED,
  FFB_PC_ENCODER_READY,
  FFB_PC_AXIS_ERROR,
  FFB_PC_MOTOR_ERROR,
  FFB_PC_ENCODER_ERROR,
  FFB_PC_VBUS,
  FFB_PC_CURRENT_STATE,      /* only with FFB_VERIFY_CLOSED_LOOP            */
  FFB_PC_COUNT
} ffb_pc_item_t;

typedef struct
{
  float    value;
  uint32_t read_ms;
  bool     valid;            /* read successfully since the last invalidate */
  bool     failed;           /* the last read of this item failed           */
} ffb_pc_value_t;

typedef struct
{
  ffb_pc_value_t v[FFB_PC_COUNT];
  char           paths[FFB_PC_COUNT][40];
  bool           use_current_state;
  uint32_t       next;       /* round-robin cursor                          */
} ffb_pc_table_t;

typedef struct
{
  float current_lim_max;     /* A                                           */
  float vel_limit_max;       /* turns/s                                     */
  float vbus_min, vbus_max;  /* V                                           */
  float wd_timeout_min, wd_timeout_max; /* s                                */
  float kt_min, kt_max;      /* Nm/A                                        */
} ffb_limits_t;

/** Context that is not an ODrive value. */
typedef struct
{
  bool     arm_allowed_build;
  bool     simulation_build;
  uint32_t profile_safety_tbd;   /* PROFILE_SAFETY_TBD_COUNT               */
  bool     require_measured;     /* FFB_REQUIRE_MEASURED_PROFILE           */
  bool     position_fresh;
  bool     wheel_in_range;       /* |angle| <= lock/2 from the centre (arming outside the range
                                    would start inside the end-stop wall)                   */
} ffb_pc_context_t;

void ffb_limits_default(ffb_limits_t *lim);

/** @p axis 0/1. @p use_current_state = FFB_VERIFY_CLOSED_LOOP. */
void ffb_pc_init(ffb_pc_table_t *t, int axis, bool use_current_state);

/** Forget all values (start of a precheck). */
void ffb_pc_invalidate(ffb_pc_table_t *t);

const char *ffb_pc_path(const ffb_pc_table_t *t, ffb_pc_item_t item);

void ffb_pc_store(ffb_pc_table_t *t, ffb_pc_item_t item, float value, uint32_t now_ms);
void ffb_pc_store_failed(ffb_pc_table_t *t, ffb_pc_item_t item);

/** Next item to read: the first one not yet valid, else round-robin. */
ffb_pc_item_t ffb_pc_next(ffb_pc_table_t *t);

/** Next item of the run-time monitoring set (errors, vbus, current_state). */
ffb_pc_item_t ffb_pc_next_monitor(ffb_pc_table_t *t);

/** true when every item in use is valid and younger than max_age_ms. */
bool ffb_pc_complete(const ffb_pc_table_t *t, uint32_t now_ms, uint32_t max_age_ms);

/** Context checks first (build flags, profile, position), then every value
 *  in a fixed order. Returns the FIRST failing reason, or FFB_REASON_OK.
 *  Missing/old values give READ_TIMEOUT (or READ_ERROR if a read failed). */
ffb_reason_t ffb_precheck_evaluate(const ffb_pc_table_t *t, const ffb_limits_t *lim,
                                   const ffb_pc_context_t *ctx, uint32_t now_ms,
                                   uint32_t max_age_ms);

/** Run-time check of the monitored values (errors, vbus and, if
 *  @p check_state, current_state == CLOSED_LOOP). Returns FFB_FAULT_NONE or
 *  the fault. Values older than max_age_ms or a failed read ->
 *  FFB_FAULT_READ_FAILED. */
ffb_fault_t ffb_monitor_evaluate(const ffb_pc_table_t *t, const ffb_limits_t *lim,
                                 uint32_t now_ms, uint32_t max_age_ms, bool check_state);

#endif /* FFB_PRECHECK_H */
