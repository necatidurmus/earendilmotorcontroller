/*
 * bldc_commutation.c — 6-step trapezoidal commutation implementation
 *
 * Commutation table (asynchronous drive):
 *
 *   State | Low-side ON | High-side PWM | Floating
 *   ------|-------------|---------------|--------
 *     0   |    AL       |     BH        |   C
 *     1   |    AL       |     CH        |   B
 *     2   |    BL       |     CH        |   A
 *     3   |    BL       |     AH        |   C
 *     4   |    CL       |     AH        |   B
 *     5   |    CL       |     BH        |   A
 *
 * Dead-time handling:
 *   On state change, we set all outputs to 0 (off) for one ISR period
 *   before applying the new pattern. This is NON-BLOCKING — the ISR sets
 *   a flag, and the actual pattern is applied on the NEXT ISR tick.
 *
 *   This is safer than the old code which used delayMicroseconds(3) inside ISR.
 *   With 12.5 kHz tick (80 us period), the all-off gap is 80 us which is
 *   more than enough dead-time for any MOSFET/gate-driver combination.
 */

#include "bldc_commutation.h"
#include "board_io.h"
#include "motor_config.h"

/* Active state tracking */
static uint8_t  activeState = 255;
static uint16_t activeDuty = 0;

/* Dead-time state machine */
static bool     deadtimePending = false;
static uint8_t  pendingState = 255;
static uint16_t pendingDuty = 0;

/*
 * Build phase pattern for a given commutation state and PWM duty.
 * Duty is already in PWM_PERIOD_COUNTS range (0..3332).
 */
static PhasePattern buildPattern(uint8_t state, uint16_t duty) {
    PhasePattern p = {0, 0, 0, false, false, false};

    switch (state) {
        case 0: /* AL low, BH PWM, C float */
            p.bh_pwm = duty;
            p.al_on = true;
            break;
        case 1: /* AL low, CH PWM, B float */
            p.ch_pwm = duty;
            p.al_on = true;
            break;
        case 2: /* BL low, CH PWM, A float */
            p.ch_pwm = duty;
            p.bl_on = true;
            break;
        case 3: /* BL low, AH PWM, C float */
            p.ah_pwm = duty;
            p.bl_on = true;
            break;
        case 4: /* CL low, AH PWM, B float */
            p.ah_pwm = duty;
            p.cl_on = true;
            break;
        case 5: /* CL low, BH PWM, A float */
            p.bh_pwm = duty;
            p.cl_on = true;
            break;
        default:
            /* Invalid state — all off */
            break;
    }

    return p;
}

static void applyPattern(const PhasePattern *p) {
    BoardIO_SetAllPWM(p->ah_pwm, p->bh_pwm, p->ch_pwm);
    BoardIO_SetLowSide(p->al_on, p->bl_on, p->cl_on);
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void Comm_Init(void) {
    activeState = 255;
    activeDuty = 0;
    deadtimePending = false;
    pendingState = 255;
    pendingDuty = 0;
    BoardIO_AllOff();
}

void Comm_Apply(uint8_t state, uint16_t duty_pwm, bool stateChanged) {
    /*
     * Two-stage dead-time:
     *   If deadtime is pending from last tick, apply the pending pattern now.
     *   If this is a state change, enter dead-time (all off) and defer.
     *   If no state change (only duty change), apply directly.
     */

    if (deadtimePending) {
        /* Previous tick was dead-time — apply the deferred pattern */
        PhasePattern pattern = buildPattern(pendingState, pendingDuty);
        applyPattern(&pattern);
        activeState = pendingState;
        activeDuty = pendingDuty;
        deadtimePending = false;
        return;
    }

    if (state > 5) {
        /* Invalid state — outputs off */
        BoardIO_AllOff();
        activeState = 255;
        activeDuty = 0;
        deadtimePending = false;
        return;
    }

    if (stateChanged && activeState <= 5) {
        /* State transition — enter dead-time for one tick */
        BoardIO_AllOff();
        pendingState = state;
        pendingDuty = duty_pwm;
        deadtimePending = true;
        return;
    }

    /* No state change — apply directly (duty update or first commutation) */
    PhasePattern pattern = buildPattern(state, duty_pwm);
    applyPattern(&pattern);
    activeState = state;
    activeDuty = duty_pwm;
}

void Comm_AllOff(void) {
    BoardIO_AllOff();
    activeState = 255;
    activeDuty = 0;
    deadtimePending = false;
}

uint8_t Comm_GetActiveState(void) {
    return activeState;
}

uint16_t Comm_GetActiveDuty(void) {
    return activeDuty;
}
