/**
 * @file    hid_report.c
 * @brief   1 kHz input collection and USB HID report transmission.
 */
#include "hid_report.h"

#include "buttons_mcp23017.h"
#include "debounce.h"
#include "encoder.h"
#include "hid_pack.h"
#include "main.h"
#include "odrive_link.h"
#include "steering_pot.h"
#include "usbd_customhid.h"
#include "usbd_custom_hid_if.h"
#include "wheel_config.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
extern ADC_HandleTypeDef  hadc1;
extern I2C_HandleTypeDef  hi2c1;

/* These settings live in generated code (usbd_conf.h). Fail the build if a
 * CubeMX regeneration from an un-updated .ioc silently reverts them. */
_Static_assert(USBD_CUSTOM_HID_REPORT_DESC_SIZE == 42U,
               "Report descriptor size must be 42 (update USB_DEVICE settings in .ioc)");
_Static_assert(CUSTOM_HID_FS_BINTERVAL == 1U,
               "HID polling interval must be 1 ms (update FS binterval in .ioc)");
_Static_assert(sizeof(JoystickReport_t) == 8U, "HID report must be 8 bytes");
_Static_assert(HID_REPORT_SIZE == sizeof(JoystickReport_t), "hid_pack and descriptor disagree");

static JoystickReport_t s_report;          /* logical values (host byte order)          */
static uint8_t          s_tx[HID_REPORT_SIZE]; /* packed bytes; static: valid for the PMA copy */
static volatile uint8_t s_tick_pending;
static uint32_t         s_ms;
static uint32_t         s_heartbeat_ms;
static debounce_t       s_b1_debounce;     /* blue user button B1 (PA0)     */
static hold_detector_t  s_b1_hold;         /* hold -> set steering centre   */

#if (STEERING_SOURCE != STEERING_SOURCE_ODRIVE) && (STEERING_SOURCE != STEERING_SOURCE_ENCODER) \
    && (STEERING_SOURCE != STEERING_SOURCE_POT)
#error "STEERING_SOURCE must be STEERING_SOURCE_ODRIVE, _ENCODER or _POT"
#endif

/* ---------------------------------------------------------------------- */
/* 1 kHz tick: TIM6 update interrupt                                      */
/* ---------------------------------------------------------------------- */
void TIM6_DAC_IRQHandler(void)
{
  if ((TIM6->SR & TIM_SR_UIF) != 0U)
  {
    TIM6->SR = ~TIM_SR_UIF;
    s_tick_pending = 1U;
  }
}

static void tick_timer_start(void)
{
  /* Timer clock = PCLK1, doubled when the APB1 prescaler is not 1. */
  uint32_t tim_clk = HAL_RCC_GetPCLK1Freq();
  if ((RCC->CFGR & RCC_CFGR_PPRE1_2) != 0U)
  {
    tim_clk *= 2U;
  }

  __HAL_RCC_TIM6_CLK_ENABLE();
  TIM6->CR1  = 0U;
  TIM6->PSC  = (uint16_t)((tim_clk / 1000000U) - 1U);   /* 1 MHz counter    */
  TIM6->ARR  = 999U;                                    /* 1000 us = 1 kHz  */
  TIM6->EGR  = TIM_EGR_UG;                              /* load PSC/ARR     */
  TIM6->SR   = 0U;
  TIM6->DIER = TIM_DIER_UIE;

  HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
  TIM6->CR1  = TIM_CR1_CEN;
}

/* ---------------------------------------------------------------------- */
/* Simulation (SIMULATE_INPUTS)                                           */
/* ---------------------------------------------------------------------- */
#if SIMULATE_INPUTS
/* Smooth sine approximation without libm: parabola refined with the usual
 * 0.225 correction (max error about 0.1 %). phase is 0..1, result -1..1. */
static float sine_approx(float phase)
{
  const float x = (phase * 6.2831853f) - 3.1415927f;     /* -pi..pi        */
  float y = (1.2732395f * x) - (0.4052847f * x * (x < 0.0f ? -x : x));
  y = 0.225f * (y * (y < 0.0f ? -y : y) - y) + y;
  return -y;                                             /* sin(x + pi)    */
}

static uint16_t simulated_x(uint32_t ms)
{
  const float phase = (float)(ms % 4000U) / 4000.0f;     /* 4 s period     */
  const float v = 32768.0f + (32767.0f * sine_approx(phase));
  return (uint16_t)(v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v));
}

static uint16_t simulated_buttons(uint32_t ms)
{
  /* Button n pressed for 250 ms, released for 250 ms, then the next one. */
  const uint32_t step = ms / 250U;
  if ((step & 1U) != 0U)
  {
    return 0U;
  }
  return (uint16_t)(1U << ((step / 2U) % BUTTON_COUNT));
}
#endif /* SIMULATE_INPUTS */

/* ---------------------------------------------------------------------- */
/* Steering source                                                        */
/* ---------------------------------------------------------------------- */
#if !SIMULATE_INPUTS
static void steering_init(void)
{
#if (STEERING_SOURCE == STEERING_SOURCE_ODRIVE)
  odrive_link_init();          /* USART1 PA9/PA10, read-only "f 0" polling */
  (void)hadc1;
#elif (STEERING_SOURCE == STEERING_SOURCE_ENCODER)
  encoder_init();
  (void)hadc1;
#else
  steering_pot_init(&hadc1);
#endif
}

/** Called every 1 ms tick. ODrive RX/TX is serviced from the main loop
 *  (main.c); here only the latest mapped value is picked up. */
static uint16_t steering_axis16(void)
{
#if (STEERING_SOURCE == STEERING_SOURCE_ODRIVE)
  return odrive_link_axis16();
#elif (STEERING_SOURCE == STEERING_SOURCE_ENCODER)
  encoder_update();
  return encoder_get_axis16();
#else
  steering_pot_update();
  return steering_pot_get_axis16();
#endif
}
#endif /* !SIMULATE_INPUTS */

static bool steering_set_centre(void)
{
#if (STEERING_SOURCE == STEERING_SOURCE_ODRIVE)
  return odrive_link_set_centre(HAL_GetTick());
#elif (STEERING_SOURCE == STEERING_SOURCE_ENCODER)
  encoder_set_center();
  return true;
#else
  return false;                /* a potentiometer has a fixed centre */
#endif
}

/** Blue user button B1 (PA0, active high, external pull-down on the
 *  Discovery board): debounced, held SET_CENTRE_HOLD_MS -> set centre. */
static void centre_button_task(uint32_t now_ms)
{
  const uint16_t raw = (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_SET) ? 1U : 0U;
  const bool pressed = (debounce_update(&s_b1_debounce, raw, now_ms) & 1U) != 0U;

  if (hold_update(&s_b1_hold, pressed, now_ms))
  {
    if (steering_set_centre())
    {
      HAL_GPIO_TogglePin(LD9_GPIO_Port, LD9_Pin);   /* blue: centre accepted */
    }
  }
}

/* ---------------------------------------------------------------------- */
/* Public API                                                             */
/* ---------------------------------------------------------------------- */
void hid_report_init(void)
{
  debounce_init(&s_b1_debounce, 1U, BUTTON_DEBOUNCE_MS, 0U);
  hold_init(&s_b1_hold, SET_CENTRE_HOLD_MS);

#if !SIMULATE_INPUTS
  steering_init();
  buttons_init(&hi2c1);
#else
  (void)hadc1;
  (void)hi2c1;
#endif

  tick_timer_start();
}

static void status_leds(uint32_t now_ms)
{
  if ((now_ms - s_heartbeat_ms) >= 500U)
  {
    s_heartbeat_ms = now_ms;
    HAL_GPIO_TogglePin(LD3_GPIO_Port, LD3_Pin);
  }
  HAL_GPIO_WritePin(LD7_GPIO_Port, LD7_Pin,
                    (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED)
                        ? GPIO_PIN_SET : GPIO_PIN_RESET);
#if (STEERING_SOURCE == STEERING_SOURCE_ODRIVE) && !SIMULATE_INPUTS
  /* Orange LD5 = ODrive position missing or stale (X is being held). */
  HAL_GPIO_WritePin(LD5_GPIO_Port, LD5_Pin,
                    odrive_link_ok(HAL_GetTick()) ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

static uint8_t usb_ready_for_report(void)
{
  const USBD_CUSTOM_HID_HandleTypeDef *hhid =
      (const USBD_CUSTOM_HID_HandleTypeDef *)hUsbDeviceFS.pClassData;

  return (hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED)
      && (hhid != NULL)
      && (hhid->state == CUSTOM_HID_IDLE);
}

void hid_report_poll(void)
{
  if (s_tick_pending == 0U)
  {
    return;
  }
  s_tick_pending = 0U;
  s_ms++;

  /* 1. Collect state. */
#if SIMULATE_INPUTS
  s_report.x       = simulated_x(s_ms);
  s_report.buttons = simulated_buttons(s_ms);
#else
  s_report.x = steering_axis16();
  buttons_update(s_ms);
  s_report.buttons = buttons_get();
#endif
  s_report.y = HID_AXIS_Y_NEUTRAL;   /* throttle: reserved for the future pedals */
  s_report.z = HID_AXIS_Z_NEUTRAL;   /* brake:    reserved for the future pedals */

  centre_button_task(s_ms);
  status_leds(s_ms);

  /* 2. Send only when the previous IN transfer has completed. */
  if (usb_ready_for_report() != 0U)
  {
    hid_pack_report(s_tx, s_report.x, s_report.y, s_report.z,
                    s_report.buttons, BUTTON_COUNT);
    (void)USBD_CUSTOM_HID_SendReport(&hUsbDeviceFS, s_tx, (uint16_t)sizeof(s_tx));
  }
}
