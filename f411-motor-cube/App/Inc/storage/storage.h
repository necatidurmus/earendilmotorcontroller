/* ============================================================
 * App/Inc/storage/storage.h
 * Flash storage for STM32F411 (Hall map + persistent config).
 *
 * SECTOR LAYOUT (last 128 KB sector at 0x08060000):
 *   Offset 0x0000: Hall map record (14 bytes, HMAP v1)
 *   Offset 0x0100: Config record area (append-only CFG2 slots,
 *                  see CFG_AREA_OFFSET in storage.c)
 *                   Append-only CFG2 records with sequence numbers.
 *                   On load, the latest valid record wins.
 *                   When the area is full, the sector is erased
 *                   and a fresh record is written at offset 0x0100.
 *
 * Safety:
 *   - Motor must be STOPPED before any erase/program.
 *   - No 128 KB stack buffer; records are small (~80 bytes).
 *   - Hall map at offset 0 is preserved during config erase
 *     by only erasing when the config area is full, and then
 *     immediately rewriting the hall map if it exists.
 *   - CRC32 validation on every record.
 * ============================================================ */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Hall map operations (unchanged API) ---- */
bool Storage_SaveHallMap(const uint8_t map[8]);
bool Storage_LoadHallMap(uint8_t map[8]);

/* ---- Persistent config ---- */

#define STORAGE_BASE_PWM_COUNT   8U
#define STORAGE_BOOST_PWM_COUNT  8U

typedef struct {
    float    kp;
    float    ki;
    uint16_t base_pwm[STORAGE_BASE_PWM_COUNT];
    uint16_t boost_pwm[STORAGE_BOOST_PWM_COUNT];
    uint16_t boost_ms;
    float    ramp_up;
    float    ramp_down;
    bool     kick_enabled;
    bool     ramp_enabled;
    uint16_t kick_duty;
    uint16_t kick_ms;
    uint16_t ramp_step;
    uint16_t ramp_interval_ms;
    uint16_t default_pwm;
    uint16_t brake_hold_ms;
    uint32_t telemetry_interval_ms;
} PersistentConfig_t;

/* Save config to flash. Motor must be stopped. Returns true on success. */
bool Storage_SaveConfig(const PersistentConfig_t *cfg);

/* Load latest valid config from flash. Returns true if found. */
bool Storage_LoadConfig(PersistentConfig_t *cfg);

/* Erase all config records from flash. Preserves hall map if present.
 * Motor must be stopped. */
bool Storage_EraseConfig(void);

/* Returns true if at least one valid config record exists in flash. */
bool Storage_HasValidConfig(void);

/* Returns the sequence number of the latest valid config record.
 * Returns 0 if no valid config exists. */
uint32_t Storage_GetConfigSequence(void);

/* ---- Legacy API (backward-compatible wrappers) ---- */
bool Storage_SaveConfigLegacy(uint16_t kickDuty, uint16_t kickMs,
                              uint16_t rampStep, uint16_t rampIntervalMs,
                              uint16_t defaultPwm, uint16_t brakeHoldMs);
bool Storage_LoadConfigLegacy(uint16_t *kickDuty, uint16_t *kickMs,
                              uint16_t *rampStep, uint16_t *rampIntervalMs,
                              uint16_t *defaultPwm, uint16_t *brakeHoldMs);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
