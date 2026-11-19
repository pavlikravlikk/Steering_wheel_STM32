/**
 * @file    usbd_ffb_hid.c
 * @brief   HID class with full-length OUT reports, SET_REPORT/GET_REPORT for
 *          feature and input reports and a 16-bit report-descriptor length.
 *          Written for the ST USB device library in Middlewares/ (same
 *          callback table as the CustomHID class). FFB builds only.
 */
#include "usbd_ffb_hid.h"

#include <string.h>

#include "ffb_hid_desc.h"
#include "ffb_pid.h"
#include "main.h"
#include "usbd_ctlreq.h"
#include "usbd_customhid.h"      /* endpoint addresses/sizes shared with usbd_conf.c */
#include "usbd_ioreq.h"

#define HID_DESC_TYPE          0x21U
#define HID_REPORT_DESC_TYPE   0x22U
#define HID_REQ_GET_REPORT     0x01U
#define HID_REQ_GET_IDLE       0x02U
#define HID_REQ_GET_PROTOCOL   0x03U
#define HID_REQ_SET_REPORT     0x09U
#define HID_REQ_SET_IDLE       0x0AU
#define HID_REQ_SET_PROTOCOL   0x0BU
#define FFB_EP_SIZE            64U
#define CFG_DESC_SIZE          41U

_Static_assert(CUSTOM_HID_EPIN_SIZE >= FFB_LEN_MAX, "IN endpoint too small");
_Static_assert(CUSTOM_HID_EPOUT_SIZE >= FFB_LEN_MAX, "OUT endpoint too small");

typedef struct
{
  uint8_t          out_buf[FFB_EP_SIZE];   /* interrupt OUT                    */
  uint8_t          ep0_buf[FFB_EP_SIZE];   /* SET_REPORT data stage            */
  uint8_t          reply[FFB_EP_SIZE];     /* GET_REPORT answer                */
  uint8_t          last_joystick[FFB_LEN_JOYSTICK];
  uint32_t         protocol;
  uint32_t         idle;
  uint32_t         alt;
  uint8_t          set_type;
  uint16_t         set_len;
  bool             set_pending;
  volatile bool    in_busy;
} ffb_usb_t;

static ffb_usb_t s_usb;

/* Configuration descriptor; the report-descriptor length (2 bytes) is
 * patched in usbd_ffb_install(). */
static uint8_t s_cfg[CFG_DESC_SIZE] = {
  0x09, USB_DESC_TYPE_CONFIGURATION, CFG_DESC_SIZE, 0x00,
  0x01,                    /* 1 interface                                     */
  0x01,                    /* configuration value                             */
  0x00,                    /* no string                                       */
  0xC0,                    /* self powered (same as the stock class)          */
  0x32,                    /* 100 mA                                          */
  /* interface */
  0x09, USB_DESC_TYPE_INTERFACE, 0x00, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
  /* HID */
  0x09, HID_DESC_TYPE, 0x11, 0x01, 0x00, 0x01, HID_REPORT_DESC_TYPE, 0x00, 0x00,
  /* endpoint IN */
  0x07, USB_DESC_TYPE_ENDPOINT, CUSTOM_HID_EPIN_ADDR, 0x03, CUSTOM_HID_EPIN_SIZE, 0x00,
  CUSTOM_HID_FS_BINTERVAL,
  /* endpoint OUT */
  0x07, USB_DESC_TYPE_ENDPOINT, CUSTOM_HID_EPOUT_ADDR, 0x03, CUSTOM_HID_EPOUT_SIZE, 0x00,
  CUSTOM_HID_FS_BINTERVAL,
};
#define CFG_HID_OFFSET  18U

static uint8_t s_qualifier[USB_LEN_DEV_QUALIFIER_DESC] = {
  USB_LEN_DEV_QUALIFIER_DESC, USB_DESC_TYPE_DEVICE_QUALIFIER, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40,
  0x01, 0x00,
};

static uint8_t ffb_init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  USBD_LL_OpenEP(pdev, CUSTOM_HID_EPIN_ADDR, USBD_EP_TYPE_INTR, CUSTOM_HID_EPIN_SIZE);
  pdev->ep_in[CUSTOM_HID_EPIN_ADDR & 0xFU].is_used = 1U;
  USBD_LL_OpenEP(pdev, CUSTOM_HID_EPOUT_ADDR, USBD_EP_TYPE_INTR, CUSTOM_HID_EPOUT_SIZE);
  pdev->ep_out[CUSTOM_HID_EPOUT_ADDR & 0xFU].is_used = 1U;
  pdev->pClassData = &s_usb;
  s_usb.in_busy = false;
  s_usb.set_pending = false;
  USBD_LL_PrepareReceive(pdev, CUSTOM_HID_EPOUT_ADDR, s_usb.out_buf, sizeof(s_usb.out_buf));
  return USBD_OK;
}

static uint8_t ffb_deinit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
  (void)cfgidx;
  USBD_LL_CloseEP(pdev, CUSTOM_HID_EPIN_ADDR);
  pdev->ep_in[CUSTOM_HID_EPIN_ADDR & 0xFU].is_used = 0U;
  USBD_LL_CloseEP(pdev, CUSTOM_HID_EPOUT_ADDR);
  pdev->ep_out[CUSTOM_HID_EPOUT_ADDR & 0xFU].is_used = 0U;
  pdev->pClassData = NULL;
  /* host gone: a game's effects must not survive (also stops the force) */
  {
    const uint8_t reset[FFB_LEN_DEVICE_CONTROL] = { FFB_RID_DEVICE_CONTROL, FFB_DC_RESET };
    ffb_pid_rx_isr(FFB_REPORT_OUTPUT, reset, sizeof(reset));
  }
  return USBD_OK;
}

static uint16_t min16(uint16_t a, uint16_t b)
{
  return (a < b) ? a : b;
}

static uint8_t class_request(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  const uint8_t type = (uint8_t)(req->wValue >> 8);
  const uint8_t id = (uint8_t)(req->wValue & 0xFFU);
  size_t n = 0U;

  switch (req->bRequest)
  {
    case HID_REQ_GET_REPORT:
      if ((type == FFB_REPORT_INPUT) && (id == FFB_RID_JOYSTICK))
      {
        memcpy(s_usb.reply, s_usb.last_joystick, sizeof(s_usb.last_joystick));
        n = sizeof(s_usb.last_joystick);
      }
      else
      {
        n = ffb_pid_get_report_isr(type, id, s_usb.reply, sizeof(s_usb.reply));
      }
      if (n == 0U)
      {
        USBD_CtlError(pdev, req);
        return USBD_FAIL;
      }
      USBD_CtlSendData(pdev, s_usb.reply, min16((uint16_t)n, req->wLength));
      return USBD_OK;

    case HID_REQ_SET_REPORT:
      s_usb.set_type = type;
      s_usb.set_len = min16(req->wLength, (uint16_t)sizeof(s_usb.ep0_buf));
      if (s_usb.set_len > 0U)
      {
        s_usb.set_pending = true;
        USBD_CtlPrepareRx(pdev, s_usb.ep0_buf, s_usb.set_len);
      }
      return USBD_OK;

    case HID_REQ_SET_IDLE:
      s_usb.idle = (uint8_t)(req->wValue >> 8);
      return USBD_OK;
    case HID_REQ_GET_IDLE:
      USBD_CtlSendData(pdev, (uint8_t *)&s_usb.idle, 1U);
      return USBD_OK;
    case HID_REQ_SET_PROTOCOL:
      s_usb.protocol = (uint8_t)req->wValue;
      return USBD_OK;
    case HID_REQ_GET_PROTOCOL:
      USBD_CtlSendData(pdev, (uint8_t *)&s_usb.protocol, 1U);
      return USBD_OK;
    default:
      USBD_CtlError(pdev, req);
      return USBD_FAIL;
  }
}

static uint8_t standard_request(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  static uint16_t status_info = 0U;

  switch (req->bRequest)
  {
    case USB_REQ_GET_STATUS:
      USBD_CtlSendData(pdev, (uint8_t *)&status_info, 2U);
      return USBD_OK;
    case USB_REQ_GET_DESCRIPTOR:
      if ((req->wValue >> 8) == HID_REPORT_DESC_TYPE)
      {
        USBD_CtlSendData(pdev, (uint8_t *)ffb_hid_report_desc,
                         min16(ffb_hid_report_desc_size, req->wLength));
        return USBD_OK;
      }
      if ((req->wValue >> 8) == HID_DESC_TYPE)
      {
        USBD_CtlSendData(pdev, &s_cfg[CFG_HID_OFFSET], min16(9U, req->wLength));
        return USBD_OK;
      }
      USBD_CtlError(pdev, req);
      return USBD_FAIL;
    case USB_REQ_GET_INTERFACE:
      USBD_CtlSendData(pdev, (uint8_t *)&s_usb.alt, 1U);
      return USBD_OK;
    case USB_REQ_SET_INTERFACE:
      s_usb.alt = (uint8_t)req->wValue;
      return USBD_OK;
    default:
      USBD_CtlError(pdev, req);
      return USBD_FAIL;
  }
}

static uint8_t ffb_setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
  switch (req->bmRequest & USB_REQ_TYPE_MASK)
  {
    case USB_REQ_TYPE_CLASS:
      return class_request(pdev, req);
    case USB_REQ_TYPE_STANDARD:
      return standard_request(pdev, req);
    default:
      USBD_CtlError(pdev, req);
      return USBD_FAIL;
  }
}

static uint8_t ffb_ep0_rx_ready(USBD_HandleTypeDef *pdev)
{
  (void)pdev;
  if (s_usb.set_pending)
  {
    s_usb.set_pending = false;
    ffb_pid_rx_isr(s_usb.set_type, s_usb.ep0_buf, s_usb.set_len);
  }
  return USBD_OK;
}

static uint8_t ffb_data_in(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  (void)pdev;
  (void)epnum;
  s_usb.in_busy = false;
  return USBD_OK;
}

static uint8_t ffb_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
  const uint32_t n = USBD_LL_GetRxDataSize(pdev, epnum);
  ffb_pid_rx_isr(FFB_REPORT_OUTPUT, s_usb.out_buf, (n > sizeof(s_usb.out_buf)) ? sizeof(s_usb.out_buf) : n);
  USBD_LL_PrepareReceive(pdev, CUSTOM_HID_EPOUT_ADDR, s_usb.out_buf, sizeof(s_usb.out_buf));
  return USBD_OK;
}

static uint8_t *ffb_cfg_desc(uint16_t *length)
{
  *length = (uint16_t)sizeof(s_cfg);
  return s_cfg;
}

static uint8_t *ffb_qualifier_desc(uint16_t *length)
{
  *length = (uint16_t)sizeof(s_qualifier);
  return s_qualifier;
}

USBD_ClassTypeDef USBD_FFB_HID = {
  ffb_init, ffb_deinit, ffb_setup, NULL, ffb_ep0_rx_ready, ffb_data_in, ffb_data_out,
  NULL, NULL, NULL, ffb_cfg_desc, ffb_cfg_desc, ffb_cfg_desc, ffb_qualifier_desc,
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
  NULL,
#endif
};

void usbd_ffb_install(USBD_HandleTypeDef *pdev)
{
  /* 16-bit report descriptor length in the HID descriptor */
  s_cfg[CFG_HID_OFFSET + 7U] = (uint8_t)(ffb_hid_report_desc_size & 0xFFU);
  s_cfg[CFG_HID_OFFSET + 8U] = (uint8_t)(ffb_hid_report_desc_size >> 8);
  memset(s_usb.last_joystick, 0, sizeof(s_usb.last_joystick));
  s_usb.last_joystick[0] = FFB_RID_JOYSTICK;
  s_usb.last_joystick[2] = 0x80U;           /* X = 32768 until the first report */
  /* No USBD_Stop()/USBD_Start() here: on the F3 the HAL stop powers the USB
   * cell down (FRES|PDWN) and start does not undo it. Swapping the class
   * pointer is enough because nothing has enumerated yet (the host waits
   * >100 ms after attach before the bus reset). */
  __disable_irq();
  (void)USBD_RegisterClass(pdev, &USBD_FFB_HID);
  __enable_irq();
}

bool usbd_ffb_in_ready(USBD_HandleTypeDef *pdev)
{
  return (pdev->dev_state == USBD_STATE_CONFIGURED) && (pdev->pClassData == &s_usb) && !s_usb.in_busy;
}

bool usbd_ffb_send(USBD_HandleTypeDef *pdev, uint8_t *report, uint16_t len)
{
  if ((len == FFB_LEN_JOYSTICK) && (report[0] == FFB_RID_JOYSTICK))
  {
    memcpy(s_usb.last_joystick, report, FFB_LEN_JOYSTICK);
  }
  if (!usbd_ffb_in_ready(pdev))
  {
    return false;
  }
  s_usb.in_busy = true;
  USBD_LL_Transmit(pdev, CUSTOM_HID_EPIN_ADDR, report, len);
  return true;
}
