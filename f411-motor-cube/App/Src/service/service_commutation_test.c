/* ============================================================
 * App/Src/service/service_commutation_test.c
 * Scan (monitor Hall for 10s) and test (apply 6 sectors).
 * ============================================================ */
#include "service_commutation_test.h"
#include "motor_driver.h"
#include "hall_sensor.h"
#include "bldc_commutation.h"
#include "uart_protocol.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Tunables ---- */
#define SCAN_DURATION_MS         10000U
#define SCAN_POLL_MS             5U

#define TEST_DUTY                156U   /* ~4% duty: 10 in old 255 range → 10*4000/255 */
#define TEST_STEP_MS             300U
#define TEST_REPORT_MS           200U

/* ---- Scan state ---- */
static struct {
    uint32_t start_ms;
    uint32_t next_action_ms;
    uint8_t  last_hall;
} s_scan;

/* ---- Test state ---- */
static struct {
    uint8_t  step;
    bool     active;
    bool     reported;
    uint32_t step_start_ms;
} s_test;

/* ---- Scan ---- */

void ServiceScan_Start(void)
{
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.start_ms = HAL_GetTick();
    s_scan.next_action_ms = s_scan.start_ms;
    s_scan.last_hall = 255;
    UartProtocol_Print("\r\n[INFO] Scan start");
}

bool ServiceScan_Update(void)
{
    uint32_t now = HAL_GetTick();

    if ((now - s_scan.start_ms) >= SCAN_DURATION_MS) {
        UartProtocol_Print("\r\n[INFO] Scan done");
        MotorDriver_AllOff();
        memset(&s_scan, 0, sizeof(s_scan));
        return false;
    }

    if ((int32_t)(now - s_scan.next_action_ms) < 0) return true;
    s_scan.next_action_ms = now + SCAN_POLL_MS;

    uint8_t h = HallSensor_GetStableRaw();
    if (h != s_scan.last_hall) {
        s_scan.last_hall = h;
        UartProtocol_Printf("\r\nHall=%u bin=%u%u%u",
                            (unsigned)h,
                            (unsigned)((h >> 2) & 1),
                            (unsigned)((h >> 1) & 1),
                            (unsigned)(h & 1));
    }
    return true;
}

/* ---- Test ---- */

void ServiceTest_Start(void)
{
    memset(&s_test, 0, sizeof(s_test));
    UartProtocol_Print("\r\n[INFO] Test start");
}

bool ServiceTest_Update(void)
{
    uint32_t now = HAL_GetTick();

    if (s_test.step >= 6) {
        UartProtocol_Print("\r\n[INFO] Test done");
        MotorDriver_AllOff();
        memset(&s_test, 0, sizeof(s_test));
        return false;
    }

    if (!s_test.active) {
        s_test.active = true;
        s_test.reported = false;
        s_test.step_start_ms = now;
        MotorDriver_ApplyStep(s_test.step, +1, TEST_DUTY);
        return true;
    }

    uint32_t elapsed = now - s_test.step_start_ms;

    if (!s_test.reported && elapsed >= TEST_REPORT_MS) {
        uint8_t hall = HallSensor_GetStableRaw();
        uint8_t mapped = Commutation_HallToState(hall);
        UartProtocol_Printf("\r\n[TEST] step=%u hall=%u mapped=%u",
                            (unsigned)s_test.step,
                            (unsigned)hall,
                            (unsigned)mapped);
        s_test.reported = true;
    }

    if (elapsed >= TEST_STEP_MS) {
        MotorDriver_AllOff();
        s_test.step++;
        s_test.active = false;
    }
    return true;
}

/* ---- Common cancel ---- */

void ServiceCommutation_Cancel(void)
{
    MotorDriver_AllOff();
    memset(&s_scan, 0, sizeof(s_scan));
    memset(&s_test, 0, sizeof(s_test));
}
