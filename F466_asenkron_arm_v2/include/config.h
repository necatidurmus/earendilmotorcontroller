#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include "types.h"

// ============================================================
// STM32F446RE Nucleo UART pin map - FL/FR + Mechanical Arm
// ============================================================
// PC terminal / ST-LINK VCP:
//   USART2 TX = PA2
//   USART2 RX = PA3
//
// Active wheel controllers:
//   FL -> USART1 TX = PA9   RX = PA10, optional
//   FR -> USART3 TX = PC10  RX = PC11, optional
//
// Disabled wheel controllers:
//   RL/RR are not initialized and are never contacted.
//
// Mechanical arm bridge:
//   Laptop/GUI sends:       arm <payload>
//   F446 sends to arm MCU:  <payload>
//   Default arm UART: USART6 TX=PC6, RX=PC7
// ============================================================
#define TERMINAL_TX_PIN       PA2
#define TERMINAL_RX_PIN       PA3
#define TERMINAL_BAUD         115200

#define MOTOR_UART_RX_ENABLED 1

#define MOTOR_FL_TX_PIN       PA9    // USART1_TX
#define MOTOR_FL_RX_PIN       PA10   // USART1_RX, used only if MOTOR_UART_RX_ENABLED=1

#define MOTOR_FR_TX_PIN       PC10   // USART3_TX
#define MOTOR_FR_RX_PIN       PC11   // USART3_RX, used only if MOTOR_UART_RX_ENABLED=1

#if MOTOR_UART_RX_ENABLED
  #define MOTOR_FL_RX_INIT_PIN MOTOR_FL_RX_PIN
  #define MOTOR_FR_RX_INIT_PIN MOTOR_FR_RX_PIN
#else
  #define MOTOR_FL_RX_INIT_PIN NC
  #define MOTOR_FR_RX_INIT_PIN NC
#endif

#define MOTOR_UART_BAUD       115200

// Mechanical arm UART. TX works even if RX bridge is off.
#define ARM_UART_ENABLED          1
#define ARM_UART_RX_ENABLED       1
#define ARM_TX_PIN                PC6   // USART6_TX -> Arm controller RX
#define ARM_RX_PIN                PC7   // USART6_RX <- Arm controller TX
#define ARM_UART_BAUD             115200

#if ARM_UART_RX_ENABLED
  #define ARM_RX_INIT_PIN ARM_RX_PIN
#else
  #define ARM_RX_INIT_PIN NC
#endif

// Keep RX bridge OFF by default because a disconnected ARM RX pin can float
// and flood the terminal. Turn it on from GUI/terminal with: armbridge on
#define ARM_RX_BRIDGE_DEFAULT_ON  0
#define ARM_RX_MAX_LINE           160
#define ARM_RX_MAX_BYTES_PER_LOOP 64
#define ARM_TRUNCATE_NOTICE_MS    1000

// ============================================================
// Safety / timing
// ============================================================
#define PWM_MIN               0
#define PWM_MAX               255

// sendLineSafe() will never wait forever on a bad UART.
#define UART_TX_CHAR_TIMEOUT_MS  5
#define UART_TX_GAP_MS           2
#define IDENTIFY_BETWEEN_MS      25

// Emergency stop input. F446 uses PB0 because PC10 is already FR USART3_TX.
//
// IMPORTANT WIRING FOR THIS BUILD (default):
//   PB0 ---- E-STOP contact ---- GND
//   normal/released -> PB0 pulled HIGH by INPUT_PULLUP -> NORMAL
//   pressed/closed  -> PB0 shorted to GND              -> ACTIVE
//
// This default is chosen so an unconnected PB0 does NOT lock the robot during
// bring-up. If you wire a fail-safe NC E-STOP where normal=LOW and pressed/open=HIGH,
// change ESTOP_ACTIVE_LEVEL to HIGH. Use the `estop` command to see raw pin level.
#define ESTOP_ENABLED         1
#define ESTOP_PIN             PB0
#define ESTOP_ACTIVE_LEVEL    LOW
#define ESTOP_DEBOUNCE_MS     20
#define ESTOP_REASSERT_INTERVAL_MS 250

// Heartbeat LED for proving the MCU loop is alive.
#define HEARTBEAT_ENABLED     1
#define HEARTBEAT_INTERVAL_MS 500

// Terminal line handling
#define TERMINAL_LINE_MAX     120
#define TERMINAL_PROMPT       "> "

// ============================================================
// Explicit HardwareSerial objects
// Defined once in src/main.cpp
// ============================================================
extern HardwareSerial TerminalSerial;
extern HardwareSerial WheelSerialFL;
extern HardwareSerial WheelSerialFR;
#if ARM_UART_ENABLED
extern HardwareSerial ArmSerial;
#endif

#define TERMINAL_UART   TerminalSerial
#define MOTOR_UART_FL   WheelSerialFL
#define MOTOR_UART_FR   WheelSerialFR
#if ARM_UART_ENABLED
#define ARM_UART        ArmSerial
#endif

inline const char* motorName(uint8_t id) {
    switch (id) {
        case MOTOR_FL: return "FL";
        case MOTOR_FR: return "FR";
        default:       return "??";
    }
}

#endif // CONFIG_H
