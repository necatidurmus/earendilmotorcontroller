#ifndef F446_BRIDGE_CONFIG_H
#define F446_BRIDGE_CONFIG_H

#include <Arduino.h>

#ifndef PC_BAUD
#define PC_BAUD 115200
#endif

#ifndef MOTOR_BAUD
#define MOTOR_BAUD 115200
#endif

#ifndef MOTOR_TX_PIN
#define MOTOR_TX_PIN PA9
#endif

#ifndef MOTOR_RX_PIN
#define MOTOR_RX_PIN PA10
#endif

#ifndef LED_PIN
#define LED_PIN LED_BUILTIN
#endif

static constexpr size_t PC_LINE_MAX   = 160;
static constexpr size_t M1_LINE_MAX   = 192;
static constexpr size_t MOTOR_COUNT   = 1;

static constexpr uint32_t LED_BLINK_MS        = 500;
static constexpr uint32_t SAFE_STOP_DELAY_MS  = 15;
static constexpr uint32_t SERVICE_TIMEOUT_MS  = 30000;

#endif
