/* ============================================================
 * App/Inc/app_config.h
 * Tunables, defaults, and the F411 <-> H7 protocol constants.
 * ============================================================ */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ----------------------------------------------------------------
 * Motor hardware
 * ----------------------------------------------------------------
 * To verify pole pairs on this motor:
 *   1 mechanical rev = hall_edges_per_rev
 *   pole_pairs       = hall_edges_per_rev / 6
 * Default below matches the existing F411 Arduino firmware
 * (POLE_PAIRS = 15). Confirm with an `identify` run before
 * changing.
 */
#define MOTOR_POLE_PAIRS                 15U

/* PWM timer period (TIM1 Auto-Reload Register) — set by TIM1 init.
 * The motor driver uses 0..PWM_PERIOD_TICKS as duty range.
 * 96 MHz / (4799 + 1) = 20.0 kHz PWM (edge-aligned upcounting). */
#define PWM_PERIOD_TICKS                 4799U

/* Six-step commutation: which phase is the active high-side
 * (PWM) and which is the active low-side (forced-on) for each
 * sector. Index = electrical sector 0..5. */
#define AH_PIN_GPIOPORT                  GPIOA
#define AH_PIN_NUMBER                    GPIO_PIN_8
#define BH_PIN_GPIOPORT                  GPIOA
#define BH_PIN_NUMBER                    GPIO_PIN_9
#define CH_PIN_GPIOPORT                  GPIOA
#define CH_PIN_NUMBER                    GPIO_PIN_10
#define AL_PIN_GPIOPORT                  GPIOA
#define AL_PIN_NUMBER                    GPIO_PIN_7
#define BL_PIN_GPIOPORT                  GPIOB
#define BL_PIN_NUMBER                    GPIO_PIN_0
#define CL_PIN_GPIOPORT                  GPIOB
#define CL_PIN_NUMBER                    GPIO_PIN_1

#define LED_PIN_GPIOPORT                 GPIOC
#define LED_PIN_NUMBER                   GPIO_PIN_13

/* Hall inputs */
#define HALL_A_GPIOPORT                  GPIOB
#define HALL_A_NUMBER                    GPIO_PIN_6
#define HALL_B_GPIOPORT                  GPIOB
#define HALL_B_NUMBER                    GPIO_PIN_7
#define HALL_C_GPIOPORT                  GPIOB
#define HALL_C_NUMBER                    GPIO_PIN_8

/* TIM1 channels: channel 1 -> phase A, channel 2 -> phase B, channel 3 -> phase C */
#define TIM_PHASE_A_CHANNEL              TIM_CHANNEL_1
#define TIM_PHASE_B_CHANNEL              TIM_CHANNEL_2
#define TIM_PHASE_C_CHANNEL              TIM_CHANNEL_3

/* ----------------------------------------------------------------
 * RPM / Hall measurement
 * ---------------------------------------------------------------- */
#define HALL_RPM_TIMEOUT_US              200000U  /* no edge -> RPM = 0 */
#define HALL_FILTER_ALPHA                0.25f

/* ----------------------------------------------------------------
 * Speed PI defaults
 *
 * ISSUE-040: the first cube revision set these so conservatively
 * (base low=20, boost low=35, max PWM=100, Kp=0.6, Ki=0) that a
 * low-RPM hub motor could not produce enough torque to start.  The
 * legacy Arduino firmware used base low=55, boost=65, max PWM=180,
 * Kp=0.8, Ki=0.1.  The values below are a middle ground: strong
 * enough to start the motor, still safe for a current-limited bench
 * PSU and no current sense.  Tune via `pi`, `base`, `boost` after
 * first motion is observed.
 * ---------------------------------------------------------------- */
#define DEFAULT_SPEED_KP                 0.8f
#define DEFAULT_SPEED_KI                 0.05f
#define DEFAULT_BASE_PWM_LOW             40U
#define DEFAULT_BASE_PWM_MID             45U
#define DEFAULT_BASE_PWM_HIGH            40U
#define DEFAULT_BOOST_LOW_PWM            55U
#define DEFAULT_BOOST_MID_PWM            60U
#define DEFAULT_BOOST_HIGH_PWM           65U
#define DEFAULT_BOOST_TIME_MS            150U
#define DEFAULT_BOOST_EDGE_THRESH        3U
#define DEFAULT_RAMP_UP_RPM_SEC          60.0f
#define DEFAULT_RAMP_DOWN_RPM_SEC        150.0f
/* Max PWM for the PI output.  180 matches the legacy firmware and
 * leaves headroom under the 250 hard clamp.  Without current sense,
 * do not raise above 200 without a current-limited PSU. */
#define SPEED_PI_MAX_PWM                 180U
#define SPEED_PI_MIN_PWM                 0U
#define BOOST_RPM_THRESHOLD              3.0f
#define PID_INTERVAL_MS                  20U      /* 50 Hz */
#define PID_INTEGRAL_LIMIT               500.0f

/* Conservative safety */
#define SPEED_PI_MAX_PWM_SOFT_LIMIT      180U     /* in case tuning raises it later */

/* ----------------------------------------------------------------
 * Hall stability / fault detection
 * ----------------------------------------------------------------
 * These mirror the Arduino firmware values to keep compatibility
 * with the old bring-up tests. */
#define HALL_STABLE_SAMPLES              2U
#define INVALID_HALL_STOP_US             100000U  /* 100 ms */
#define START_NO_HALL_TIMEOUT_MS         700U
#define INVALID_TRANSITION_THRESHOLD     50U
#define DIRECTION_NEUTRAL_MS             80U
#define RPM_FEEDBACK_TIMEOUT_MS          5000U
#define CMD_WATCHDOG_MS                  800U
#define HOST_DISCONNECT_TIMEOUT_MS       2000U
#define BRAKE_HOLD_MS                    3000U

/* Maximum |RPM| target accepted by `rpm <signed>`.  Commands above
 * this are clamped and an [INFO] line is printed.  Bring-up is low
 * RPM only; this prevents a typo from commanding 3000 RPM. */
#define MAX_RPM_TARGET                   500U

/* Duty-mode kick/ramp limits (ISSUE-038). */
#define KICK_DUTY_MAX                    250U
#define KICK_MS_MAX                      1000U
#define RAMP_STEP_MAX                    64U
#define RAMP_INTERVAL_MS_MAX             1000U
#define DEFAULT_PWM_MAX                  250U

/* ----------------------------------------------------------------
 * UART / protocol
 * ---------------------------------------------------------------- */
#define UART_LINE_MAX                    64U
#define CMD_QUEUE_LEN                    8U
#define RX_RING_LEN                      128U
#define CMD_BAUD                         115200U

/* Telemetry */
#define TELEMETRY_INTERVAL_MS            100U

/* Default Hall map. 255 = invalid Hall code (e.g. 0b000 / 0b111). */
#define DEFAULT_HALL_MAP_0               255U
#define DEFAULT_HALL_MAP_1               1U
#define DEFAULT_HALL_MAP_2               3U
#define DEFAULT_HALL_MAP_3               2U
#define DEFAULT_HALL_MAP_4               5U
#define DEFAULT_HALL_MAP_5               0U
#define DEFAULT_HALL_MAP_6               4U
#define DEFAULT_HALL_MAP_7               255U

/* Hall-valid state range */
#define HALL_STATE_INVALID               255U

#endif /* APP_CONFIG_H */
