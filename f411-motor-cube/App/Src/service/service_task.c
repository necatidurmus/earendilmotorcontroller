/* ============================================================
 * App/Src/service/service_task.c
 * Non-blocking service task dispatcher.
 * Delegates to:
 *   service_identify.c       — identify algorithm
 *   service_commutation_test.c — scan and commutation test
 * ============================================================ */
#include "service_task.h"
#include "service_identify.h"
#include "service_commutation_test.h"
#include "motor_driver.h"
#include "uart_protocol.h"
#include "fault_manager.h"
#include "app_main.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Internal state ---- */

static ServiceTaskType s_active_task = SVC_NONE;

/* ---- Public API ---- */

void ServiceTask_Init(void)
{
    s_active_task = SVC_NONE;
}

void ServiceTask_Request(ServiceTaskType task)
{
    /* Cancel any running task first. */
    ServiceTask_Cancel();

    s_active_task = task;

    if (task == SVC_SCAN) {
        ServiceScan_Start();
    } else if (task == SVC_TEST) {
        ServiceTest_Start();
    } else if (task == SVC_IDENTIFY) {
        ServiceIdentify_Start();
    }
}

void ServiceTask_Cancel(void)
{
    if (s_active_task == SVC_IDENTIFY) {
        ServiceIdentify_Cancel();
    } else if (s_active_task == SVC_SCAN || s_active_task == SVC_TEST) {
        ServiceCommutation_Cancel();
    }
    MotorDriver_AllOff();
    s_active_task = SVC_NONE;
}

bool ServiceTask_IsActive(void)
{
    return s_active_task != SVC_NONE;
}

bool ServiceTask_IsDriving(void)
{
    return s_active_task == SVC_IDENTIFY || s_active_task == SVC_TEST;
}

/* ---- Main update (called from App_Loop) ---- */

void ServiceTask_Update(void)
{
    if (s_active_task == SVC_NONE) return;

    /* Abort immediately if a fault has been latched. */
    if (FaultManager_GetLast() != FAULT_NONE) {
        MotorDriver_AllOff();
        UartProtocol_Print("\r\n[WARN] Service task aborted (fault)");
        if (s_active_task == SVC_IDENTIFY) {
            App_SetIdentifyResult(5U); /* ABORTED */
        }
        s_active_task = SVC_NONE;
        return;
    }

    bool still_running = true;
    switch (s_active_task) {
    case SVC_SCAN:     still_running = ServiceScan_Update();     break;
    case SVC_TEST:     still_running = ServiceTest_Update();     break;
    case SVC_IDENTIFY: still_running = ServiceIdentify_Update(); break;
    default: break;
    }

    if (!still_running) {
        MotorDriver_AllOff();
        s_active_task = SVC_NONE;
    }
}
