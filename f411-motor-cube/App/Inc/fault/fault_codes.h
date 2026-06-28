/* ============================================================
 * App/Inc/fault/fault_codes.h
 * Fault code enumeration — shared across fault, motion, command.
 * ============================================================ */
#ifndef FAULT_CODES_H
#define FAULT_CODES_H

typedef enum {
    FAULT_NONE = 0,
    FAULT_NO_HALL = 1,
    FAULT_INVALID_HALL = 2,
    FAULT_ILLEGAL_TRANSITION = 3,
    FAULT_HOST_LOST = 4,
    FAULT_WATCHDOG = 5,
    FAULT_HW_BREAK = 6,
    FAULT_ESTOP = 7,
    FAULT_OVERCURRENT = 8,
    FAULT_OVERVOLTAGE = 9,
    FAULT_UNDERVOLTAGE = 10,
    FAULT_OVERTEMP = 11,
    FAULT_GATE_DRIVER = 12,
    FAULT_UART_RX_OVERFLOW = 13
} FaultCode;

#endif /* FAULT_CODES_H */
