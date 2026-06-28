/* ============================================================
 * App/Inc/gate_test.h
 * Gate test command handler and timeout logic.
 * ============================================================ */
#ifndef GATE_TEST_H
#define GATE_TEST_H

#include <stdint.h>
#include <stdbool.h>

void GateTest_Init(void);
bool GateTest_IsActive(void);
void GateTest_Service(void);

#endif /* GATE_TEST_H */
