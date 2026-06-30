#include "service_lock.h"
#include <string.h>
#include <ctype.h>
#include "stm32h7xx_hal.h"

/* ── Private state ─────────────────────────────────────────────────────────── */
static bool     s_unlocked;
static uint32_t s_unlockTick;
static uint32_t s_blockedCount;

/* ── Case-insensitive string equality (lowercase a/e) ──────────────────────── */
static bool strEqNoCase(const char *a, const char *b)
{
    while (*a && *b)
    {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return false;
        a++;
        b++;
    }
    return (*a == '\0') && (*b == '\0');
}

/* ── Case-insensitive prefix check ─────────────────────────────────────────── */
static bool strStartsNoCase(const char *s, const char *prefix)
{
    while (*prefix)
    {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix))
            return false;
        s++;
        prefix++;
    }
    return true;
}

/* ── Dangerous command lists ─────────────────────────────────────────────────
 *  Adapted from F446 isDangerousServiceCmd().  These are commands that
 *  can cause motor motion or alter persistent state on the F411.
 *  Service unlock is required before these can be forwarded. */

static const char *dangerousExact[] = {
    "gatetest", "identify", "test", "scan",
    "savecfg", "loadcfg", "saveall", "save",
    "map set", "map apply", "map reset",
    "map save", "map load", "map edit", "map discard",
    NULL
};

static const char *dangerousPrefixes[] = {
    "gatetest ", "base ", "boost ", "pi ",
    "kp ", "ki ", "kickduty ", "kickms ",
    "ramprate ", "rampms ", "defpwm ",
    "ramp ",       /* "ramp UP DOWN" changes ramp rates */
    NULL
};

/* ── Public functions ───────────────────────────────────────────────────────── */

void ServiceLock_Init(void)
{
    s_unlocked    = false;
    s_unlockTick  = 0;
    s_blockedCount = 0;
}

bool ServiceLock_TryUnlock(const char *token)
{
    if (token == NULL || token[0] == '\0')
        return false;

    if (strEqNoCase(token, SERVICE_TOKEN))
    {
        s_unlocked   = true;
        s_unlockTick = HAL_GetTick();
        return true;
    }
    return false;
}

void ServiceLock_Lock(void)
{
    s_unlocked = false;
}

bool ServiceLock_IsUnlocked(void)
{
    return s_unlocked;
}

void ServiceLock_Update(void)
{
    /* Auto-expire: called from main loop each iteration */
    if (s_unlocked &&
        (HAL_GetTick() - s_unlockTick) >= SERVICE_TIMEOUT_MS)
    {
        s_unlocked = false;
    }
}

void ServiceLock_GetStatus(ServiceLockStatus_t *out)
{
    if (out == NULL)
        return;

    out->unlocked     = s_unlocked;
    out->blocked_count = s_blockedCount;

    if (s_unlocked)
    {
        uint32_t elapsed = HAL_GetTick() - s_unlockTick;
        out->remaining_ms = (elapsed < SERVICE_TIMEOUT_MS)
                          ? (SERVICE_TIMEOUT_MS - elapsed)
                          : 0;
    }
    else
    {
        out->remaining_ms = 0;
    }
}

bool ServiceLock_CheckCommand(const char *cmd, bool requireUnlock)
{
    if (requireUnlock && !s_unlocked)
    {
        s_blockedCount++;
        return false;
    }
    return true;
}

bool ServiceLock_IsDangerousCmd(const char *cmd)
{
    if (cmd == NULL || cmd[0] == '\0')
        return false;

    /* Check exact matches (also matches with trailing space, e.g. "map set ")
     * to catch "identify" exactly and "map set" exactly */
    for (const char **d = dangerousExact; *d != NULL; d++)
    {
        if (strEqNoCase(cmd, *d))
            return true;
        /* Also match if cmd starts with dangerousExact + space,
         * e.g. "map set 0 3" matches "map set" */
        size_t dlen = strlen(*d);
        if (strStartsNoCase(cmd, *d) &&
            cmd[dlen] == ' ')
        {
            return true;
        }
    }

    /* Check prefix matches */
    for (const char **p = dangerousPrefixes; *p != NULL; p++)
    {
        if (strStartsNoCase(cmd, *p))
            return true;
    }

    return false;
}
