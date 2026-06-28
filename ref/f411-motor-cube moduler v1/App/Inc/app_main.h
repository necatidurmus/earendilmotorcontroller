/* ============================================================
 * App/Inc/app_main.h
 * Top-level app glue. Called from Core/Src/main.c.
 * ============================================================ */
#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void App_Init(void);
void App_Loop(void);

/* ISR shims (called from Core/Src/stm32f4xx_it.c) */
void App_Usart2RxIsr(uint16_t bytes);
void App_Tim6SchedulerTick(void);
void App_Tim1BrkIsr(void);
void App_Tim4HallIsr(void);

/* Read-only app state accessors for telemetry (see app_main.c). */
uint8_t App_GetTargetDuty(void);
uint8_t App_GetCurrentDuty(void);
int8_t  App_GetDirection(void);
uint8_t App_GetMotorPhase(void);
bool    App_IsSpeedMode(void);
bool    App_IsBrakeActive(void);

/* Identify result callback (called from service_task.c). */
void App_SetIdentifyResult(uint8_t result);

#ifdef __cplusplus
}
#endif

#endif /* APP_MAIN_H */
