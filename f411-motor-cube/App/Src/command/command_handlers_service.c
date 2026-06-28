/* ============================================================
 * App/Src/command/command_handlers_service.c
 * Service command handlers: arm, disarm, identify, scan,
 * test, gatetest.
 * ============================================================ */
#include "command_handlers_service.h"
#include "app_state.h"
#include "app_config.h"
#include "app_utils.h"
#include "service_task.h"
#include "gate_test.h"
#include "motor_driver.h"
#include "motion_control.h"
#include "fault_manager.h"
#include "uart_protocol.h"
#include "stm32f4xx_hal.h"

#include "stm32f4xx_hal_gpio.h"
#include <string.h>
#include <stdlib.h>

bool CommandHandlers_Service_Handle(char *cmd)
{
    AppState *s = AppState_Get();

    /* --- identify --- */
    /* DEBUG: arming check temporarily removed for bring-up */
    if (strcmp(cmd, "identify") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        FaultManager_Clear();
        MotorDriver_SetSafetyLock(false);
        MotorDriver_AllOff();
        MotionControl_StopImmediate();
        s->identify_was_run = true;
        s->identify_last_result = 0U;
        ServiceTask_Request(SVC_IDENTIFY);
        return true;
    }

    /* --- scan --- */
    if (strcmp(cmd, "scan") == 0) {
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT && s->phase != PHASE_BRAKE) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        ServiceTask_Request(SVC_SCAN);
        return true;
    }

    /* --- test --- */
    if (strcmp(cmd, "test") == 0) {
        if (!s->service_armed) {
            UartProtocol_Print("\r\n[ERR] Service not armed. Use: arm service CURRENT_LIMITED_BENCH_SUPPLY");
            return true;
        }
        if (MotionControl_ServiceBusy()) { UartProtocol_Print("\r\n[ERR] BUSY_SERVICE"); return true; }
        if (s->phase != PHASE_STOPPED && s->phase != PHASE_FAULT && s->phase != PHASE_BRAKE) {
            UartProtocol_Print("\r\n[ERR] Stop motor first");
            return true;
        }
        ServiceTask_Request(SVC_TEST);
        return true;
    }

    /* --- gpiotest --- */
    /* DEBUG: Full diagnostic. Tests GPIO + TIM1 registers directly. */
    if (strcmp(cmd, "gpiotest") == 0) {
        /* 1. Read and print GPIOA MODER to verify PA7/PA8/PA9 mode */
        uint32_t moder = GPIOA->MODER;
        uint32_t pa7_mode = (moder >> (7 * 2)) & 0x3;
        uint32_t pa8_mode = (moder >> (8 * 2)) & 0x3;
        uint32_t pa9_mode = (moder >> (9 * 2)) & 0x3;
        UartProtocol_Printf("\r\n[DEBUG] PA7_mode=%lu PA8_mode=%lu PA9_mode=%lu (0=IN,1=OUT,2=AF,3=AN)",
                            (unsigned long)pa7_mode, (unsigned long)pa8_mode, (unsigned long)pa9_mode);

        /* 2. Read TIM1 key registers */
        UartProtocol_Printf("\r\n[DEBUG] BDTR=0x%08lX CCER=0x%08lX CCMR1=0x%08lX",
                            (unsigned long)TIM1->BDTR, (unsigned long)TIM1->CCER, (unsigned long)TIM1->CCMR1);
        UartProtocol_Printf("\r\n[DEBUG] CCR1=%lu CCR2=%lu ARR=%lu",
                            (unsigned long)TIM1->CCR1, (unsigned long)TIM1->CCR2, (unsigned long)TIM1->ARR);

        /* 3. Now replicate exactly what MotorDriver_ApplyStep(0, +1, 156) does
         *    for sector 0: HIGH=CH2(PA9), LOW=PA7(GPIO) */
        uint32_t period = (uint32_t)TIM1->ARR + 1U;
        uint16_t ccr_val = (uint16_t)((156U * period) / PWM_MAX_DUTY);

        /* All off first */
        TIM1->CCER = 0;
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);

        /* Set OC2M = PWM mode 1, OC2PE = 1 */
        uint32_t ccmr1 = TIM1->CCMR1;
        ccmr1 &= ~TIM_CCMR1_OC2M;
        ccmr1 |= (6U << 12);   /* PWM mode 1 for CH2 */
        ccmr1 |= TIM_CCMR1_OC2PE;
        TIM1->CCMR1 = ccmr1;
        TIM1->CCR2 = ccr_val;

        /* Enable CH2 high-side */
        TIM1->CCER = TIM_CCER_CC2E;

        /* Ensure MOE */
        TIM1->BDTR |= TIM_BDTR_MOE;

        /* LOW-side: PA7 GPIO HIGH */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_SET);

        UartProtocol_Printf("\r\n[OK] DIRECT TEST: PA9(PWM=%lu) + PA7(GPIO HIGH) for 3s", (unsigned long)ccr_val);
        HAL_Delay(3000);

        /* Restore */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_7, GPIO_PIN_RESET);
        TIM1->CCER = 0;
        UartProtocol_Print("\r\n[OK] Direct test done");
        return true;
    }

    /* --- pinetest <high_pin> <low_pin> <duty> --- */
    /* DEBUG: Direct GPIO+PWM test. HIGH pin = TIM1 PWM, LOW pin = GPIO HIGH.
     * Pins: 8=PA8(AH), 9=PA9(BH), 10=PA10(CH), 7=PA7(AL), 0=PB0(BL), 1=PB1(CL)
     * Example: "pinetest 8 7 50" = PA8 PWM + PA7 HIGH */
    if (AppUtils_StartsWith(cmd, "pinetest ")) {
        FaultManager_Clear();
        MotorDriver_SetSafetyLock(false);
        MotorDriver_AllOff();
        MotionControl_StopImmediate();
        ServiceTask_Cancel();

        char *pp = cmd + 9;
        while (*pp == ' ') pp++;
        long high_pin = strtol(pp, &pp, 10);
        while (*pp == ' ') pp++;
        long low_pin = strtol(pp, &pp, 10);
        while (*pp == ' ') pp++;
        long duty_val = strtol(pp, NULL, 10);

        if (duty_val < 1 || duty_val > PWM_MAX_DUTY) {
            UartProtocol_Print("\r\n[ERR] Usage: pinetest <high_pin> <low_pin> <1-4000>");
            return true;
        }

        /* Map high_pin to TIM1 channel and GPIO */
        volatile uint32_t *ccr = NULL;
        uint32_t ccxe = 0;
        uint32_t ocxm_shift = 0;
        GPIO_TypeDef *high_port = NULL;
        uint16_t high_gpio = 0;

        if (high_pin == 8)       { ccr = &TIM1->CCR1; ccxe = TIM_CCER_CC1E; ocxm_shift = 4;  high_port = GPIOA; high_gpio = GPIO_PIN_8; }
        else if (high_pin == 9)  { ccr = &TIM1->CCR2; ccxe = TIM_CCER_CC2E; ocxm_shift = 12; high_port = GPIOA; high_gpio = GPIO_PIN_9; }
        else if (high_pin == 10) { ccr = &TIM1->CCR3; ccxe = TIM_CCER_CC3E; ocxm_shift = 4;  high_port = GPIOA; high_gpio = GPIO_PIN_10; }
        else { UartProtocol_Print("\r\n[ERR] high_pin: 8,9,10"); return true; }

        /* Map low_pin to GPIO */
        GPIO_TypeDef *low_port = NULL;
        uint16_t low_gpio = 0;
        if (low_pin == 7)       { low_port = GPIOA; low_gpio = GPIO_PIN_7; }
        else if (low_pin == 0)  { low_port = GPIOB; low_gpio = GPIO_PIN_0; }
        else if (low_pin == 1)  { low_port = GPIOB; low_gpio = GPIO_PIN_1; }
        else { UartProtocol_Print("\r\n[ERR] low_pin: 7,0,1"); return true; }

        /* Step 1: Reconfigure LOW pin as GPIO OUTPUT (bypass TIM1 AF) */
        GPIO_InitTypeDef gpio = {0};
        gpio.Pin   = low_gpio;
        gpio.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio.Pull  = GPIO_NOPULL;
        gpio.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(low_port, &gpio);
        HAL_GPIO_WritePin(low_port, low_gpio, GPIO_PIN_RESET);

        /* Step 2: Configure TIM1 for single-channel PWM */
        uint32_t period = (uint32_t)TIM1->ARR + 1U;
        uint16_t ccr_val = (uint16_t)(((uint32_t)duty_val * period) / PWM_MAX_DUTY);
        *ccr = ccr_val;

        /* Disable all channels first */
        uint32_t ccer = TIM1->CCER;
        ccer &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | TIM_CCER_CC3E | TIM_CCER_CC3NE);
        TIM1->CCER = ccer;

        /* Set OCxM = PWM mode 1, OCxPE = 1 for the selected channel */
        volatile uint32_t *ccmr = (high_pin <= 9) ? &TIM1->CCMR1 : &TIM1->CCMR2;
        uint32_t msk = (high_pin == 9) ? TIM_CCMR1_OC2M : ((high_pin <= 9) ? TIM_CCMR1_OC1M : TIM_CCMR2_OC3M);
        uint32_t pe  = (high_pin == 9) ? TIM_CCMR1_OC2PE : ((high_pin <= 9) ? TIM_CCMR1_OC1PE : TIM_CCMR2_OC3PE);
        uint32_t v = *ccmr;
        v &= ~msk;
        v |= (6U << ocxm_shift);
        v |= pe;
        *ccmr = v;

        /* Enable HIGH-side only */
        ccer = TIM1->CCER;
        ccer |= ccxe;
        TIM1->CCER = ccer;

        /* Ensure MOE */
        TIM1->BDTR |= TIM_BDTR_MOE;

        /* Step 3: LOW-side ON */
        HAL_GPIO_WritePin(low_port, low_gpio, GPIO_PIN_SET);

        UartProtocol_Printf("\r\n[OK] PINETEST: PA%ld(PWM duty=%lu) + P%c%ld(GPIO HIGH) for 3s",
                            high_pin, (unsigned long)duty_val,
                            (low_port == GPIOA) ? 'A' : 'B', low_pin);

        /* 3-second blocking delay for observation */
        HAL_Delay(3000);

        /* Step 4: Restore */
        HAL_GPIO_WritePin(low_port, low_gpio, GPIO_PIN_RESET);
        MotorDriver_AllOff();

        /* Restore AF1 for the low pin */
        gpio.Pin       = low_gpio;
        gpio.Mode      = GPIO_MODE_AF_PP;
        gpio.Pull      = GPIO_PULLDOWN;
        gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
        gpio.Alternate = GPIO_AF1_TIM1;
        HAL_GPIO_Init(low_port, &gpio);

        UartProtocol_Print("\r\n[OK] PINETEST done, pins restored");
        return true;
    }

    /* --- gatetest <sector> <duty> --- */
    /* DEBUG: all safety/arming checks removed for bring-up */
    if (AppUtils_StartsWith(cmd, "gatetest ")) {
        FaultManager_Clear();
        MotorDriver_SetSafetyLock(false);
        MotorDriver_AllOff();
        MotionControl_StopImmediate();
        ServiceTask_Cancel();
        const char *p = cmd + 9;
        while (*p == ' ') p++;
        char *ge1 = NULL;
        long sector = strtol(p, &ge1, 10);
        if (ge1 == p) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>"); return true; }
        while (*ge1 == ' ') ge1++;
        char *ge2 = NULL;
        long duty = strtol(ge1, &ge2, 10);
        if (ge2 == ge1) { UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>"); return true; }
        if (sector < 0 || sector > 5 || duty < 1 || duty > PWM_MAX_DUTY) {
            UartProtocol_Print("\r\n[ERR] Usage: gatetest <0-5> <1-4000>");
            return true;
        }
        s->gatetest_active   = true;
        s->gatetest_sector   = (uint8_t)sector;
        s->gatetest_duty     = (uint16_t)duty;
        s->gatetest_start_ms = HAL_GetTick();
        s->gatetest_timeout_ms = 2000U;  /* 2s for manual observation */
        MotorDriver_ApplyStep((uint8_t)sector, +1, (uint16_t)duty);
        UartProtocol_Printf("\r\n[OK] Gate test sector=%lu duty=%lu timeout=2s",
                            (unsigned long)sector, (unsigned long)duty);
        return true;
    }

    /* --- arm gatetest --- */
    if (AppUtils_StartsWith(cmd, "arm gatetest ")) {
        const char *token = cmd + 13;
        while (*token == ' ') token++;
        if (strcmp(token, "motor_disconnected_i_understand") == 0) {
            s->gate_test_armed = true;
            s->gate_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Gate test armed for 30s. Motor must be disconnected!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND");
        }
        return true;
    }

    /* --- arm service --- */
    if (AppUtils_StartsWith(cmd, "arm service ")) {
        const char *token = cmd + 12;
        while (*token == ' ') token++;
        if (strcmp(token, "current_limited_bench_supply") == 0) {
            s->service_armed = true;
            s->service_arm_start_ms = HAL_GetTick();
            UartProtocol_Print("\r\n[OK] Service armed for 30s. Use current-limited PSU!");
        } else {
            UartProtocol_Print("\r\n[ERR] Usage: arm service CURRENT_LIMITED_BENCH_SUPPLY");
        }
        return true;
    }

    /* --- disarm --- */
    if (strcmp(cmd, "disarm gatetest") == 0) {
        s->gate_test_armed = false;
        UartProtocol_Print("\r\n[OK] Gate test disarmed");
        return true;
    }
    if (strcmp(cmd, "disarm service") == 0) {
        s->service_armed = false;
        UartProtocol_Print("\r\n[OK] Service disarmed");
        return true;
    }

    return false;
}
