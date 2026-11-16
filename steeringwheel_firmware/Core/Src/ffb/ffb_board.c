/**
 * @file    ffb_board.c
 * @brief   FFB hardware glue: binds ffb_service to the ODrive link, the UART
 *          write path, the LEDs and the DWT cycle counter. Firmware only.
 */
#include "ffb_board.h"

#include "ffb_config.h"
#include "ffb_pid.h"
#include "ffb_service.h"
#include "ffb_ui.h"
#include "main.h"
#include "odrive_link.h"
#include "odrive_uart.h"
#include "usbd_ffb_hid.h"

extern USBD_HandleTypeDef hUsbDeviceFS;

#if (STEERING_SOURCE != STEERING_SOURCE_ODRIVE)
#error "WHEEL_FFB needs STEERING_SOURCE=ODRIVE (the position must come from the ODrive)"
#endif

static ffb_service_t s_ffb;

static bool io_queue_read(const char *path)
{
  return odrive_link_queue_read(path);
}

static odr_read_state_t io_read_poll(float *value)
{
  return odrive_link_read_poll(value);
}

static bool io_send_write(const char *cmd, bool urgent)
{
  return odrive_uart_send_ffb(cmd, urgent);
}

static void io_link_request(uint32_t now_ms)
{
  odrive_link_request(now_ms);
}

static bool io_wheel(uint32_t now_ms, float *angle, float *vel, uint32_t *age)
{
  return odrive_link_wheel(now_ms, angle, vel, age);
}

static uint32_t io_timeouts(void)
{
  return odrive_link_stats()->timeouts;
}

static uint32_t io_replies(void)
{
  return odrive_link_stats()->replies_ok + odrive_link_stats()->reads_ok;
}

static uint32_t io_micros(void)
{
  return DWT->CYCCNT / (SystemCoreClock / 1000000U);
}

static float io_host_force(uint32_t now_ms, float pos_norm, float vel_norm, bool *active)
{
  return ffb_pid_force(now_ms, pos_norm, vel_norm, active);
}

static const ffb_io_t k_io = {
  io_queue_read, io_read_poll, io_send_write, io_link_request, io_wheel,
  io_timeouts, io_replies, io_host_force, io_micros,
};

void ffb_board_init(void)
{
  ffb_service_cfg_t cfg;

  /* DWT cycle counter for the loop-time statistics. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

#if FFB_ARM_PINS
  /* PD0 / PD1: arm buttons to GND, internal pull-up (released = high = not pressed; an unplugged wire
   * reads "released", so it can never arm). Not configured in CubeMX (.ioc untouched). */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  GPIO_InitTypeDef arm_gpio = {0};
  arm_gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  arm_gpio.Mode = GPIO_MODE_INPUT;
  arm_gpio.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOD, &arm_gpio);
#endif

  ffb_pid_init();
  usbd_ffb_install(&hUsbDeviceFS);      /* HID PID class instead of CustomHID */

  ffb_service_default_cfg(&cfg);
  ffb_service_init(&s_ffb, &cfg, &k_io, HAL_GetTick());
}

static void leds(uint32_t now_ms)
{
  const uint8_t l = ffb_service_leds(&s_ffb, now_ms);
  HAL_GPIO_WritePin(LD6_GPIO_Port, LD6_Pin, (l & FFB_LED_GREEN) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LD8_GPIO_Port, LD8_Pin, (l & FFB_LED_ORANGE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LD4_GPIO_Port, LD4_Pin, (l & FFB_LED_BLUE) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void ffb_board_poll(uint32_t now_ms)
{
  uint8_t status[FFB_STATUS_REPORT_LEN];
  const bool configured = (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED);

  /* Game reports -> effect engine (main-loop context only). */
  ffb_service_status_report(&s_ffb, status);
  ffb_pid_poll(now_ms, s_ffb.sm.state == FFB_STATE_RUNNING, status);
  /* No game force while the host is gone or suspended, or has disabled
   * the actuators; the built-in layer A effects keep working. */
  ffb_service_set_actuators(&s_ffb, configured && ffb_pid_actuators_enabled());

  ffb_service_poll(&s_ffb, now_ms);
  leds(now_ms);
}

void ffb_board_buttons(uint32_t now_ms, bool b1, uint16_t buttons)
{
#if FFB_ARM_PINS
  buttons = ffb_buttons_with_arm_pins(buttons,
                                      HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_0) == GPIO_PIN_RESET,
                                      HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_1) == GPIO_PIN_RESET);
#endif
  ffb_service_buttons(&s_ffb, b1, buttons, now_ms);
}

bool ffb_board_centre_allowed(uint32_t now_ms)
{
  return ffb_service_centre_allowed(&s_ffb, now_ms);
}

void ffb_board_status_report(uint8_t *out16)
{
  ffb_service_status_report(&s_ffb, out16);
}

void ffb_board_set_actuators(bool enabled)
{
  ffb_service_set_actuators(&s_ffb, enabled);
}

bool ffb_board_send_joystick(uint8_t *report9)
{
  return usbd_ffb_send(&hUsbDeviceFS, report9, FFB_LEN_JOYSTICK);
}

bool ffb_board_usb_ready(void)
{
  return usbd_ffb_in_ready(&hUsbDeviceFS);
}
