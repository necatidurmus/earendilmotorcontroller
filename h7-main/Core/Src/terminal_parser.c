#include "terminal_parser.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include "app_config.h"
#define MAX_LINE_LEN 64

#define RPM_MAX   H7_RPM_MAX_ROVER
#define DUTY_MAX   H7_DUTY_MAX

static bool allDigits(const char *str)
{
    if (*str == '\0')
        return false;
    for (const char *p = str; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
            return false;
    }
    return true;
}

/* ── Tuning-command helpers ──────────────────────────────────────────────── */

/* Return true if `s` is a valid integer (optional leading sign, digits). */
static bool IsInt(const char *s)
{
    if (s == NULL || *s == '\0')
        return false;
    const char *p = s;
    if (*p == '-' || *p == '+')
        p++;
    if (*p == '\0')
        return false;
    for (; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
            return false;
    }
    return true;
}

/* Return true if `s` is a valid number (integer or decimal float).
 * Accepts optional leading sign, at most one decimal point. */
static bool IsNumeric(const char *s)
{
    if (s == NULL || *s == '\0')
        return false;
    const char *p = s;
    if (*p == '-' || *p == '+')
        p++;
    if (*p == '\0')
        return false;
    bool dot = false;
    for (; *p; p++)
    {
        if (*p == '.')
        {
            if (dot) return false;
            dot = true;
        }
        else if (!isdigit((unsigned char)*p))
            return false;
    }
    return true;
}

/* Parse a 2-char motor tag ("fl"/"fr"/"rl"/"rr") and set *out accordingly.
 * Returns true if recognized. */

/* Advance *pp past one whitespace-delimited token, storing the token in
 * `tok` (max `tksz` bytes including NUL).  Returns true if a token was
 * found.  On success *pp points to the character after the token (either
 * a space or NUL). */
static bool NextToken(const char **pp, char *tok, size_t tksz)
{
    const char *p = *pp;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return false;
    size_t i = 0;
    while (p[i] != '\0' && p[i] != ' ' && i < tksz - 1)
    {
        tok[i] = p[i];
        i++;
    }
    tok[i] = '\0';
    *pp = p + i;
    return i > 0;
}

/* ── Tuning command parser ─────────────────────────────────────────────────
 * Called when the input starts with a known motor tag (FL/FR/RL/RR/ALL)
 * followed by a space.  `tag` is the 2- or 3-char lowercased tag,
 * `rest` points to the first character after the tag's trailing space,
 * and `restLen` is the remaining length.  On success fills outResult as
 * TCMD_MOTOR_TUNE and returns true; on failure returns false (caller falls
 * through to raw-motor or unknown-command handling). */
static bool ParseTuneCommand(const char *tag, TuneMotorTarget_t *target,
                             const char *rest, size_t restLen,
                             TerminalCommand_t *outResult)
{
    const char *p = rest;
    char kw[12];
    if (!NextToken(&p, kw, sizeof(kw)))
        return false;

    /* ── base P1 P2 P3 P4 P5 P6 P7 P8 ────────────────────────────────── */
    if (strcmp(kw, "base") == 0)
    {
        int vals[8];
        for (int i = 0; i < 8; i++)
        {
            char tok[8];
            if (!NextToken(&p, tok, sizeof(tok)))
                return false;
            if (!IsInt(tok))
                return false;
            vals[i] = atoi(tok);
            if (vals[i] < 0 || vals[i] > 4000)
                return false;
        }
        /* No extra tokens allowed */
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "base %d %d %d %d %d %d %d %d",
                         vals[0], vals[1], vals[2], vals[3],
                         vals[4], vals[5], vals[6], vals[7]);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_BASE;
        return true;
    }

    /* ── boost P1..P8 MS ──────────────────────────────────────────────── */
    if (strcmp(kw, "boost") == 0)
    {
        int vals[9]; /* 8 PWM + 1 MS */
        for (int i = 0; i < 9; i++)
        {
            char tok[8];
            if (!NextToken(&p, tok, sizeof(tok)))
                return false;
            if (!IsInt(tok))
                return false;
            vals[i] = atoi(tok);
            if (i < 8 && (vals[i] < 0 || vals[i] > 4000))
                return false;
            if (i == 8 && (vals[i] < 0 || vals[i] > (int)H7_BOOST_MS_MAX))
                return false;
        }
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "boost %d %d %d %d %d %d %d %d %d",
                         vals[0], vals[1], vals[2], vals[3],
                         vals[4], vals[5], vals[6], vals[7], vals[8]);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_BOOST;
        return true;
    }

    /* ── kickduty VALUE ────────────────────────────────────────────────── */
    if (strcmp(kw, "kickduty") == 0)
    {
        char tok[8];
        if (!NextToken(&p, tok, sizeof(tok)))
            return false;
        if (!IsInt(tok))
            return false;
        int v = atoi(tok);
        if (v < 0 || v > 4000)
            return false;
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "kickduty %d", v);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_KICKDUTY;
        return true;
    }

    /* ── kick duty VALUE  (two-word alias -> kickduty) ────────────────── */
    if (strcmp(kw, "kick") == 0)
    {
        char sub[8];
        if (!NextToken(&p, sub, sizeof(sub)))
            return false;
        if (strcmp(sub, "duty") == 0)
        {
            char tok[8];
            if (!NextToken(&p, tok, sizeof(tok)))
                return false;
            if (!IsInt(tok))
                return false;
            int v = atoi(tok);
            if (v < 0 || v > 4000)
                return false;
            { char junk; if (NextToken(&p, &junk, 1)) return false; }

            int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                             "kickduty %d", v);
            if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
                return false;
            outResult->type      = TCMD_MOTOR_TUNE;
            outResult->tuneTarget = *target;
            outResult->tuneKind   = TUNE_KIND_KICKDUTY;
            return true;
        }
        if (strcmp(sub, "ms") == 0)
        {
            char tok[8];
            if (!NextToken(&p, tok, sizeof(tok)))
                return false;
            if (!IsInt(tok))
                return false;
            int v = atoi(tok);
            if (v < 0 || v > (int)H7_KICKMS_MAX)
                return false;
            { char junk; if (NextToken(&p, &junk, 1)) return false; }

            int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                             "kickms %d", v);
            if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
                return false;
            outResult->type      = TCMD_MOTOR_TUNE;
            outResult->tuneTarget = *target;
            outResult->tuneKind   = TUNE_KIND_KICKMS;
            return true;
        }
        /* "kick <unknown>" — not a tune command */
        return false;
    }

    /* ── kickms VALUE ──────────────────────────────────────────────────── */
    if (strcmp(kw, "kickms") == 0)
    {
        char tok[8];
        if (!NextToken(&p, tok, sizeof(tok)))
            return false;
        if (!IsInt(tok))
            return false;
        int v = atoi(tok);
        if (v < 0 || v > (int)H7_KICKMS_MAX)
            return false;
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "kickms %d", v);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_KICKMS;
        return true;
    }

    /* ── ramp UP DOWN ─────────────────────────────────────────────────── */
    if (strcmp(kw, "ramp") == 0)
    {
        char t1[12], t2[12];
        if (!NextToken(&p, t1, sizeof(t1)))
            return false;
        if (!IsNumeric(t1))
            return false;
        if (!NextToken(&p, t2, sizeof(t2)))
            return false;
        if (!IsNumeric(t2))
            return false;
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "ramp %s %s", t1, t2);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_RAMP;
        return true;
    }

    /* ── rampup UP  (rejected — use "ramp UP DOWN") ───────────────────── */
    if (strcmp(kw, "rampup") == 0)
    {
        return false; /* caller falls through to raw or unknown */
    }

    /* ── rampdown DOWN  (rejected — use "ramp UP DOWN") ───────────────── */
    if (strcmp(kw, "rampdown") == 0)
    {
        return false;
    }

    /* ── pi KP KI ─────────────────────────────────────────────────────── */
    if (strcmp(kw, "pi") == 0)
    {
        char t1[12], t2[12];
        if (!NextToken(&p, t1, sizeof(t1)))
            return false;
        if (!IsNumeric(t1))
            return false;
        if (!NextToken(&p, t2, sizeof(t2)))
            return false;
        if (!IsNumeric(t2))
            return false;
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "pi %s %s", t1, t2);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_PI;
        return true;
    }

    /* ── kp VALUE  (alias -> pi VALUE 0)  — not used by GUI, skip ────── */
    /* ── ki VALUE  (alias -> pi 0 VALUE)  — not used by GUI, skip ────── */

    /* ── telper MS ────────────────────────────────────────────────────── */
    if (strcmp(kw, "telper") == 0)
    {
        char tok[8];
        if (!NextToken(&p, tok, sizeof(tok)))
            return false;
        if (!IsInt(tok))
            return false;
        int v = atoi(tok);
        if (v < (int)H7_TELPER_MIN || v > (int)H7_TELPER_MAX)
            return false;
        { char junk; if (NextToken(&p, &junk, 1)) return false; }

        int n = snprintf(outResult->tunePayload, TUNE_PAYLOAD_MAX,
                         "telper %d", v);
        if (n < 0 || (size_t)n >= TUNE_PAYLOAD_MAX)
            return false;
        outResult->type      = TCMD_MOTOR_TUNE;
        outResult->tuneTarget = *target;
        outResult->tuneKind   = TUNE_KIND_TELPER;
        return true;
    }

    /* Not a recognized tuning keyword — caller falls through to raw. */
    return false;
}

/* Common setup for a motion command. Stores both the clamped and original
 * value plus clamping state. Does not execute or log anything. */
static void FillMotion(TerminalCommand_t *out, Direction_t dir,
                       int raw, int max)
{
    out->type          = TCMD_MOTION;
    out->motion.direction = dir;

    out->originalValue = (uint16_t)raw;
    out->hasValue      = true;

    if (raw > max)
    {
        out->value     = (uint16_t)max;
        out->wasClamped = true;
    }
    else
    {
        out->value     = (uint16_t)raw;
        out->wasClamped = false;
    }

    out->motion.speed  = (uint16_t)out->value;
}

bool TerminalParser_Parse(const char *line, TerminalCommand_t *outResult)
{
    if (line == NULL || outResult == NULL)
        return false;

    memset(outResult, 0, sizeof(*outResult));
    outResult->isDuty = false;

    char buf[MAX_LINE_LEN];
    size_t len = strlen(line);
    if (len >= MAX_LINE_LEN)
        return false;

    memcpy(buf, line, len + 1);

    while (*buf && isspace((unsigned char)buf[0]))
        memmove(buf, buf + 1, strlen(buf));

    len = strlen(buf);
    while (len > 0 && isspace((unsigned char)buf[len - 1]))
        buf[--len] = '\0';

    for (size_t i = 0; i < len; i++)
        buf[i] = (char)tolower((unsigned char)buf[i]);

    /* ── 1. help ─────────────────────────────────────────────────────── */
    if (strcmp(buf, "help") == 0)
    {
        outResult->type = TCMD_HELP;
        return true;
    }

    /* ── 2. stop ─────────────────────────────────────────────────────── */
    if (strcmp(buf, "stop") == 0)
    {
        outResult->type = TCMD_STOP;
        outResult->motion.direction = DIR_STOP;
        outResult->motion.speed = 0;
        return true;
    }

    /* ── 3. brake ────────────────────────────────────────────────────── */
    if (strcmp(buf, "brake") == 0)
    {
        outResult->type = TCMD_BRAKE;
        return true;
    }

    /* ── 3b. estop ─────────────────────────────────────────────────────── */
    if (strcmp(buf, "estop") == 0)
    {
        outResult->type = TCMD_ESTOP;
        return true;
    }

    /* ── 3c. safe / alloff ─────────────────────────────────────────────── */
    if (strcmp(buf, "safe") == 0 || strcmp(buf, "alloff") == 0)
    {
        outResult->type = TCMD_SAFE;
        return true;
    }

    /* ── 4. identify ─────────────────────────────────────────────────── */
    if (strcmp(buf, "identify") == 0)
    {
        outResult->type = TCMD_IDENTIFY;
        return true;
    }

    /* ── 5. status ───────────────────────────────────────────────────── */
    if (strcmp(buf, "status") == 0)
    {
        outResult->type = TCMD_STATUS;
        return true;
    }

    /* ── 5b. hall ────────────────────────────────────────────────────── */
    if (strcmp(buf, "hall") == 0 || strcmp(buf, "h") == 0)
    {
        outResult->type = TCMD_HALL;
        return true;
    }

    /* ── 6. m speed (control mode) ──────────────────────────────────── */
    if (strcmp(buf, "m speed") == 0)
    {
        outResult->type = TCMD_MODE_RPM;
        return true;
    }

    /* ── 7. m duty (control mode) ───────────────────────────────────── */
    if (strcmp(buf, "m duty") == 0)
    {
        outResult->type = TCMD_MODE_PWM;
        return true;
    }

    /* ── 7b. mode speed / mode duty (control mode aliases) ────────────
     *  Same synchronized control-mode switch as `m speed` / `m duty`.
     *  These are distinct from the rover operating-mode commands
     *  (`mode disarm`/`mode manual`/`mode auto`) parsed below. */
    if (strcmp(buf, "mode speed") == 0)
    {
        outResult->type = TCMD_MODE_RPM;
        return true;
    }
    if (strcmp(buf, "mode duty") == 0)
    {
        outResult->type = TCMD_MODE_PWM;
        return true;
    }

    /* ── 8. mode (plain query) ───────────────────────────────────────── */
    if (strcmp(buf, "mode") == 0)
    {
        outResult->type = TCMD_MODE_QUERY;
        return true;
    }

    /* ── 9. Operating-mode transitions ─────────────────────────────────
     * These are NOT executed here; the parser only classifies them as
     * TCMD_OP_MODE.  command_handler.c routes them through OperatingMode_Set()
     * which enforces the DISARM safety lock and GPIO/LED update. */
    if (strcmp(buf, "mode disarm") == 0)
    {
        outResult->type   = TCMD_OP_MODE;
        outResult->opMode = ROVER_MODE_DISARM;
        return true;
    }

    if (strcmp(buf, "mode manual") == 0)
    {
        outResult->type   = TCMD_OP_MODE;
        outResult->opMode = ROVER_MODE_MANUAL;
        return true;
    }

    if (strcmp(buf, "mode auto") == 0 ||
        strcmp(buf, "mode autonomous") == 0)
    {
        outResult->type   = TCMD_OP_MODE;
        outResult->opMode = ROVER_MODE_AUTONOMOUS;
        return true;
    }

    /* ── 9a. Bridge commands: bridge on/off/status/unlock_service/lock_service ──
     *  Bridge prefix must be parsed before motor tags (FL/FR/RL/RR).  */
    if (len >= 7 && buf[0] == 'b' && buf[1] == 'r' && buf[2] == 'i' &&
        buf[3] == 'd' && buf[4] == 'g' && buf[5] == 'e' && buf[6] == ' ')
    {
        const char *arg = buf + 7;
        while (*arg == ' ') arg++;

        if (strcmp(arg, "on") == 0)
        {
            outResult->type = TCMD_BRIDGE;
            outResult->bridgeAction = 0; /* BRIDGE_ON */
            return true;
        }
        if (strcmp(arg, "off") == 0)
        {
            outResult->type = TCMD_BRIDGE;
            outResult->bridgeAction = 1; /* BRIDGE_OFF */
            return true;
        }
        if (strcmp(arg, "status") == 0)
        {
            outResult->type = TCMD_BRIDGE;
            outResult->bridgeAction = 2; /* BRIDGE_STATUS */
            return true;
        }
        /* bridge unlock_service <token> */
        if (strncmp(arg, "unlock_service ", 15) == 0)
        {
            const char *token = arg + 15;
            while (*token == ' ') token++;
            if (*token != '\0')
            {
                outResult->type = TCMD_BRIDGE;
                outResult->bridgeAction = 3; /* BRIDGE_UNLOCK_SERVICE */
                strncpy(outResult->bridgeToken, token, RAW_PAYLOAD_MAX - 1);
                outResult->bridgeToken[RAW_PAYLOAD_MAX - 1] = '\0';
                return true;
            }
            /* empty token — fall through to unknown */
        }
        if (strcmp(arg, "lock_service") == 0)
        {
            outResult->type = TCMD_BRIDGE;
            outResult->bridgeAction = 4; /* BRIDGE_LOCK_SERVICE */
            return true;
        }
        /* Unknown bridge sub-command — fall through to unknown */
    }

    /* ── 9a-2. Service alias commands: service unlock/lock/status ────────── */
    if (len >= 8 && buf[0] == 's' && buf[1] == 'e' && buf[2] == 'r' &&
        buf[3] == 'v' && buf[4] == 'i' && buf[5] == 'c' && buf[6] == 'e' &&
        buf[7] == ' ')
    {
        const char *arg = buf + 8;
        while (*arg == ' ') arg++;

        /* service unlock <token> */
        if (strncmp(arg, "unlock ", 7) == 0)
        {
            const char *token = arg + 7;
            while (*token == ' ') token++;
            if (*token != '\0')
            {
                outResult->type = TCMD_SERVICE;
                outResult->serviceAction = 0; /* SERVICE_UNLOCK */
                strncpy(outResult->serviceToken, token, RAW_PAYLOAD_MAX - 1);
                outResult->serviceToken[RAW_PAYLOAD_MAX - 1] = '\0';
                return true;
            }
        }
        if (strcmp(arg, "lock") == 0)
        {
            outResult->type = TCMD_SERVICE;
            outResult->serviceAction = 1; /* SERVICE_LOCK */
            return true;
        }
        if (strcmp(arg, "status") == 0)
        {
            outResult->type = TCMD_SERVICE;
            outResult->serviceAction = 2; /* SERVICE_STATUS */
            return true;
        }
        /* Unknown service sub-command — fall through */
    }

    /* ── 9b. Motor commands: FL/FR/RL/RR [tune | raw] ──────────────────
     *  Must be parsed before the single-letter motion commands (e.g.
     *  "FL f100" must not fall through to the f<number> branch).
     *  First tries to parse as a validated tuning command (TCMD_MOTOR_TUNE).
     *  If the keyword after the tag is not a recognised tuning keyword,
     *  falls through to raw-motor forwarding (TCMD_MOTOR_RAW). */
    if (len >= 2 &&
        (buf[0] == 'f' || buf[0] == 'r') &&
        (buf[1] == 'l' || buf[1] == 'r'))
    {
        TuneMotorTarget_t target = TUNE_MOTOR_NONE;
        if      (buf[0] == 'f' && buf[1] == 'l') target = TUNE_MOTOR_FL;
        else if (buf[0] == 'f' && buf[1] == 'r') target = TUNE_MOTOR_FR;
        else if (buf[0] == 'r' && buf[1] == 'l') target = TUNE_MOTOR_RL;
        else if (buf[0] == 'r' && buf[1] == 'r') target = TUNE_MOTOR_RR;

        if (target != TUNE_MOTOR_NONE)
        {
            /* Bare tag: "FL" (no payload). */
            if (len == 2)
            {
                outResult->type            = TCMD_MOTOR_RAW;
                outResult->rawMotor        = (MotorId_t)((int)target - 1);
                outResult->rawPayload[0]   = '\0';
                return true;
            }

            /* Must be followed by exactly one space, then a non-empty payload. */
            if (buf[2] == ' ')
            {
                const char *payload = buf + 3;
                size_t plen = len - 3;
                if (plen == 0)
                {
                    outResult->type          = TCMD_MOTOR_RAW;
                    outResult->rawMotor      = (MotorId_t)((int)target - 1);
                    outResult->rawPayload[0] = '\0';
                    return true;
                }
                if (plen > 0 && plen < sizeof(outResult->rawPayload))
                {
                    /* Try tuning command first */
                    TuneMotorTarget_t tt = target;
                    if (ParseTuneCommand(NULL, &tt, payload, plen, outResult))
                        return true;

                    /* Not a tuning keyword — fall through to raw forwarding */
                    outResult->type     = TCMD_MOTOR_RAW;
                    outResult->rawMotor = (MotorId_t)((int)target - 1);
                    memcpy(outResult->rawPayload, payload, plen + 1);
                    return true;
                }
                /* plen too long: fall through to "unknown" */
            }
        }
    }

    /* ── 9c. ALL motor commands: ALL <command> ────────────────────────────
     *  "ALL" can be used with identify, status, hall, stop, safe, brake,
     *  estop, and tuning commands.  For motion (f/b/r/l), use the rover-
     *  level motion commands instead, not ALL. */
    if (len >= 3 && buf[0] == 'a' && buf[1] == 'l' && buf[2] == 'l')
    {
        if (len == 3)
        {
            /* bare "ALL" — not valid */
            return false;
        }
        if (buf[3] == ' ')
        {
            const char *payload = buf + 4;
            size_t plen = len - 4;
            if (plen > 0)
            {
                /* ALL identify — broadcast identify */
                if (strcmp(payload, "identify") == 0)
                {
                    outResult->type = TCMD_IDENTIFY;
                    return true;
                }
                /* ALL status — broadcast status */
                if (strcmp(payload, "status") == 0)
                {
                    outResult->type = TCMD_STATUS;
                    return true;
                }
                /* ALL hall — broadcast hall */
                if (strcmp(payload, "hall") == 0 || strcmp(payload, "h") == 0)
                {
                    outResult->type = TCMD_HALL;
                    return true;
                }
                /* ALL stop — broadcast stop (already handled as TCMD_STOP) */
                if (strcmp(payload, "stop") == 0 || strcmp(payload, "s") == 0)
                {
                    outResult->type = TCMD_STOP;
                    outResult->motion.direction = DIR_STOP;
                    outResult->motion.speed = 0;
                    return true;
                }
                /* ALL safe/alloff */
                if (strcmp(payload, "safe") == 0 || strcmp(payload, "alloff") == 0)
                {
                    outResult->type = TCMD_SAFE;
                    return true;
                }
                /* ALL brake */
                if (strcmp(payload, "brake") == 0 || strcmp(payload, "x") == 0)
                {
                    outResult->type = TCMD_BRAKE;
                    return true;
                }
                /* ALL estop */
                if (strcmp(payload, "estop") == 0)
                {
                    outResult->type = TCMD_ESTOP;
                    return true;
                }
                /* ALL mode duty / mode speed */
                if (strcmp(payload, "mode duty") == 0)
                {
                    outResult->type = TCMD_MODE_PWM;
                    return true;
                }
                if (strcmp(payload, "mode speed") == 0)
                {
                    outResult->type = TCMD_MODE_RPM;
                    return true;
                }
                /* Try tuning command (base, boost, pi, etc.) */
                TuneMotorTarget_t target = TUNE_MOTOR_ALL;
                if (ParseTuneCommand(NULL, &target, payload, plen, outResult))
                    return true;

                /* Fall through: ALL with unknown payload -> error */
            }
        }
    }

    /* ── 10. fd / bd / rd / ld (duty, value 0..4000) ─────────────────── */
    if (len >= 3 && buf[1] == 'd' &&
        (buf[0] == 'f' || buf[0] == 'b' || buf[0] == 'r' || buf[0] == 'l'))
    {
        const char *valStr = buf + 2;
        if (!allDigits(valStr))
            return false;

        int val = atoi(valStr);
        outResult->isDuty = true;

        Direction_t dir = DIR_STOP;
        switch (buf[0])
        {
            case 'f': dir = DIR_FORWARD;  break;
            case 'b': dir = DIR_BACKWARD; break;
            case 'r': dir = DIR_RIGHT;    break;
            case 'l': dir = DIR_LEFT;     break;
        }

        FillMotion(outResult, dir, val, DUTY_MAX);
        return true;
    }

    /* ── 11. f / b / r / l (RPM, value 0..200) ──────────────────────── */
    if (len >= 2)
    {
        char dir = buf[0];
        if (dir == 'f' || dir == 'b' || dir == 'r' || dir == 'l')
        {
            const char *valStr = buf + 1;
            if (allDigits(valStr))
            {
                int val = atoi(valStr);

                Direction_t direction = DIR_STOP;
                switch (dir)
                {
                    case 'f': direction = DIR_FORWARD;  break;
                    case 'b': direction = DIR_BACKWARD; break;
                    case 'r': direction = DIR_RIGHT;    break;
                    case 'l': direction = DIR_LEFT;     break;
                }

                FillMotion(outResult, direction, val, RPM_MAX);
                return true;
            }
        }
    }

    return false;
}
