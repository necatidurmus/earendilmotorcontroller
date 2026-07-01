/* ============================================================
 * App/Inc/app_config.h
 * Tunables, defaults, and the F411 protocol constants.
 * ============================================================ */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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
 * 96 MHz / (4799 + 1) = 20.0 kHz PWM (edge-aligned upcounting).
 * The motor driver maps the user-facing 0..PWM_MAX_DUTY range onto
 * 0..PWM_PERIOD_TICKS for the TIM1 CCR register. */
#define PWM_PERIOD_TICKS                 4799U
#define PWM_MAX_DUTY                     4000U

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
 * (base=20, boost=35, max PWM=100, Kp=0.6, Ki=0) that a
 * low-RPM hub motor could not produce enough torque to start.  The
 * legacy Arduino firmware used base=55, boost=65, max PWM=180,
 * Kp=0.8, Ki=0.1.  The values below are a middle ground: strong
 * enough to start the motor, still safe for a current-limited bench
 * PSU and no current sense.  Tune via `pi`, `base`, `boost` after
 * first motion is observed.
 *
 * Duty range is 0..PWM_MAX_DUTY (4000).  The old 0..250 values are
 * scaled by 16x so the same percentage duty is preserved.  Base and
 * boost use eight equal target-RPM bands across 0..MAX_RPM_TARGET;
 * boost duration is one shared value for all bands.
 * ---------------------------------------------------------------- */
#define DEFAULT_SPEED_KP                 0.8f
#define DEFAULT_SPEED_KI                 0.05f
#define SPEED_PI_KP_MAX                  10.0f
#define SPEED_PI_KI_MAX                  10.0f
#define DEFAULT_BASE_PWM_1               640U
#define DEFAULT_BASE_PWM_2               660U
#define DEFAULT_BASE_PWM_3               680U
#define DEFAULT_BASE_PWM_4               700U
#define DEFAULT_BASE_PWM_5               720U
#define DEFAULT_BASE_PWM_6               700U
#define DEFAULT_BASE_PWM_7               670U
#define DEFAULT_BASE_PWM_8               640U
#define DEFAULT_BOOST_PWM_1              880U
#define DEFAULT_BOOST_PWM_2              900U
#define DEFAULT_BOOST_PWM_3              920U
#define DEFAULT_BOOST_PWM_4              940U
#define DEFAULT_BOOST_PWM_5              960U
#define DEFAULT_BOOST_PWM_6              990U
#define DEFAULT_BOOST_PWM_7              1020U
#define DEFAULT_BOOST_PWM_8              1040U
#define DEFAULT_BOOST_TIME_MS            150U
#define DEFAULT_BOOST_EDGE_THRESH        3U
#define DEFAULT_RAMP_UP_RPM_SEC          60.0f
#define DEFAULT_RAMP_DOWN_RPM_SEC        150.0f
/* Max PWM for the PI output.  Without current sense, a stalled motor
 * will pull current limited only by the PSU setting — use a
 * current-limited bench supply and verify thermals before extended
 * high-duty operation. */
#define SPEED_PI_MAX_PWM                 PWM_MAX_DUTY
#define SPEED_PI_MIN_PWM                 0U
#define BOOST_RPM_THRESHOLD              3.0f
#define PID_INTERVAL_MS                  20U      /* 50 Hz */
#define PID_INTEGRAL_LIMIT               500.0f

/* ----------------------------------------------------------------
 * Hall stability / fault detection
 * ----------------------------------------------------------------
 * These mirror the Arduino firmware values to keep compatibility
 * with the old bring-up tests. */
#define HALL_STABLE_SAMPLES              2U
#define HALL_DEBOUNCE_US                 50U
#define INVALID_HALL_STOP_US             100000U  /* 100 ms */
#define START_NO_HALL_TIMEOUT_MS         700U
#define INVALID_TRANSITION_THRESHOLD     50U
#define DIRECTION_NEUTRAL_MS             80U
/* P0-02: Reduced from 5000ms to 2000ms.  During a stall with no
 * current sensing, PI applies max duty for the full timeout.  2s is
 * enough for low-RPM startup but limits MOSFET/PSU stress on stall. */
#define RPM_FEEDBACK_TIMEOUT_MS          2000U

/* P0-02b: Stall progressive duty reduction.  If no Hall edges for
 * longer than this but shorter than RPM_FEEDBACK_TIMEOUT_MS, the PI
 * output is progressively reduced to limit stress during a stall. */
#define STALL_DUTY_REDUCE_START_MS       500U
#define DUTY_HALL_LOSS_TIMEOUT_MS        1500U
#define CMD_WATCHDOG_MS                  800U
#define HOST_DISCONNECT_TIMEOUT_MS       2000U
#define BRAKE_HOLD_MS                    3000U
#define ARM_TIMEOUT_MS                   30000U

/* Maximum |RPM| target accepted by `rpm <signed>`.  Commands above
 * this are clamped and an [INFO] line is printed.  Bring-up is low
 * RPM only; this prevents a typo from commanding 3000 RPM. */
#define MAX_RPM_TARGET                   500

/* Duty-mode kick/ramp limits (ISSUE-038).
 * Ramp step is in PWM_MAX_DUTY units; default 128 matches the old
 * default of 8 in the 0..250 scale. */
#define KICK_DUTY_MAX                    PWM_MAX_DUTY
#define KICK_MS_MAX                      1000U
#define RAMP_STEP_MAX                    256U
#define RAMP_INTERVAL_MS_MAX             1000U
#define DEFAULT_PWM_MAX                  PWM_MAX_DUTY

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

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
