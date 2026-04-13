/*
 * usbd_conf.h — USB Device donanım soyutlama katmanı bildirimleri
 *
 * STM32Cube USB Device Library, donanıma özgü geri çağırma işlevleri için
 * bu dosyayı kullanır. HAL PCD (Peripheral Control Driver) ile entegre çalışır.
 *
 * STM32F411 USB OTG FS:
 *   - PA11 = OTG_FS_DM (D-)
 *   - PA12 = OTG_FS_DP (D+)
 *   - 48 MHz USB saati: PLLQ=4, VCO=192 MHz → 192/4 = 48 MHz
 */

#ifndef USBD_CONF_H
#define USBD_CONF_H

#include "stm32f4xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Endpoint sayısı — CDC için 3 yeterli (control + data in + data out) */
#define USBD_MAX_NUM_INTERFACES      1U
#define USBD_MAX_NUM_CONFIGURATION   1U
#define USBD_MAX_POWER               100U  /* mA */
#define USBD_IDX_MFC_STR             0x01U
#define USBD_IDX_PRODUCT_STR         0x02U
#define USBD_IDX_SERIAL_STR          0x03U
#define USBD_SUPPORT_USER_STRING_DESC 0U

/* Debug: 0=kapalı, 1=açık (UART gerektirir, CDC modda kapalı tutun) */
#define USBD_DEBUG_LEVEL             0U

/* Memory allocation — standart libc malloc/free kullan */
#define USBD_malloc         malloc
#define USBD_free           free
#define USBD_memset         memset
#define USBD_memcpy         memcpy
#define USBD_Delay          HAL_Delay

#if (USBD_DEBUG_LEVEL > 0U)
#define  USBD_UsrLog(...)   /* buraya UART print eklenebilir */
#define  USBD_ErrLog(...)
#define  USBD_DbgLog(...)
#else
#define  USBD_UsrLog(...)   do {} while (0)
#define  USBD_ErrLog(...)   do {} while (0)
#define  USBD_DbgLog(...)   do {} while (0)
#endif

#endif /* USBD_CONF_H */
