/**
 * @file    usbd_ffb_hid.h
 * @brief   USB HID class driver for the FFB build (firmware only).
 *
 * Replaces the stock CustomHID class ONLY in -DWHEEL_FFB=ON builds. Why a
 * separate class instead of patching the middleware: the stock class
 *  - forwards only 2 bytes of every OUT report (USBD_CUSTOMHID_OUTREPORT_BUF_SIZE),
 *  - does not answer GET_REPORT (Linux reads PID Block Load / PID Pool with it),
 *  - stores the report-descriptor length in ONE byte (ours is longer than 255).
 * This driver uses the same endpoints (0x81 IN, 0x01 OUT, 64 bytes) and the
 * PMA layout already set up in usbd_conf.c, so no generated file changes.
 * The default build does not compile this file.
 */
#ifndef USBD_FFB_HID_H
#define USBD_FFB_HID_H

#include <stdbool.h>
#include <stdint.h>

#include "usbd_def.h"

extern USBD_ClassTypeDef USBD_FFB_HID;

/** Swap the registered class to USBD_FFB_HID. Call right after
 *  MX_USB_DEVICE_Init(), before the host can enumerate. */
void usbd_ffb_install(USBD_HandleTypeDef *pdev);

/** true when configured and the IN endpoint is free. */
bool usbd_ffb_in_ready(USBD_HandleTypeDef *pdev);

/** Send an input report (with Report ID). The buffer must stay valid until
 *  the transfer completes (use a static buffer). Also kept for GET_REPORT. */
bool usbd_ffb_send(USBD_HandleTypeDef *pdev, uint8_t *report, uint16_t len);

#endif /* USBD_FFB_HID_H */
