/* ============================================================
 * App/Src/service/service_identify.c
 * Identify algorithm: toggle sectors, read Hall, build map.
 * ============================================================ */
#include "service_identify.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "uart_protocol.h"
#include "app_main.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Tunables ---- */
#define IDENTIFY_STEP_TOGGLES    50U
#define IDENTIFY_DUTY            156U   /* ~4% duty: 10 in old 255 range → 10*4000/255 */
#define IDENTIFY_TOGGLE_MS       5U
#define IDENTIFY_SETTLE_A_MS     12U
#define IDENTIFY_SETTLE_B_MS     6U

/* ---- Internal phases ---- */
typedef enum {
    IDLE = 0,
    TOGGLE,
    SETTLE_A,
    SETTLE_B
} IdentifyPhase;

/* ---- Internal state ---- */
static struct {
    uint8_t        step;
    IdentifyPhase  phase;
    uint16_t       toggle_count;
    bool           toggle_flip;
    uint8_t        hall_a;
    uint8_t        candidate_map[8];
    uint8_t        unstable_count;
    uint32_t       next_action_ms;
} s_id;

/* ---- Public API ---- */

void ServiceIdentify_Start(void)
{
    memset(&s_id, 0, sizeof(s_id));
    s_id.phase = TOGGLE;
    for (uint8_t i = 0; i < 8; i++) s_id.candidate_map[i] = 255;
    UartProtocol_Print("\r\n[INFO] Identify start");
}

void ServiceIdentify_Cancel(void)
{
    App_SetIdentifyResult(5U); /* ABORTED */
    MotorDriver_AllOff();
    memset(&s_id, 0, sizeof(s_id));
}

bool ServiceIdentify_Update(void)
{
    uint32_t now = HAL_GetTick();

    if (s_id.step >= 6) {
        MotorDriver_AllOff();

        /* Print candidate map for user inspection */
        UartProtocol_Print("\r\n[IDENTIFY] candidate map:");
        for (uint8_t i = 0; i < 8; i++)
            UartProtocol_Printf(" %u:%u", (unsigned)i, (unsigned)s_id.candidate_map[i]);

        if (s_id.unstable_count > 0U) {
            UartProtocol_Printf("\r\n[IDENTIFY] REJECTED: %u unstable Hall step(s)",
                                (unsigned)s_id.unstable_count);
            UartProtocol_Print("\r\n[ERR] Identify failed — unstable Hall readings");
            UartProtocol_Print("\r\n[SAFE] Existing RAM hall map unchanged");
            App_SetIdentifyResult(5U); /* ABORTED */
            memset(&s_id, 0, sizeof(s_id));
            return false;
        }

        /* Validate the candidate map with full checks */
        char reason[32];
        if (Commutation_ValidateHallMapVerbose(s_id.candidate_map,
                                               reason, sizeof(reason))) {
            /* Valid — apply atomically to active map */
            Commutation_ApplyMap(s_id.candidate_map);
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

        memset(&s_id, 0, sizeof(s_id));
        return false;
    }

    switch (s_id.phase) {
    case TOGGLE: {
        if ((int32_t)(now - s_id.next_action_ms) < 0) return true;
        s_id.next_action_ms = now + IDENTIFY_TOGGLE_MS;

        uint8_t a = s_id.step;
        uint8_t b = (uint8_t)((a + 1U) % 6U);

        if (!s_id.toggle_flip)
            MotorDriver_ApplyStep(a, +1, IDENTIFY_DUTY);
        else
            MotorDriver_ApplyStep(b, +1, IDENTIFY_DUTY);

        s_id.toggle_flip = !s_id.toggle_flip;
        s_id.toggle_count++;

        if (s_id.toggle_count >= IDENTIFY_STEP_TOGGLES) {
            MotorDriver_AllOff();
            s_id.phase = SETTLE_A;
            s_id.next_action_ms = now + IDENTIFY_SETTLE_A_MS;
        }
        break;
    }

    case SETTLE_A:
        if ((int32_t)(now - s_id.next_action_ms) < 0) return true;
        s_id.hall_a = HallSensor_GetStableRaw();
        s_id.phase = SETTLE_B;
        s_id.next_action_ms = now + IDENTIFY_SETTLE_B_MS;
        break;

    case SETTLE_B: {
        if ((int32_t)(now - s_id.next_action_ms) < 0) return true;
        uint8_t hall = HallSensor_GetStableRaw();
        uint8_t mapped_state = (uint8_t)((s_id.step + 2U) % 6U);
        bool step_ok = true;

        /* Reject unstable Hall: two samples disagree */
        if (hall != s_id.hall_a) {
            UartProtocol_Printf("\r\n[ID] step=%u UNSTABLE hallA=%u hallB=%u (REJECTED)",
                                (unsigned)s_id.step,
                                (unsigned)s_id.hall_a,
                                (unsigned)hall);
            s_id.unstable_count++;
            step_ok = false;
        }

        /* Reject invalid raw Hall codes (0b000=0, 0b111=7) */
        if (hall == 0U || hall == 7U) {
            UartProtocol_Printf("\r\n[ID] step=%u INVALID raw=%u (REJECTED)",
                                (unsigned)s_id.step, (unsigned)hall);
            step_ok = false;
        }

        if (step_ok && hall < 8U) {
            if (s_id.candidate_map[hall] != 255U &&
                s_id.candidate_map[hall] != mapped_state) {
                UartProtocol_Printf("\r\n[ID] CONFLICT raw=%u was=%u now=%u",
                    (unsigned)hall,
                    (unsigned)s_id.candidate_map[hall],
                    (unsigned)mapped_state);
            }
            s_id.candidate_map[hall] = mapped_state;
            UartProtocol_Printf("\r\n[ID] step=%u hall=%u -> state=%u",
                                (unsigned)s_id.step,
                                (unsigned)hall,
                                (unsigned)mapped_state);
        } else if (!step_ok) {
            UartProtocol_Printf("\r\n[ID] step=%u SKIPPED (unstable/invalid)",
                                (unsigned)s_id.step);
        }

        s_id.step++;
        s_id.phase = TOGGLE;
        s_id.toggle_count = 0;
        s_id.toggle_flip = false;
        s_id.next_action_ms = now + 1U;
        break;
    }

    case IDLE:
    default:
        break;
    }

    return true;
}
