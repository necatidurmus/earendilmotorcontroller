/* ============================================================
 * App/Inc/service/service_commutation_test.h
 * Scan and commutation test algorithms.
 * ============================================================ */
#ifndef SERVICE_COMMUTATION_TEST_H
#define SERVICE_COMMUTATION_TEST_H

#include <stdbool.h>

/* Scan: monitor Hall signals for 10 seconds. */
void ServiceScan_Start(void);
/* Returns true while running, false when done. */
bool ServiceScan_Update(void);

/* Test: apply each of 6 sectors, read Hall. */
void ServiceTest_Start(void);
/* Returns true while running, false when done. */
bool ServiceTest_Update(void);

/* Common cancel (calls MotorDriver_AllOff). */
void ServiceCommutation_Cancel(void);

#endif /* SERVICE_COMMUTATION_TEST_H */
