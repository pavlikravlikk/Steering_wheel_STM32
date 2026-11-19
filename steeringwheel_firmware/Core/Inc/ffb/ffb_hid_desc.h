/**
 * @file    ffb_hid_desc.h
 * @brief   HID report descriptor of the FFB build: joystick + USB HID PID
 *          (Physical Interface Device) force feedback + vendor status.
 *
 * Written from the USB "Device Class Definition for PID 1.0" usage tables and
 * the requirements of the Linux hid-pidff driver (which reports are required,
 * how arrays and nested logical collections are looked up). It is NOT used by
 * the default build, which keeps its 42-byte descriptor untouched.
 *
 * All multi-byte fields are little endian. Sizes include the Report ID byte.
 */
#ifndef FFB_HID_DESC_H
#define FFB_HID_DESC_H

#include <stddef.h>
#include <stdint.h>

/* Report IDs */
#define FFB_RID_JOYSTICK        0x01U  /* input   X,Y,Z,16 buttons           */
#define FFB_RID_PID_STATE       0x02U  /* input   (GET_REPORT only)          */
#define FFB_RID_SET_EFFECT      0x11U  /* output                             */
#define FFB_RID_SET_ENVELOPE    0x12U  /* output                             */
#define FFB_RID_SET_CONDITION   0x13U  /* output                             */
#define FFB_RID_SET_PERIODIC    0x14U  /* output                             */
#define FFB_RID_SET_CONSTANT    0x15U  /* output                             */
#define FFB_RID_SET_RAMP        0x16U  /* output                             */
#define FFB_RID_EFFECT_OP       0x17U  /* output                             */
#define FFB_RID_BLOCK_FREE      0x18U  /* output                             */
#define FFB_RID_DEVICE_CONTROL  0x19U  /* output                             */
#define FFB_RID_DEVICE_GAIN     0x1AU  /* output                             */
#define FFB_RID_CREATE_EFFECT   0x21U  /* feature (SET_REPORT)               */
#define FFB_RID_BLOCK_LOAD      0x22U  /* feature (GET_REPORT)               */
#define FFB_RID_POOL            0x23U  /* feature (GET_REPORT)               */
#define FFB_RID_STATUS          0x40U  /* feature (GET_REPORT), vendor page  */

/* Report lengths in bytes, including the Report ID */
#define FFB_LEN_JOYSTICK        9U
#define FFB_LEN_PID_STATE       3U
#define FFB_LEN_SET_EFFECT      16U
#define FFB_LEN_SET_ENVELOPE    10U
#define FFB_LEN_SET_CONDITION   15U
#define FFB_LEN_SET_PERIODIC    10U
#define FFB_LEN_SET_CONSTANT    4U
#define FFB_LEN_SET_RAMP        6U
#define FFB_LEN_EFFECT_OP       4U
#define FFB_LEN_BLOCK_FREE      2U
#define FFB_LEN_DEVICE_CONTROL  2U
#define FFB_LEN_DEVICE_GAIN     2U
#define FFB_LEN_CREATE_EFFECT   2U
#define FFB_LEN_BLOCK_LOAD      5U
#define FFB_LEN_POOL            5U
#define FFB_LEN_STATUS          16U
#define FFB_LEN_MAX             16U

/* Effect block indexes are 1..FFB_MAX_EFFECTS (logical range of 0x22). */
#define FFB_MAX_EFFECTS         16U

/* Value of the effect-type array = 1-based position in the descriptor list.
 * The SAME order is used in Set Effect and Create New Effect. */
#define FFB_ET_CONSTANT   1U
#define FFB_ET_RAMP       2U
#define FFB_ET_SQUARE     3U
#define FFB_ET_SINE       4U
#define FFB_ET_TRIANGLE   5U
#define FFB_ET_SAW_UP     6U
#define FFB_ET_SAW_DOWN   7U
#define FFB_ET_SPRING     8U
#define FFB_ET_DAMPER     9U
#define FFB_ET_INERTIA    10U
#define FFB_ET_FRICTION   11U
#define FFB_ET_COUNT      11U

/* Effect operation (array 0x78) */
#define FFB_OP_START      1U
#define FFB_OP_START_SOLO 2U
#define FFB_OP_STOP       3U

/* Device control (array 0x96) */
#define FFB_DC_ENABLE_ACTUATORS  1U
#define FFB_DC_DISABLE_ACTUATORS 2U
#define FFB_DC_STOP_ALL          3U
#define FFB_DC_RESET             4U
#define FFB_DC_PAUSE             5U
#define FFB_DC_CONTINUE          6U

/* Block load status (array 0x8B) */
#define FFB_BL_PENDING    0U   /* not a valid array value: "ask again"       */
#define FFB_BL_SUCCESS    1U
#define FFB_BL_FULL       2U
#define FFB_BL_ERROR      3U

/* Scales used in the descriptor */
#define FFB_SCALE_MAG     10000   /* magnitudes, coefficients: -10000..10000  */
#define FFB_DIR_MAX       35999U  /* direction / phase: 0.01 degree units      */
#define FFB_TIME_INFINITE 0xFFFFU /* duration: Linux sends all ones = forever  */

extern const uint8_t ffb_hid_report_desc[];
extern const uint16_t ffb_hid_report_desc_size;

#endif /* FFB_HID_DESC_H */
