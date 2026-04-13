/*
 * main.c — Earendil BLDC Motor Controller entry point
 *
 * STM32F411 Black Pill + L6388 + 6-NMOS sensored 6-step BLDC driver
 *
 * Architecture:
 *   - TIM3 ISR at 12.5 kHz calls MotorControl_Tick()
 *   - Main loop handles CLI and LED blink only
 *   - No Arduino dependency — pure STM32Cube HAL
 *
 * Shared state between ISR and main loop:
 *   - g_runMode: volatile RunMode (written by CLI, read by ISR)
 *   - g_commandDuty: volatile uint16_t (written by CLI, read by ISR)
 *   - g_appliedDuty: volatile uint16_t (written by ISR, read by CLI)
 *   - g_isrTickCount: volatile uint32_t (written by ISR, read by CLI)
 *
 * Safety:
 *   - On fault: outputs off immediately, duty zeroed, mode set to STOPPED
 *   - On invalid hall: outputs off
 *   - On hard overcurrent: latched fault, requires CLI 'clear'
 *   - On soft overcurrent: duty reduced proportionally
 */

#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdbool.h>

#include "motor_config.h"
#include "board_io.h"
#include "hall.h"
#include "bldc_commutation.h"
#include "protection.h"
#include "cli.h"

/* ====================================================================
 * Shared state (ISR <-> main loop)
 * ==================================================================== */

typedef enum {
    RUN_STOPPED = 0,
    RUN_FORWARD = 1,
    RUN_BACKWARD = 2
} RunMode;

volatile RunMode   g_runMode     = RUN_STOPPED;
volatile uint16_t  g_commandDuty = DUTY_DEFAULT;
volatile uint16_t  g_appliedDuty = 0;
volatile uint32_t  g_isrTickCount = 0;

/* Duty to PWM conversion: scale 0..255 command to 0..PWM_PERIOD_COUNTS */
static inline uint16_t dutyToPwm(uint16_t duty) {
    return (uint16_t)((uint32_t)duty * PWM_PERIOD_COUNTS / 255);
}

/* ====================================================================
 * MotorControl_Tick() — Called from TIM3 ISR at 12.5 kHz
 *
 * This is the ENTIRE motor control hot path.
 * Keep it short, deterministic, no blocking, no prints.
 * ==================================================================== */

void MotorControl_Tick(void) {
    g_isrTickCount++;

    /* 1) Sample ADC (decimated inside) */
    Prot_SampleTick();

    /* 2) Hard overcurrent check */
    if (Prot_CheckHardLimit()) {
        /* Fault was latched — outputs already off */
        g_runMode = RUN_STOPPED;
        g_appliedDuty = 0;
        return;
    }

    /* 3) Check for stopped/faulted/zero-duty conditions */
    RunMode mode = (RunMode)g_runMode;
    uint16_t cmdDuty = g_commandDuty;

    if (mode == RUN_STOPPED || Prot_IsFaulted() || cmdDuty == 0) {
        g_appliedDuty = 0;
        Comm_AllOff();
        return;
    }

    /* 4) Resolve hall to commutation state */
    uint32_t nowUsFine = (g_isrTickCount * 80);  /* 80 us per tick at 12.5 kHz */
    uint8_t baseState = Hall_ResolveState(nowUsFine);
    if (baseState > 5) {
        /* Invalid hall — outputs off */
        g_appliedDuty = 0;
        Comm_AllOff();
        return;
    }

    /* 5) Soft current limit */
    uint16_t targetDuty = Prot_ApplySoftLimit(cmdDuty);

    /* Apply minimum duty floor */
    if (targetDuty > 0 && targetDuty < DUTY_MIN_ACTIVE) {
        targetDuty = DUTY_MIN_ACTIVE;
    }

    /* 6) Slew limit */
    g_appliedDuty = Prot_SlewDuty(g_appliedDuty, targetDuty);

    /* 7) Resolve drive state (direction) */
    uint8_t driveState;
    if (mode == RUN_FORWARD) {
        driveState = baseState;
    } else {
        /* BACKWARD: opposite voltage vector */
        int16_t bs = (int16_t)baseState + 3;
        driveState = (uint8_t)(bs % 6);
    }

    /* 8) Apply commutation */
    uint8_t prevState = Comm_GetActiveState();
    bool stateChanged = (driveState != prevState);
    uint16_t pwmDuty = dutyToPwm(g_appliedDuty);

    Comm_Apply(driveState, pwmDuty, stateChanged);
}

/* ====================================================================
 * Main entry point
 * ==================================================================== */

int main(void) {
    /* Initialize all hardware */
    BoardIO_InitAll();

    /* Initialize hall processing */
    HallConfig hallCfg = {
        .profile = 0,
        .polarityMask = 0,
        .stateOffset = 0
    };
    Hall_Init(&hallCfg);
    Hall_SetDirection(1);  /* forward by default */

    /* Initialize commutation */
    Comm_Init();

    /* Initialize protection */
    ProtectionConfig protCfg = {
        .softLimitAdc = CURRENT_SOFT_LIMIT,
        .hardLimitAdc = CURRENT_HARD_LIMIT,
        .hardStrikesToTrip = HARD_LIMIT_STRIKES
    };
    Prot_Init(&protCfg);

    /* Calibrate current sensor offset */
    {
        const char *msg = "Calibrating ISENSE...\r\n";
        HAL_UART_Transmit(&huart2, (uint8_t *)msg, strlen(msg), 200);
    }
    Prot_CalibrateOffset();

    /* Print banner via raw HAL */
    {
        const char *banner =
            "\r\n"
            "========================================\r\n"
            " Earendil BLDC Motor Controller\r\n"
            " STM32F411 + L6388 + 6-NMOS\r\n"
            " 6-step sensored trapezoidal\r\n"
            " Control: 12.5 kHz timer ISR\r\n"
            " CLI: UART2 @ 115200 (PA2/PA3)\r\n"
            "========================================\r\n"
            "\r\n";
        HAL_UART_Transmit(&huart2, (uint8_t *)banner, strlen(banner), 500);
    }

    /* Initialize CLI (prints help) */
    CLI_Init();

    /* Start control timer — motor control begins now */
    BoardIO_StartControlTimer();

    /* LED blink state */
    uint32_t lastBlinkMs = HAL_GetTick();

    /* ====================================================================
     * Main loop — only non-critical tasks
     * Motor control is entirely in TIM3 ISR.
     * ==================================================================== */
    while (1) {
        /* Service CLI */
        CLI_Service();

        /* LED blink every 400 ms */
        uint32_t nowMs = HAL_GetTick();
        if ((nowMs - lastBlinkMs) >= 400) {
            lastBlinkMs = nowMs;
            BoardIO_LEDToggle();
        }
    }
}
