#ifndef SERVICE_LOCK_H
#define SERVICE_LOCK_H

#include <stdbool.h>
#include <stdint.h>

/* ── Service lock constants ─────────────────────────────────────────────────
 *  F446 bridge compatibility: same timeout and token string. */
#define SERVICE_TIMEOUT_MS  30000u
#define SERVICE_TOKEN       "current_limited_bench_supply"  /* lowercased for compare */

/* ── Status query result ─────────────────────────────────────────────────── */
typedef struct {
    bool     unlocked;
    uint32_t remaining_ms;   /* 0 if locked */
    uint32_t blocked_count;  /* lifetime counter */
} ServiceLockStatus_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

void   ServiceLock_Init(void);
bool   ServiceLock_TryUnlock(const char *token);
void   ServiceLock_Lock(void);
bool   ServiceLock_IsUnlocked(void);
void   ServiceLock_Update(void);          /* auto-expire check (call from main loop) */
void   ServiceLock_GetStatus(ServiceLockStatus_t *out);

/* Check if a command requires service unlock.
 * If requireUnlock=true and service is locked:
 *   - blocked_count is incremented
 *   - returns false (command blocked)
 * If requireUnlock=true and service is unlocked:
 *   - returns true (command allowed)
 * If requireUnlock=false:
 *   - always returns true */
bool   ServiceLock_CheckCommand(const char *cmd, bool requireUnlock);

/* Dangerous command classification.
 * Returns true if the command is in the dangerous list
 * (identify, gatetest, scan, test, save/load, base, boost, pi,
 *  kickduty, kickms, ramp, ramprate, rampms, defpwm, map mutating). */
bool   ServiceLock_IsDangerousCmd(const char *cmd);

#endif /* SERVICE_LOCK_H */
