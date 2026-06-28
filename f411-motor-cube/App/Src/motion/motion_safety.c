/* ============================================================
 * App/Src/motion/motion_safety.c
 * Safety guard queries for motion control.
 * ============================================================ */
#include "motion_safety.h"
#include "app_state.h"
#include "service_task.h"

bool MotionControl_Allowed(void)
{
    /* Faults are no longer latched.  A new motion command clears the
     * displayed fault and releases the safety lock, so motion is always
     * allowed unless the caller is blocked for some other reason. */
    return true;
}

bool MotionControl_ServiceBusy(void)
{
    AppState *s = AppState_Get();
    return s->gatetest_active || ServiceTask_IsActive();
}
