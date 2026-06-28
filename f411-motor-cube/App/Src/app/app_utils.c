/* ============================================================
 * App/Src/app_utils.c
 * String helpers and parsing utilities.
 * Extracted from app_main.c — behaviour must be identical.
 * ============================================================ */
#include "app_utils.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

void AppUtils_TrimInPlace(char *s)
{
    size_t len = strlen(s);
    while (len > 0U && (s[len-1] == '\r' || s[len-1] == '\n' ||
                        s[len-1] == ' '  || s[len-1] == '\t')) {
        s[--len] = '\0';
    }
    size_t start = 0U;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0U) memmove(s, s + start, strlen(s + start) + 1U);
}

void AppUtils_LowerInPlace(char *s)
{
    for (; *s; ++s) {
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
    }
}

bool AppUtils_StartsWith(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

long AppUtils_ParseLongAfter(const char *s, const char *prefix, bool *ok)
{
    if (!AppUtils_StartsWith(s, prefix)) { *ok = false; return 0; }
    s += strlen(prefix);
    while (*s == ' ') s++;
    if (*s == '\0') { *ok = false; return 0; }
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s)              { *ok = false; return 0; }
    while (*end == ' ') end++;
    if (*end != '\0')          { *ok = false; return 0; }
    *ok = true;
    return v;
}

float AppUtils_ParseFloatAfter(const char *s, const char *prefix, bool *ok)
{
    if (!AppUtils_StartsWith(s, prefix)) { *ok = false; return 0.0f; }
    s += strlen(prefix);
    while (*s == ' ') s++;
    if (*s == '\0') { *ok = false; return 0.0f; }
    char *end = NULL;
    float v = strtof(s, &end);
    if (end == s)              { *ok = false; return 0.0f; }
    while (*end == ' ') end++;
    if (*end != '\0')          { *ok = false; return 0.0f; }
    if (!isfinite(v))          { *ok = false; return 0.0f; }
    *ok = true;
    return v;
}
