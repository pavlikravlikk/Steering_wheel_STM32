/**
 * @file    ffb_precheck.c
 * @brief   Pre-arm / monitoring evaluation of ODrive values. Pure.
 */
#include "ffb_precheck.h"

#include <string.h>

#include "ffb_config.h"

/* ODrive 0.5.x property paths (READ ONLY). "axis0" is patched for axis 1. */
static const char *const k_paths[FFB_PC_COUNT] = {
  "axis0.config.enable_watchdog",
  "axis0.config.watchdog_timeout",
  "axis0.controller.config.control_mode",
  "axis0.motor.config.current_lim",
  "axis0.controller.config.vel_limit",
  "axis0.motor.config.torque_constant",
  "axis0.motor.is_calibrated",
  "axis0.encoder.is_ready",
  "axis0.error",
  "axis0.motor.error",
  "axis0.encoder.error",
  "vbus_voltage",
  "axis0.current_state",       /* present in docs/odrive_dump.txt (int32)   */
};
/* Every path above exists in docs/odrive_dump.txt (tests/test_ffb_dump.c). */

#define CONTROL_MODE_TORQUE   1.0f   /* ODrive ControlMode.TORQUE_CONTROL      */
#define AXIS_STATE_CLOSED_LOOP 8.0f  /* ODrive AxisState.CLOSED_LOOP_CONTROL   */
#define AXIS_STATE_IDLE        1.0f  /* ODrive AxisState.IDLE                  */

static const ffb_pc_item_t k_monitor[] = {
  FFB_PC_AXIS_ERROR, FFB_PC_MOTOR_ERROR, FFB_PC_ENCODER_ERROR, FFB_PC_VBUS, FFB_PC_CURRENT_STATE,
};
#define MONITOR_N (sizeof(k_monitor) / sizeof(k_monitor[0]))

void ffb_limits_default(ffb_limits_t *lim)
{
  lim->current_lim_max = FFB_PRECHECK_CURRENT_LIM_MAX;
  lim->vel_limit_max = FFB_PRECHECK_VEL_LIMIT_MAX;
  lim->vbus_min = FFB_VBUS_MIN_V;
  lim->vbus_max = FFB_VBUS_MAX_V;
  lim->wd_timeout_min = FFB_WATCHDOG_TIMEOUT_MIN_S;
  lim->wd_timeout_max = FFB_WATCHDOG_TIMEOUT_MAX_S;
  lim->kt_min = FFB_TORQUE_CONSTANT_MIN;
  lim->kt_max = FFB_TORQUE_CONSTANT_MAX;
}

void ffb_pc_init(ffb_pc_table_t *t, int axis, bool use_current_state)
{
  memset(t, 0, sizeof(*t));
  t->use_current_state = use_current_state;
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    strncpy(t->paths[i], k_paths[i], sizeof(t->paths[i]) - 1U);
    if ((axis == 1) && (strncmp(t->paths[i], "axis0", 5) == 0))
    {
      t->paths[i][4] = '1';
    }
  }
}

void ffb_pc_invalidate(ffb_pc_table_t *t)
{
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    t->v[i].valid = false;
    t->v[i].failed = false;
  }
  t->next = 0U;
}

const char *ffb_pc_path(const ffb_pc_table_t *t, ffb_pc_item_t item)
{
  return ((unsigned)item < (unsigned)FFB_PC_COUNT) ? t->paths[item] : "";
}

void ffb_pc_store(ffb_pc_table_t *t, ffb_pc_item_t item, float value, uint32_t now_ms)
{
  if ((unsigned)item < (unsigned)FFB_PC_COUNT)
  {
    t->v[item].value = value;
    t->v[item].read_ms = now_ms;
    t->v[item].valid = true;
    t->v[item].failed = false;
  }
}

void ffb_pc_store_failed(ffb_pc_table_t *t, ffb_pc_item_t item)
{
  if ((unsigned)item < (unsigned)FFB_PC_COUNT)
  {
    t->v[item].valid = false;
    t->v[item].failed = true;
  }
}

static bool in_use(const ffb_pc_table_t *t, unsigned i)
{
  return (i != (unsigned)FFB_PC_CURRENT_STATE) || t->use_current_state;
}

ffb_pc_item_t ffb_pc_next(ffb_pc_table_t *t)
{
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    if (in_use(t, i) && !t->v[i].valid && !t->v[i].failed)
    {
      return (ffb_pc_item_t)i;
    }
  }
  for (unsigned k = 0U; k < (unsigned)FFB_PC_COUNT; k++)
  {
    const unsigned i = (t->next + k) % (unsigned)FFB_PC_COUNT;
    if (in_use(t, i))
    {
      t->next = (i + 1U) % (unsigned)FFB_PC_COUNT;
      return (ffb_pc_item_t)i;
    }
  }
  return FFB_PC_AXIS_ERROR;
}

ffb_pc_item_t ffb_pc_next_monitor(ffb_pc_table_t *t)
{
  for (unsigned k = 0U; k < MONITOR_N; k++)
  {
    const ffb_pc_item_t item = k_monitor[(t->next + k) % MONITOR_N];
    if (in_use(t, (unsigned)item))
    {
      t->next = (unsigned)((t->next + k + 1U) % MONITOR_N);
      return item;
    }
  }
  return FFB_PC_AXIS_ERROR;
}

static bool fresh(const ffb_pc_value_t *v, uint32_t now_ms, uint32_t max_age_ms)
{
  return v->valid && ((now_ms - v->read_ms) <= max_age_ms);
}

bool ffb_pc_complete(const ffb_pc_table_t *t, uint32_t now_ms, uint32_t max_age_ms)
{
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    if (in_use(t, i) && !fresh(&t->v[i], now_ms, max_age_ms))
    {
      return false;
    }
  }
  return true;
}

static bool is_true(float v)
{
  return (v > 0.5f) && (v < 1.5f);      /* exactly "1" / "True" */
}

static bool is_zero(float v)
{
  return (v == 0.0f);
}

static bool within(float v, float lo, float hi)
{
  return (v == v) && (v >= lo) && (v <= hi);
}

ffb_reason_t ffb_precheck_evaluate(const ffb_pc_table_t *t, const ffb_limits_t *lim,
                                   const ffb_pc_context_t *ctx, uint32_t now_ms,
                                   uint32_t max_age_ms)
{
  if (!ctx->arm_allowed_build)
  {
    return FFB_REASON_ARM_NOT_ALLOWED;
  }
  if (ctx->simulation_build)
  {
    return FFB_REASON_SIMULATION_BUILD;
  }
  if (ctx->require_measured && (ctx->profile_safety_tbd > 0U))
  {
    return FFB_REASON_PROFILE_TBD;
  }
  if (!ctx->position_fresh)
  {
    return FFB_REASON_POSITION_STALE;
  }
  if (!ctx->wheel_in_range)
  {
    return FFB_REASON_WHEEL_OUT_OF_RANGE;
  }
  for (unsigned i = 0U; i < (unsigned)FFB_PC_COUNT; i++)
  {
    if (!in_use(t, i))
    {
      continue;
    }
    if (t->v[i].failed)
    {
      return FFB_REASON_READ_ERROR;
    }
    if (!fresh(&t->v[i], now_ms, max_age_ms))
    {
      return FFB_REASON_READ_TIMEOUT;
    }
  }
#define V(item) (t->v[(item)].value)
  if (!is_true(V(FFB_PC_ENABLE_WATCHDOG)))                         return FFB_REASON_WATCHDOG_DISABLED;
  if (!within(V(FFB_PC_WATCHDOG_TIMEOUT), lim->wd_timeout_min, lim->wd_timeout_max))
                                                                   return FFB_REASON_WATCHDOG_TIMEOUT_RANGE;
  if (V(FFB_PC_CONTROL_MODE) != CONTROL_MODE_TORQUE)               return FFB_REASON_CONTROL_MODE;
  if (!within(V(FFB_PC_CURRENT_LIM), 0.0f, lim->current_lim_max))  return FFB_REASON_CURRENT_LIM_HIGH;
  if (!within(V(FFB_PC_VEL_LIMIT), 0.0f, lim->vel_limit_max))      return FFB_REASON_VEL_LIMIT_HIGH;
  if (!within(V(FFB_PC_TORQUE_CONSTANT), lim->kt_min, lim->kt_max)) return FFB_REASON_TORQUE_CONSTANT_RANGE;
  if (!is_true(V(FFB_PC_MOTOR_CALIBRATED)))                        return FFB_REASON_MOTOR_NOT_CALIBRATED;
  if (!is_true(V(FFB_PC_ENCODER_READY)))                           return FFB_REASON_ENCODER_NOT_READY;
  if (!is_zero(V(FFB_PC_AXIS_ERROR)))                              return FFB_REASON_AXIS_ERROR;
  if (!is_zero(V(FFB_PC_MOTOR_ERROR)))                             return FFB_REASON_MOTOR_ERROR;
  if (!is_zero(V(FFB_PC_ENCODER_ERROR)))                           return FFB_REASON_ENCODER_ERROR;
  if (t->use_current_state && (V(FFB_PC_CURRENT_STATE) != AXIS_STATE_IDLE))
                                                                   return FFB_REASON_NOT_IDLE;
  if (!within(V(FFB_PC_VBUS), lim->vbus_min, lim->vbus_max))       return FFB_REASON_VBUS_RANGE;
#undef V
  return FFB_REASON_OK;
}

ffb_fault_t ffb_monitor_evaluate(const ffb_pc_table_t *t, const ffb_limits_t *lim,
                                 uint32_t now_ms, uint32_t max_age_ms, bool check_state)
{
  for (unsigned k = 0U; k < MONITOR_N; k++)
  {
    const ffb_pc_item_t i = k_monitor[k];
    const ffb_pc_value_t *v = &t->v[i];
    if (!in_use(t, (unsigned)i) || ((i == FFB_PC_CURRENT_STATE) && !check_state))
    {
      continue;
    }
    if (v->failed || !fresh(v, now_ms, max_age_ms))
    {
      return FFB_FAULT_READ_FAILED;
    }
    switch (i)
    {
      case FFB_PC_AXIS_ERROR:
      case FFB_PC_MOTOR_ERROR:
      case FFB_PC_ENCODER_ERROR:
        if (!is_zero(v->value))
        {
          return FFB_FAULT_ODRIVE_ERROR;
        }
        break;
      case FFB_PC_VBUS:
        if (!within(v->value, lim->vbus_min, lim->vbus_max))
        {
          return FFB_FAULT_VBUS_RANGE;
        }
        break;
      case FFB_PC_CURRENT_STATE:
        if (v->value != AXIS_STATE_CLOSED_LOOP)
        {
          return FFB_FAULT_ODRIVE_ERROR;   /* the ODrive left closed loop */
        }
        break;
      default:
        break;
    }
  }
  return FFB_FAULT_NONE;
}
