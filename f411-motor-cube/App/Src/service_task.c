/* ============================================================
 * App/Src/service_task.c
 * Non-blocking service tasks: scan, test, identify.
 * Ported from the old Arduino firmware.
 * ============================================================ */

#include "service_task.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "uart_protocol.h"
#include "fault_manager.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Tunables (matching old Arduino firmware) ---- */
#define SCAN_DURATION_MS         10000U
#define SCAN_POLL_MS             5U

#define TEST_DUTY                60U
#define TEST_STEP_MS             2000U
#define TEST_REPORT_MS           1500U

#define IDENTIFY_STEP_TOGGLES    100U
#define IDENTIFY_DUTY            35U
#define IDENTIFY_TOGGLE_MS       5U
#define IDENTIFY_SETTLE_A_MS     12U
#define IDENTIFY_SETTLE_B_MS     6U

/* ---- Internal state ---- */

typedef enum {
    IDLE = 0,
    TOGGLE,
    SETTLE_A,
    SETTLE_B
} IdentifyPhase;

static struct {
    ServiceTaskType task;
    uint32_t start_ms;
    uint32_t next_action_ms;

    /* scan */
    uint8_t scan_last_hall;

    /* test */
    uint8_t test_step;
    bool    test_active;
    bool    test_reported;
    uint32_t test_step_start_ms;

    /* identify */
    uint8_t        id_step;
    IdentifyPhase  id_phase;
    uint16_t       id_toggle_count;
    bool           id_toggle_flip;
    uint8_t        id_hall_a;
    uint8_t        id_candidate_map[8];
} s_svc;

/* ---- Public API ---- */

void ServiceTask_Init(void)
{
    memset(&s_svc, 0, sizeof(s_svc));
}

void ServiceTask_Request(ServiceTaskType task)
{
    memset(&s_svc, 0, sizeof(s_svc));
    s_svc.task = task;
    s_svc.start_ms = HAL_GetTick();
    s_svc.next_action_ms = s_svc.start_ms;

    if (task == SVC_SCAN) {
        s_svc.scan_last_hall = 255;
        UartProtocol_Print("\r\n[INFO] Scan start");
    } else if (task == SVC_TEST) {
        s_svc.test_step = 0;
        s_svc.test_active = false;
        s_svc.test_reported = false;
        UartProtocol_Print("\r\n[INFO] Test start");
    } else if (task == SVC_IDENTIFY) {
        s_svc.id_step = 0;
        s_svc.id_phase = TOGGLE;
        s_svc.id_toggle_count = 0;
        s_svc.id_toggle_flip = false;
        for (uint8_t i = 0; i < 8; i++) s_svc.id_candidate_map[i] = 255;
        UartProtocol_Print("\r\n[INFO] Identify start");
    }
}

void ServiceTask_Cancel(void)
{
    MotorDriver_AllOff();
    memset(&s_svc, 0, sizeof(s_svc));
}

bool ServiceTask_IsActive(void)
{
    return s_svc.task != SVC_NONE;
}

/* ---- Internal helpers ---- */

static void finish(void)
{
    MotorDriver_AllOff();
    memset(&s_svc, 0, sizeof(s_svc));
}

/* ---- Scan: monitor hall signals for 10 seconds ---- */

static void update_scan(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - s_svc.start_ms) >= SCAN_DURATION_MS) {
        UartProtocol_Print("\r\n[INFO] Scan done");
        finish();
        return;
    }

    if ((int32_t)(now - s_svc.next_action_ms) < 0) return;
    s_svc.next_action_ms = now + SCAN_POLL_MS;

    uint8_t h = HallSensor_GetStableRaw();
    if (h != s_svc.scan_last_hall) {
        s_svc.scan_last_hall = h;
        UartProtocol_Printf("\r\nHall=%u bin=%u%u%u",
                            (unsigned)h,
                            (unsigned)((h >> 2) & 1),
                            (unsigned)((h >> 1) & 1),
                            (unsigned)(h & 1));
    }
}

/* ---- Test: apply each of 6 sectors, read hall ---- */

static void update_test(void)
{
    uint32_t now = HAL_GetTick();

    if (s_svc.test_step >= 6) {
        UartProtocol_Print("\r\n[INFO] Test done");
        finish();
        return;
    }

    if (!s_svc.test_active) {
        s_svc.test_active = true;
        s_svc.test_reported = false;
        s_svc.test_step_start_ms = now;
        MotorDriver_ApplyStep(s_svc.test_step, +1, TEST_DUTY);
        return;
    }

    uint32_t elapsed = now - s_svc.test_step_start_ms;

    if (!s_svc.test_reported && elapsed >= TEST_REPORT_MS) {
        uint8_t hall = HallSensor_GetStableRaw();
        uint8_t mapped = Commutation_HallToState(hall);
        UartProtocol_Printf("\r\n[TEST] step=%u hall=%u mapped=%u",
                            (unsigned)s_svc.test_step,
                            (unsigned)hall,
                            (unsigned)mapped);
        s_svc.test_reported = true;
    }

    if (elapsed >= TEST_STEP_MS) {
        MotorDriver_AllOff();
        s_svc.test_step++;
        s_svc.test_active = false;
    }
}

/* ---- Identify: toggle sectors, read hall, build map ---- */

static void update_identify(void)
{
    uint32_t now = HAL_GetTick();

    if (s_svc.id_step >= 6) {
        MotorDriver_AllOff();

        if (Commutation_IsValidState(s_svc.id_candidate_map[1]) &&
            Commutation_IsValidState(s_svc.id_candidate_map[2]) &&
            Commutation_IsValidState(s_svc.id_candidate_map[3]) &&
            Commutation_IsValidState(s_svc.id_candidate_map[4]) &&
            Commutation_IsValidState(s_svc.id_candidate_map[5]) &&
            Commutation_IsValidState(s_svc.id_candidate_map[6])) {
            /* Apply candidate map to RAM */
            bool map_ok = true;
            for (uint8_t i = 0; i < 8; i++) {
                if (!Commutation_SetMapEntry(i, s_svc.id_candidate_map[i])) {
                    map_ok = false;
                    break;
                }
            }
            if (map_ok) {
                UartProtocol_Print("\r\n[OK] Identify updated RAM map");
            } else {
                UartProtocol_Print("\r\n[ERR] Identify: map entry rejected");
            }
        } else {
            UartProtocol_Print("\r\n[INFO] Candidate map: ");
            for (uint8_t i = 0; i < 8; i++)
                UartProtocol_Printf("%u ", (unsigned)s_svc.id_candidate_map[i]);
            UartProtocol_Print("\r\n[WARN] Identify produced invalid map");
        }

        finish();
        return;
    }

    switch (s_svc.id_phase) {
    case TOGGLE: {
        if ((int32_t)(now - s_svc.next_action_ms) < 0) return;
        s_svc.next_action_ms = now + IDENTIFY_TOGGLE_MS;

        uint8_t a = s_svc.id_step;
        uint8_t b = (uint8_t)((a + 1U) % 6U);

        if (!s_svc.id_toggle_flip)
            MotorDriver_ApplyStep(a, +1, IDENTIFY_DUTY);
        else
            MotorDriver_ApplyStep(b, +1, IDENTIFY_DUTY);

        s_svc.id_toggle_flip = !s_svc.id_toggle_flip;
        s_svc.id_toggle_count++;

        if (s_svc.id_toggle_count >= IDENTIFY_STEP_TOGGLES) {
            MotorDriver_AllOff();
            s_svc.id_phase = SETTLE_A;
            s_svc.next_action_ms = now + IDENTIFY_SETTLE_A_MS;
        }
        break;
    }

    case SETTLE_A:
        if ((int32_t)(now - s_svc.next_action_ms) < 0) return;
        s_svc.id_hall_a = HallSensor_GetStableRaw();
        s_svc.id_phase = SETTLE_B;
        s_svc.next_action_ms = now + IDENTIFY_SETTLE_B_MS;
        break;

    case SETTLE_B: {
        if ((int32_t)(now - s_svc.next_action_ms) < 0) return;
        uint8_t hall = HallSensor_GetStableRaw();

        if (hall != s_svc.id_hall_a) {
            UartProtocol_Printf("\r\n[ID] step=%u UNSTABLE hallA=%u hallB=%u",
                                (unsigned)s_svc.id_step,
                                (unsigned)s_svc.id_hall_a,
                                (unsigned)hall);
        }

        uint8_t mapped_state = (uint8_t)((s_svc.id_step + 2U) % 6U);
        if (hall < 8U)
            s_svc.id_candidate_map[hall] = mapped_state;

        UartProtocol_Printf("\r\n[ID] step=%u hall=%u -> state=%u",
                            (unsigned)s_svc.id_step,
                            (unsigned)hall,
                            (unsigned)mapped_state);

        s_svc.id_step++;
        s_svc.id_phase = TOGGLE;
        s_svc.id_toggle_count = 0;
        s_svc.id_toggle_flip = false;
        s_svc.next_action_ms = now + 1U;
        break;
    }

    case IDLE:
    default:
        break;
    }
}

/* ---- Main update (called from App_Loop) ---- */

void ServiceTask_Update(void)
{
    if (s_svc.task == SVC_NONE) return;

    /* Abort immediately if a fault has been latched.  This prevents
     * test/identify from continuing to drive the motor after a
     * fault (e.g. HW_BREAK, INVALID_HALL). */
    if (FaultManager_GetLast() != FAULT_NONE) {
        MotorDriver_AllOff();
        UartProtocol_Print("\r\n[WARN] Service task aborted (fault)");
        memset(&s_svc, 0, sizeof(s_svc));
        return;
    }

    switch (s_svc.task) {
    case SVC_SCAN:     update_scan();     break;
    case SVC_TEST:     update_test();     break;
    case SVC_IDENTIFY: update_identify(); break;
    default: break;
    }
}
