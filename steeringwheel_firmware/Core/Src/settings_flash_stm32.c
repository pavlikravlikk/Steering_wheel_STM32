/**
 * @file    settings_flash_stm32.c
 * @brief   settings_flash_t for the STM32F303VC: the LAST 2 KB flash page
 *          (0x0803F800). Compiled empty unless SETTINGS_FLASH_ENABLE = 1.
 *
 * NOTE: page erase stalls the CPU for about 20-40 ms (flash busy). It is only
 * ever done on an explicit "save" request, never in the normal loop, and
 * never while FFB is RUNNING. The firmware image (about 85 KB) is far from
 * the last page; the linker script still includes it in FLASH, so a much
 * larger image would need a reserved region in STM32F303xx_FLASH.ld first
 * (TODO(TBD) when enabling).
 */
#include "settings.h"

#ifndef SETTINGS_FLASH_ENABLE
#define SETTINGS_FLASH_ENABLE 0
#endif

#if SETTINGS_FLASH_ENABLE
#include <string.h>

#include "main.h"

#define SETTINGS_PAGE_ADDR  0x0803F800UL
#define SETTINGS_PAGE_SIZE  2048U

static bool fl_read(void *ctx, uint8_t *buf, size_t len)
{
  (void)ctx;
  memcpy(buf, (const void *)SETTINGS_PAGE_ADDR, len);
  return true;
}

static bool fl_erase(void *ctx)
{
  FLASH_EraseInitTypeDef e = {0};
  uint32_t err = 0U;
  HAL_StatusTypeDef st;
  (void)ctx;
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.PageAddress = SETTINGS_PAGE_ADDR;
  e.NbPages = 1U;
  HAL_FLASH_Unlock();
  st = HAL_FLASHEx_Erase(&e, &err);
  HAL_FLASH_Lock();
  return (st == HAL_OK) && (err == 0xFFFFFFFFUL);
}

static bool fl_write(void *ctx, const uint8_t *buf, size_t len)
{
  bool ok = true;
  (void)ctx;
  if ((len % 2U) != 0U || len > SETTINGS_PAGE_SIZE)
  {
    return false;
  }
  HAL_FLASH_Unlock();
  for (size_t i = 0U; ok && (i < len); i += 2U)
  {
    const uint16_t hw = (uint16_t)(buf[i] | (buf[i + 1U] << 8));
    ok = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, SETTINGS_PAGE_ADDR + i, hw) == HAL_OK;
  }
  HAL_FLASH_Lock();
  return ok;
}

const settings_flash_t g_settings_flash = { fl_read, fl_erase, fl_write, NULL };
#endif /* SETTINGS_FLASH_ENABLE */
