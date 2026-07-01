/* ============================================================
 * App/Inc/storage/config_snapshot.h
 * Config snapshot helpers: capture runtime state to PersistentConfig_t,
 * apply PersistentConfig_t to runtime, and validate.
 * ============================================================ */
#ifndef CONFIG_SNAPSHOT_H
#define CONFIG_SNAPSHOT_H

#include "storage.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Capture current runtime state into a PersistentConfig_t struct. */
void ConfigSnapshot_FromRuntime(PersistentConfig_t *cfg);

/* Apply a PersistentConfig_t to the runtime (SpeedPI, AppState, Telemetry). */
void ConfigSnapshot_ApplyToRuntime(const PersistentConfig_t *cfg);

/* Validate config values are within acceptable limits. Returns true if ok. */
bool ConfigSnapshot_Validate(const PersistentConfig_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_SNAPSHOT_H */
