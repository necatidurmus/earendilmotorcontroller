#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hall map operations */
bool Storage_SaveHallMap(const uint8_t map[8]);
bool Storage_LoadHallMap(uint8_t map[8]);

/* Config operations */
bool Storage_SaveConfig(uint8_t kickDuty, uint16_t kickMs,
                        uint8_t rampStep, uint16_t rampIntervalMs,
                        uint8_t defaultPwm, uint16_t brakeHoldMs);
bool Storage_LoadConfig(uint8_t *kickDuty, uint16_t *kickMs,
                        uint8_t *rampStep, uint16_t *rampIntervalMs,
                        uint8_t *defaultPwm, uint16_t *brakeHoldMs);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
