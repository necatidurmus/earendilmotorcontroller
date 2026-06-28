/* ============================================================
 * App/Inc/app_utils.h
 * String helpers and parsing utilities.
 * ============================================================ */
#ifndef APP_UTILS_H
#define APP_UTILS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Safe absolute value for int32_t — avoids undefined behavior for
 * INT32_MIN.  Standard abs(INT32_MIN) is UB in C; casting to unsigned
 * first and then negating is well-defined because unsigned arithmetic
 * wraps.  Returns unsigned so the caller must cast if signed is needed. */
#define SAFE_ABS(x) ((x) < 0 ? -(unsigned)(x) : (unsigned)(x))

void AppUtils_TrimInPlace(char *s);
void AppUtils_LowerInPlace(char *s);
bool AppUtils_StartsWith(const char *s, const char *prefix);
long AppUtils_ParseLongAfter(const char *s, const char *prefix, bool *ok);
float AppUtils_ParseFloatAfter(const char *s, const char *prefix, bool *ok);

#ifdef __cplusplus
}
#endif

#endif /* APP_UTILS_H */
