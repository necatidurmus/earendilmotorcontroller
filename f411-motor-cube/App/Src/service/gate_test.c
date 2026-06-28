/* ============================================================
 * App/Src/gate_test.c
 * Gate test timeout logic.
 * Extracted from app_main.c — behaviour must be identical.
 * ============================================================ */
#include "gate_test.h"
#include "app_state.h"
#include "motor_driver.h"
#include "uart_protocol.h"
#include "stm32f4xx_hal.h"

void GateTest_Init(void)
{
    /* Nothing extra — AppState_InitDefaults handles init. */
}

bool GateTest_IsActive(void)
{
    return AppState_Get()->gatetest_active;
}

void GateTest_Service(void)
{
    AppState *s = AppState_Get();
    if (!s->gatetest_active) return;

    if ((HAL_GetTick() - s->gatetest_start_ms) >= s->gatetest_timeout_ms) {
        MotorDriver_AllOff();
        s->gatetest_active = false;
        UartProtocol_Print("\r\n[OK] Gate test done (outputs off)");
    }
}
