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
#include "app_main.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Tunables (matching old Arduino firmware) ---- */
#define SCAN_DURATION_MS         10000U
#define SCAN_POLL_MS             5U

#define TEST_DUTY                10U
#define TEST_STEP_MS             300U
#define TEST_REPORT_MS           200U

#define IDENTIFY_STEP_TOGGLES    50U
#define IDENTIFY_DUTY            10U
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
    uint8_t        id_unstable_count;  /* steps with mismatched hallA/hallB */
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
    if (s_svc.task == SVC_IDENTIFY) {
        App_SetIdentifyResult(5U); /* ABORTED */
    }
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

        /* Print candidate map for user inspection */
        UartProtocol_Print("\r\n[IDENTIFY] candidate map:");
        for (uint8_t i = 0; i < 8; i++)
            UartProtocol_Printf(" %u:%u", (unsigned)i, (unsigned)s_svc.id_candidate_map[i]);

        if (s_svc.id_unstable_count > 0U) {
            UartProtocol_Printf("\r\n[IDENTIFY] REJECTED: %u unstable Hall step(s)",
                                (unsigned)s_svc.id_unstable_count);
            UartProtocol_Print("\r\n[ERR] Identify failed — unstable Hall readings");
            UartProtocol_Print("\r\n[SAFE] Existing RAM hall map unchanged");
            App_SetIdentifyResult(5U); /* ABORTED */
            finish();
            return;
        }

        /* Validate the candidate map with full checks */
        char reason[32];
        if (Commutation_ValidateHallMapVerbose(s_svc.id_candidate_map,
                                               reason, sizeof(reason))) {
            /* Valid — apply atomically to active map */
            Commutation_ApplyMap(s_svc.id_candidate_map);
            HallSensor_OnMapChanged();
            UartProtocol_Print("\r\n[IDENTIFY] validation: OK");
            UartProtocol_Print("\r\n[OK] Identify updated RAM hall map");
            UartProtocol_Print("\r\n[WARN] Map is RAM-only. Use 'map save' after verification if storage is enabled.");
            App_SetIdentifyResult(1U); /* OK */
        } else {
            /* Invalid — do NOT apply. Active map unchanged. */
            UartProtocol_Printf("\r\n[IDENTIFY] validation: FAILED (%s)", reason);
            UartProtocol_Print("\r\n[ERR] Identify map rejected");
            UartProtocol_Print("\r\n[SAFE] Existing RAM hall map unchanged");
            if (strcmp(reason, "duplicate_sector") == 0) {
                App_SetIdentifyResult(2U);
            } else if (strcmp(reason, "missing_sector") == 0) {
                App_SetIdentifyResult(3U);
            } else if (strcmp(reason, "sector_out_of_range") == 0 ||
                       strcmp(reason, "raw0_not_invalid") == 0 ||
                       strcmp(reason, "raw7_not_invalid") == 0) {
                App_SetIdentifyResult(4U);
            } else {
                App_SetIdentifyResult(2U); /* generic reject */
            }
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
        uint8_t mapped_state = (uint8_t)((s_svc.id_step + 2U) % 6U);
        bool step_ok = true;

        /* Reject unstable Hall: two samples disagree */
        if (hall != s_svc.id_hall_a) {
            UartProtocol_Printf("\r\n[ID] step=%u UNSTABLE hallA=%u hallB=%u (REJECTED)",
                                (unsigned)s_svc.id_step,
                                (unsigned)s_svc.id_hall_a,
                                (unsigned)hall);
            s_svc.id_unstable_count++;
            step_ok = false;
        }

        /* Reject invalid raw Hall codes (0b000=0, 0b111=7) */
        if (hall == 0U || hall == 7U) {
            UartProtocol_Printf("\r\n[ID] step=%u INVALID raw=%u (REJECTED)",
                                (unsigned)s_svc.id_step, (unsigned)hall);
            step_ok = false;
        }

        if (step_ok && hall < 8U) {
            /* Detect conflict: if this raw was already assigned a
             * different sector, log it. The validation at the end
             * will catch duplicates. */
            if (s_svc.id_candidate_map[hall] != 255U &&
                s_svc.id_candidate_map[hall] != mapped_state) {
                UartProtocol_Printf("\r\n[ID] CONFLICT raw=%u was=%u now=%u",
                    (unsigned)hall,
                    (unsigned)s_svc.id_candidate_map[hall],
                    (unsigned)mapped_state);
            }
            s_svc.id_candidate_map[hall] = mapped_state;
            UartProtocol_Printf("\r\n[ID] step=%u hall=%u -> state=%u",
                                (unsigned)s_svc.id_step,
                                (unsigned)hall,
                                (unsigned)mapped_state);
        } else if (!step_ok) {
            UartProtocol_Printf("\r\n[ID] step=%u SKIPPED (unstable/invalid)",
                                (unsigned)s_svc.id_step);
        }

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
        if (s_svc.task == SVC_IDENTIFY) {
            App_SetIdentifyResult(5U); /* ABORTED */
        }
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
