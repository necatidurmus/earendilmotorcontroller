/* ============================================================
 * App/Src/app_main.c
 *
 * Thin orchestrator.  Called from Core/Src/main.c.
 *   App_Init()    - one-time setup
 *   App_Loop()    - non-blocking service loop
 *   App_Tim6SchedulerTick() - TIM6 update ISR stub
 *   App_Tim1BrkIsr()      - TIM1 BRK ISR (HW break fault)
 *   App_Tim4HallIsr()     - TIM4 ISR stub (Hall uses EXTI)
 *
 * All real logic lives in:
 *   app_state.c        — AppState singleton
 *   app_utils.c        — string helpers
 *   app_status.c       — status/help/map output
 *   command_parser.c   — UART command dispatch
 *   motion_control.c   — motor state machine
 *   safety_watchdog.c  — watchdog service
 *   gate_test.c        — gate test timeout
 *
 * ============================================================ */

#include "app_main.h"
#include "app_state.h"
#include "app_config.h"
#include "command_parser.h"
#include "motion_control.h"
#include "safety_watchdog.h"
#include "gate_test.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "speed_pi.h"
#include "uart_protocol.h"
#include "telemetry.h"
#include "fault_manager.h"
#include "service_task.h"
#include "storage.h"
#include "config_snapshot.h"
#include "app_status.h"
#include "stm32f4xx_hal.h"

/* ----------------------------------------------------------------
 * ISR shims
 * ---------------------------------------------------------------- */

void App_Tim6SchedulerTick(void)
{
    /* No-op: scheduler is driven by SysTick / HAL_GetTick() in
     * App_Loop.  This stub is kept so the ISR prototype in
     * stm32f4xx_it.c remains link-clean even though TIM6 does not
     * exist on STM32F411. */
}

void App_Tim1BrkIsr(void)
{
    FaultManager_Raise(FAULT_HW_BREAK);
}

void App_Tim4HallIsr(void)
{
    /* No-op: Hall sensing uses EXTI on PB6/7/8 (ISSUE-007).
     * TIM4 Hall interface is not active.  Stub kept so the ISR
     * prototype in stm32f4xx_it.c remains link-clean. */
}

/* ----------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------- */

void App_Init(void)
{
    AppState *s = AppState_Get();

    /* Initialize all state to safe defaults. */
    AppState_InitDefaults(s);

    /* Bring up the peripherals. */
    MotorDriver_Init();
    Commutation_LoadDefaultMap();
    HallSensor_Init();
    SpeedPI_Init();
    UartProtocol_Init();
    Telemetry_Init();
    FaultManager_Init();
    ServiceTask_Init();

    /* Load saved hall map from flash (if any). */
    {
        uint8_t map[8];
        if (Storage_LoadHallMap(map)) {
            if (Commutation_ValidateHallMap(map)) {
                Commutation_ApplyMap(map);
                HallSensor_OnMapChanged();
                SpeedPI_Reset();
                s->hall_map_source = 3U;  /* FLASH */
                s->hall_map_dirty = false;
            } else {
                /* Flash map invalid — keep defaults, warn via status */
                s->hall_map_source = 4U;  /* INVALID_FALLBACK */
                s->hall_map_dirty = false;
            }
        }
    }

    /* Load saved config from flash (if any). */
    {
        PersistentConfig_t cfg;
        if (Storage_LoadConfig(&cfg) && ConfigSnapshot_Validate(&cfg)) {
            ConfigSnapshot_ApplyToRuntime(&cfg);
            UartProtocol_Printf("\r\n[OK] Config loaded from Flash seq=%lu",
                (unsigned long)Storage_GetConfigSequence());
        } else {
            UartProtocol_Print("\r\n[INFO] No valid config in Flash — defaults active");
        }
    }

    /* Print startup banner. */
    AppStatus_PrintHelp();
    UartProtocol_Print("\r\n[CUBE] f411-motor-cube firmware ready");
    UartProtocol_PrintNewline();
}

void App_Loop(void)
{
    AppState *s = AppState_Get();
    uint32_t now = HAL_GetTick();

    /* 1. Service Hall sensor. */
    HallSensor_Update();

    /* 1b. Gate test timeout. */
    GateTest_Service();

    /* 2. Drain UART ring buffer and parse any complete lines. */
    UartProtocol_Pump();
    char line[UART_LINE_MAX];
    UartSource src;
    while (UartProtocol_PopLine(line, sizeof(line), &src)) {
        CommandParser_Handle(line, src);
    }

    /* 3. Service tasks (identify/scan/test) — non-blocking. */
    ServiceTask_Update();

    /* 4. Speed PI tick (50 Hz internally). */
    SpeedPI_Tick(now);

    /* 5. Service motor outputs. */
    MotionControl_Service();

    /* 6. Watchdogs (command timeout, host disconnect). */
    SafetyWatchdog_Service();

    /* 7. Telemetry. */
    Telemetry_Tick(now);

    s->last_loop_ms = now;
}

/* ----------------------------------------------------------------
 * Read-only accessors for the telemetry layer.
 * ---------------------------------------------------------------- */

uint16_t App_GetTargetDuty(void)  { return AppState_Get()->target_duty; }
uint16_t App_GetCurrentDuty(void) { return AppState_Get()->current_duty; }
int8_t  App_GetDirection(void)    { return (int8_t)AppState_Get()->direction; }
uint8_t App_GetMotorPhase(void)   { return (uint8_t)AppState_Get()->phase; }
bool    App_IsSpeedMode(void)     { return AppState_Get()->mode == MODE_SPEED; }
bool    App_IsBrakeActive(void)   { return AppState_Get()->phase == PHASE_BRAKE; }

void App_SetIdentifyResult(uint8_t result)
{
    AppState *s = AppState_Get();
    s->identify_last_result = result;
    if (result == 1U) {
        s->hall_map_source = 1U;  /* RAM_IDENTIFY */
        s->hall_map_dirty = true;
    }
}
