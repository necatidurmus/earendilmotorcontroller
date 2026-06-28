#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "types.h"

// ============================================================
// Emergency Stop Input
// NC button:
// - normal state  -> button closed to GND  -> pin reads LOW
// - pressed state -> line open, pulled up  -> pin reads HIGH
// Change ESTOP_PIN to the GPIO you actually wired.
// ============================================================
#define ESTOP_PIN             PB14
#define ESTOP_ACTIVE_LEVEL    HIGH
#define ESTOP_DEBOUNCE_MS     20
#define ESTOP_REASSERT_INTERVAL_MS 250

// ============================================================
// UART Configurations
// ============================================================

// Terminal (PC) — USB Serial
#define TERMINAL_UART       Serial
#define TERMINAL_BAUD       115200

// FTDI debug UART (USART6) — PG9=RX, PG14=TX
#define FTDI_UART           Serial6
#define FTDI_BAUD           115200

// Wheel motor UARTs
#define MOTOR_UART_FL       Serial2   // USART2 -> RX: PD6, TX: PD5
#define MOTOR_UART_RL       Serial7   // UART7  -> RX: PE7, TX: PE8
#define MOTOR_UART_FR       Serial4   // UART4  -> RX: PD0, TX: PD1
#define MOTOR_UART_RR       Serial5   // UART5  -> RX: PD2, TX: PC12
#define MOTOR_UART_BAUD     115200

// Dedicated ARM UART
#define ARM_UART            Serial8   // USART1 -> RX: PE0, TX: PE1
#define ARM_UART_BAUD       115200
#define ARM_TX_PIN          PE1
#define ARM_RX_PIN          PE0

// ARM RX bridge settings
#define ARM_UART_RX_ENABLED       1
#define ARM_RX_BRIDGE_DEFAULT_ON  0
#define ARM_RX_MAX_LINE           160
#define ARM_RX_MAX_BYTES_PER_LOOP 64
#define ARM_TRUNCATE_NOTICE_MS    1000

// ============================================================
// UART TX Safety
// ============================================================
#define UART_TX_CHAR_TIMEOUT_MS  5
#define UART_TX_GAP_MS           2
#define IDENTIFY_BETWEEN_MS      25

// ============================================================
// PWM Configuration (Duty mode)
// F411 duty clamp 0..250 ile tutarli.  H7 kullanici tarafinda
// 251+ degerler kabul edilmez; parser hata mesaji verir.
// ============================================================
#define PWM_MIN             0
#define PWM_MAX             250

// ============================================================
// RPM Configuration (Speed PI mode)
// H7 -> F411 "rpm <signed>" komutu icin limitler
// F411 MAX_RPM_TARGET=500 ile tutarli
// ============================================================
#define RPM_MIN             0
#define RPM_MAX             500
#define RPM_DEFAULT         23

// Motion komutu generic deger ust siniri (pwm veya rpm)
// parser bu sinira kadar kabul eder, motion_controller mode'a gore clamp eder
#define DRIVE_VALUE_MAX     500

// F411 komut broadcast arasi bekleme (multi-motor forwarding)
#define F411_FWD_GAP_MS     8

// ============================================================
// Wheel Telemetry Bridge (F411 -> H7 -> PC)
// F411 UART RX'inden gelen satirlari motor prefix ile PC'ye aktarir
// ============================================================
#define WHEEL_BRIDGE_DEFAULT_ON       0
#define WHEEL_RX_MAX_LINE             160
#define WHEEL_RX_MAX_BYTES_PER_LOOP   48
#define WHEEL_TRUNCATE_NOTICE_MS      1000

// ============================================================
// Protocol Configuration
// ============================================================
#define PROTOCOL_SYNC_BYTE  0xAA
#define PROTOCOL_MAX_PACKET 8
#define PROTOCOL_TIMEOUT_MS 100

// ============================================================
// Terminal Configuration
// ============================================================
#define TERMINAL_LINE_MAX   120
#define TERMINAL_PROMPT     "> "

// ============================================================
// Heartbeat LED
// ============================================================
#define HEARTBEAT_ENABLED     1
#define HEARTBEAT_INTERVAL_MS 500

// ============================================================
// Independent Watchdog (IWDG)
// H723ZG: LSI ~32kHz, IWDG zamanlayıcı. Loop donarsa reset.
// Timeout 2sn — loop normalde <10ms, UART TX worst-case dahil.
// STM32duino IWatchdog.begin() timeout parametresini milisaniye
// (ms) olarak alir.  IWDG_TIMEOUT_MS dogrudan kullanilir.
// ============================================================
#define IWDG_ENABLED          1
#define IWDG_TIMEOUT_MS       2000UL

// ============================================================
// RED Relay Configuration
// ============================================================
#define RED_RELAY_PIN                  PB15
#define RED_RELAY_MODULE_ACTIVE_LEVEL  LOW
#define RED_RELAY_CONTACT_IS_NC        1
#define RED_RELAY_DEFAULT_ON           1

// GREEN
#define GREEN_RELAY_PIN                  PB13
#define GREEN_RELAY_MODULE_ACTIVE_LEVEL  LOW
#define GREEN_RELAY_CONTACT_IS_NC        1
#define GREEN_RELAY_DEFAULT_ON           1

// YELLOW
#define YELLOW_RELAY_PIN                  PB12
#define YELLOW_RELAY_MODULE_ACTIVE_LEVEL  LOW
#define YELLOW_RELAY_CONTACT_IS_NC        1
#define YELLOW_RELAY_DEFAULT_ON           1

inline const char* motorName(uint8_t id) {
    switch (id) {
        case MOTOR_FL: return "FL";
        case MOTOR_RL: return "RL";
        case MOTOR_FR: return "FR";
        case MOTOR_RR: return "RR";
        default:       return "??";
    }
}

#endif // CONFIG_H