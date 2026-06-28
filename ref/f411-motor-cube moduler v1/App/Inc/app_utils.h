/* ============================================================
 * App/Inc/app_utils.h
 * String helpers and parsing utilities.
 * ============================================================ */
#ifndef APP_UTILS_H
#define APP_UTILS_H

#include <stdint.h>
#include <stdbool.h>

void AppUtils_TrimInPlace(char *s);
void AppUtils_LowerInPlace(char *s);
bool AppUtils_StartsWith(const char *s, const char *prefix);
long AppUtils_ParseLongAfter(const char *s, const char *prefix, bool *ok);
float AppUtils_ParseFloatAfter(const char *s, const char *prefix, bool *ok);

#endif /* APP_UTILS_H */
