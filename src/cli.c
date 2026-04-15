/*
 * cli.c — Motor ilk enerjileme için seri komut satırı arayüzü
 *
 * UART2 (PA2/PA3) 115200 baud kullanır.
 * Non-blocking satır okuyucu ile idle-timeout otomatik işleme.
 *
 * Tüm yazdırma işlemleri ana döngü bağlamında yapılır (ISR'dan asla).
 * ISR durumu yazdırma öncesinde kopyalanır (tutarlılık için).
 */

#include "cli.h"
#include "motor_config.h"
#include "board_io.h"
#include "hall.h"
#include "protection.h"
#include "bldc_commutation.h"

#include "stm32f4xx_hal.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/*
 * RunMode tipi bldc_commutation.h'de tanımlıdır (tek kaynak).
 * main.c'deki volatile değişkenlere extern erişim.
 */

/* CLI tarafından yazılır/okunur, ISR tarafından tüketilir */
extern volatile RunMode   g_runMode;
extern volatile uint16_t  g_commandDuty;
extern volatile uint16_t  g_appliedDuty;

/* ISR tick sayacı (status gösterimi için) */
extern volatile uint32_t  g_isrTickCount;

/* ====================================================================
 * Satır tamponu durumu
 * ==================================================================== */

static char     lineBuf[CLI_LINE_BUF];
static uint8_t  linePos = 0;
static uint32_t lastRxTick = 0;

/* ====================================================================
 * Taşıma katmanı yardımcıları
 * ==================================================================== */

static void cliTransportWrite(const uint8_t *data, uint16_t len) {
    HAL_UART_Transmit(&huart2, (uint8_t *)data, len, 100);
    BoardIO_KickWatchdog();  /* uzun print dizilerinde watchdog timeout'unu önle */
}

static int cliTransportRead(uint8_t *c) {
    return (HAL_UART_Receive(&huart2, c, 1, 0) == HAL_OK);
}

static void cliPrint(const char *s) {
    cliTransportWrite((const uint8_t *)s, (uint16_t)strlen(s));
}

static void cliPrintln(const char *s) {
    cliPrint(s);
    cliPrint("\r\n");
}

static void cliPrintUint(uint32_t val) {
    char buf[12];
    int i = 0;
    if (val == 0) {
        cliPrint("0");
        return;
    }
    while (val > 0) {
        buf[i++] = '0' + (val % 10);
        val /= 10;
    }
    for (int j = i - 1; j >= 0; --j) {
        char c = buf[j];
        cliTransportWrite((const uint8_t *)&c, 1);
    }
}

static void cliPrintInt(int32_t val) {
    if (val < 0) {
        cliPrint("-");
        if (val == INT32_MIN) {
            cliPrintUint((uint32_t)(-(val + 1)) + 1);
            return;
        }
        val = -val;
    }
    cliPrintUint((uint32_t)val);
}

static void cliPrintFloat(float val, int decimals) {
    if (val < 0.0f) {
        cliPrint("-");
        val = -val;
    }
    int32_t integer = (int32_t)val;
    cliPrintInt(integer);
    cliPrint(".");
    float frac = val - (float)integer;
    for (int i = 0; i < decimals; ++i) {
        frac *= 10.0f;
    }
    int32_t fracInt = (int32_t)(frac + 0.5f);
    if (fracInt < 0) fracInt = 0;
    /* Print with leading zeros */
    char buf[16];
    int len = decimals;
    for (int i = len - 1; i >= 0; --i) {
        buf[i] = '0' + (fracInt % 10);
        fracInt /= 10;
    }
    buf[len] = '\0';
    cliPrint(buf);
}

static void cliPrintHallBits(uint8_t hall) {
    char b[4];
    b[0] = '0' + ((hall >> 2) & 1);
    b[1] = '0' + ((hall >> 1) & 1);
    b[2] = '0' + (hall & 1);
    b[3] = '\0';
    cliPrint(b);
}

/* ====================================================================
 * Komut işleyicileri
 * ==================================================================== */

static const char* modeName(RunMode m) {
    switch (m) {
        case RUN_FORWARD:  return "FORWARD";
        case RUN_BACKWARD: return "BACKWARD";
        default:            return "STOPPED";
    }
}

static int cliRequireStoppedConfig(const char *cmdName) {
    if (g_runMode != RUN_STOPPED || g_appliedDuty != 0) {
        cliPrint("'");
        cliPrint(cmdName);
        cliPrintln("' only allowed in STOPPED mode (applied duty must be 0).");
        return 0;
    }
    return 1;
}

static void cmdSetMode(RunMode mode) {
    if (mode != RUN_STOPPED && Prot_IsFaulted()) {
        cliPrintln("FAULT active. Use 'clear' first.");
        return;
    }
    g_runMode = mode;
    if (mode == RUN_FORWARD) {
        Hall_SetDirection(1);
    } else if (mode == RUN_BACKWARD) {
        Hall_SetDirection(0);
    }
    if (mode == RUN_STOPPED) {
        g_appliedDuty = 0;
    }
    cliPrint("Mode: ");
    cliPrintln(modeName(mode));
}

static void cmdPwm(const char *arg) {
    if (!arg) { cliPrintln("Usage: pwm <0..255>"); return; }
    long val = atol(arg);
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    g_commandDuty = (uint16_t)val;
    cliPrint("Duty cmd: ");
    cliPrintUint(g_commandDuty);
    cliPrintln("");
}

static void cmdStatus(void) {
    cliPrintln("");
    cliPrintln("=== EARENDIL STATUS ===");

    cliPrint("Mode: ");
    cliPrintln(modeName(g_runMode));

    cliPrint("Duty cmd=");
    cliPrintUint(g_commandDuty);
    cliPrint(" applied=");
    cliPrintUint(g_appliedDuty);
    cliPrintln("");

    HallConfig hcfg;
    Hall_GetConfig(&hcfg);
    cliPrint("Hall prof=");
    cliPrintUint(hcfg.profile);
    cliPrint(" mask=");
    cliPrintUint(hcfg.polarityMask);
    cliPrint(" offset=");
    cliPrintInt(hcfg.stateOffset);
    cliPrintln("");

    ProtectionConfig pcfg;
    Prot_GetConfig(&pcfg);
    cliPrint("Limits soft=");
    cliPrintUint(pcfg.softLimitAdc);
    cliPrint(" hard=");
    cliPrintUint(pcfg.hardLimitAdc);
    cliPrintln("");

    UndervoltageConfig ucfg;
    Prot_GetUndervoltageConfig(&ucfg);
    cliPrint("UV en=");
    cliPrint(ucfg.enabled ? "1" : "0");
    cliPrint(" lim_mV=");
    cliPrintUint(ucfg.limitMv);
    cliPrint(" hyst_mV=");
    cliPrintUint(ucfg.hysteresisMv);
    cliPrint(" strikes=");
    cliPrintUint(ucfg.strikesToTrip);
    cliPrintln("");

    cliPrint("Fault: ");
    cliPrintln(Prot_IsFaulted() ? Prot_GetFaultReason() : "none");

    cliPrint("ISR ticks: ");
    cliPrintUint(g_isrTickCount);
    cliPrintln("");

    /* Hall snapshot */
    HallSnapshot hs;
    Hall_GetSnapshot(&hs);
    cliPrint("Hall raw=");
    cliPrintHallBits(hs.raw);
    cliPrint(" corr=");
    cliPrintHallBits(hs.corrected);
    cliPrint(" map=");
    if (hs.mapped <= 5) cliPrintUint(hs.mapped); else cliPrint("INV");
    cliPrint(" acc=");
    if (hs.accepted <= 5) cliPrintUint(hs.accepted); else cliPrint("INV");
    cliPrint(" drvHall=");
    if (hs.driveState <= 5) cliPrintUint(hs.driveState); else cliPrint("OFF");
    cliPrint(" drvAct=");
    if (Comm_GetActiveState() <= 5) cliPrintUint(Comm_GetActiveState()); else cliPrint("OFF");
    cliPrintln("");

    /* Current snapshot */
    ProtectionSnapshot ps;
    Prot_GetSnapshot(&ps);
    cliPrint("I raw=");
    cliPrintUint(ps.currentRaw);
    cliPrint(" filt=");
    cliPrintUint(ps.currentFiltered);
    cliPrint(" off=");
    cliPrintUint(ps.currentOffset);
    cliPrint(" dlt=");
    cliPrintUint(ps.currentDelta);
    cliPrint(" estA=");
    cliPrintFloat(ps.estimatedAmps, 2);
    cliPrint(" V est=");
    cliPrintFloat(ps.estimatedVolts, 2);
    cliPrint(" raw=");
    cliPrintUint(ps.voltageRaw);
    cliPrint(" uvStrikes=");
    cliPrintUint(ps.undervoltageStrikes);
    cliPrintln("");

    /* Active commutation state */
    cliPrint("Comm state=");
    if (Comm_GetActiveState() <= 5) cliPrintUint(Comm_GetActiveState()); else cliPrint("OFF");
    cliPrint(" pwm=");
    cliPrintUint(Comm_GetActiveDuty());
    cliPrintln("");

    cliPrintln("=======================");
    cliPrintln("");
}

static void cmdHall(void) {
    HallSnapshot hs;
    Hall_GetSnapshot(&hs);
    cliPrint("Hall raw=");
    cliPrintHallBits(hs.raw);
    cliPrint(" corr=");
    cliPrintHallBits(hs.corrected);
    cliPrint(" map=");
    if (hs.mapped <= 5) cliPrintUint(hs.mapped); else cliPrint("INV");
    cliPrint(" acc=");
    if (hs.accepted <= 5) cliPrintUint(hs.accepted); else cliPrint("INV");
    cliPrint(" drvHall=");
    if (hs.driveState <= 5) cliPrintUint(hs.driveState); else cliPrint("OFF");
    cliPrint(" drvAct=");
    if (Comm_GetActiveState() <= 5) cliPrintUint(Comm_GetActiveState()); else cliPrint("OFF");
    cliPrintln("");
}

static void cmdCurrent(void) {
    ProtectionSnapshot ps;
    Prot_GetSnapshot(&ps);
    cliPrint("I raw=");
    cliPrintUint(ps.currentRaw);
    cliPrint(" filt=");
    cliPrintUint(ps.currentFiltered);
    cliPrint(" off=");
    cliPrintUint(ps.currentOffset);
    cliPrint(" dlt=");
    cliPrintUint(ps.currentDelta);
    cliPrint(" estA=");
    cliPrintFloat(ps.estimatedAmps, 2);
    cliPrint(" soft=");
    cliPrint(ps.softLimitActive ? "ACT" : "off");
    cliPrint(" strikes=");
    cliPrintUint(ps.hardStrikes);
    cliPrint(" uv=");
    cliPrintUint(ps.undervoltageStrikes);
    cliPrint(" | V est=");
    cliPrintFloat(ps.estimatedVolts, 2);
    cliPrint(" raw=");
    cliPrintUint(ps.voltageRaw);
    cliPrintln("");
}

static void cmdHinv(const char *arg) {
    if (!cliRequireStoppedConfig("hinv")) return;
    if (!arg) { cliPrintln("Usage: hinv <0|1>"); return; }
    long val = atol(arg);
    uint8_t mask = (val != 0) ? 0x07 : 0x00;
    Hall_SetPolarityMask(mask);
    cliPrint("Hall invert mask=");
    cliPrintUint(mask);
    cliPrintln("");
}

static void cmdHmask(const char *arg) {
    if (!cliRequireStoppedConfig("hmask")) return;
    if (!arg) { cliPrintln("Usage: hmask <0..7>"); return; }
    long val = atol(arg);
    if (val < 0 || val > 7) { cliPrintln("Range: 0..7"); return; }
    Hall_SetPolarityMask((uint8_t)val);
    cliPrint("Hall mask=");
    cliPrintUint((uint8_t)val);
    cliPrintln("");
}

static void cmdOffset(const char *arg) {
    if (!cliRequireStoppedConfig("offset")) return;
    if (!arg) { cliPrintln("Usage: offset <-5..5>"); return; }
    long val = atol(arg);
    if (val < -5 || val > 5) { cliPrintln("Range: -5..5"); return; }
    Hall_SetStateOffset((int8_t)val);
    cliPrint("Offset=");
    cliPrintInt(val);
    cliPrintln("");
}

static void cmdMap(const char *arg) {
    if (!cliRequireStoppedConfig("map")) return;
    if (!arg) { cliPrintln("Usage: map <0..3>"); return; }
    long val = atol(arg);
    if (val < 0 || val >= HALL_PROFILE_COUNT) {
        cliPrintln("Range: 0..3");
        return;
    }
    Hall_SetProfile((uint8_t)val);
    cliPrint("Profile=");
    cliPrintUint((uint8_t)val);
    cliPrintln("");
}

static void cmdLimits(const char *softStr, const char *hardStr) {
    if (!cliRequireStoppedConfig("limits")) return;
    if (!softStr || !hardStr) {
        cliPrintln("Usage: limits <soft> <hard>");
        return;
    }
    long soft = atol(softStr);
    long hard = atol(hardStr);
    if (soft < 1 || hard < 1 || hard <= soft || soft > 4095 || hard > 4095) {
        cliPrintln("Invalid: 1 <= soft < hard <= 4095");
        return;
    }
    Prot_SetLimits((uint16_t)soft, (uint16_t)hard);
    cliPrint("Limits soft=");
    cliPrintUint((uint16_t)soft);
    cliPrint(" hard=");
    cliPrintUint((uint16_t)hard);
    cliPrintln("");
}

static void cmdGain(const char *arg) {
    if (!cliRequireStoppedConfig("gain")) return;
    if (!arg) { cliPrintln("Usage: gain <1..1000>"); return; }
    long val = atol(arg);
    if (val < 1 || val > 1000) { cliPrintln("Range: 1..1000"); return; }
    Prot_SetInaGain((float)val);
    cliPrint("INA gain=");
    cliPrintUint((uint32_t)val);
    cliPrintln("");
}

static void cmdZeroi(void) {
    if (!cliRequireStoppedConfig("zeroi")) return;
    cliPrintln("Calibrating ISENSE offset...");
    Prot_CalibrateOffset();
    ProtectionSnapshot ps;
    Prot_GetSnapshot(&ps);
    cliPrint("Offset=");
    cliPrintUint(ps.currentOffset);
    cliPrintln(" done");
}

static void cmdClear(void) {
    if (!Prot_IsFaulted()) {
        cliPrintln("No active fault.");
        return;
    }
    if (g_runMode != RUN_STOPPED || g_appliedDuty != 0) {
        cliPrintln("Stop motor first before clear.");
        return;
    }
    if (g_commandDuty != 0) {
        cliPrintln("Set pwm 0 before clear.");
        return;
    }
    Prot_ClearFault();
    g_commandDuty = 0;
    BoardIO_RearmPWMOutputs();
    cliPrintln("Fault cleared. Re-arm with pwm + mode command.");
}

static void cmdUv(const char *limStr, const char *hystStr, const char *strikesStr) {
    if (!cliRequireStoppedConfig("uv")) return;
    if (!limStr || !hystStr || !strikesStr) {
        cliPrintln("Usage: uv <limit_mV> <hyst_mV> <strikes>");
        return;
    }

    long lim = atol(limStr);
    long hyst = atol(hystStr);
    long strikes = atol(strikesStr);

    if (lim < 1000 || lim > 60000) {
        cliPrintln("limit range: 1000..60000 mV");
        return;
    }
    if (hyst < 0 || hyst > 5000) {
        cliPrintln("hyst range: 0..5000 mV");
        return;
    }
    if (strikes < 1 || strikes > 50) {
        cliPrintln("strikes range: 1..50");
        return;
    }

    UndervoltageConfig cfg;
    Prot_GetUndervoltageConfig(&cfg);
    cfg.limitMv = (uint16_t)lim;
    cfg.hysteresisMv = (uint16_t)hyst;
    cfg.strikesToTrip = (uint8_t)strikes;
    Prot_SetUndervoltageConfig(&cfg);

    cliPrint("UV cfg set: lim=");
    cliPrintUint(cfg.limitMv);
    cliPrint(" hyst=");
    cliPrintUint(cfg.hysteresisMv);
    cliPrint(" strikes=");
    cliPrintUint(cfg.strikesToTrip);
    cliPrintln("");
}

static void cmdUven(const char *arg) {
    if (!cliRequireStoppedConfig("uven")) return;
    if (!arg) {
        cliPrintln("Usage: uven <0|1>");
        return;
    }

    long en = atol(arg);
    if (en != 0 && en != 1) {
        cliPrintln("Range: 0|1");
        return;
    }

    UndervoltageConfig cfg;
    Prot_GetUndervoltageConfig(&cfg);
    cfg.enabled = (en != 0);
    Prot_SetUndervoltageConfig(&cfg);

    cliPrint("UV enable=");
    cliPrint(cfg.enabled ? "1" : "0");
    cliPrintln("");
}

static void cmdHelp(void) {
    cliPrintln("");
    cliPrintln("Earendil BLDC Controller CLI");
    cliPrintln("Commands:");
    cliPrintln("  forward|f       run forward");
    cliPrintln("  backward|b      run backward");
    cliPrintln("  stop|s          stop motor");
    cliPrintln("  pwm <0..255>    set duty");
    cliPrintln("  status          full status");
    cliPrintln("  hall            hall snapshot");
    cliPrintln("  current         current snapshot");
    cliPrintln("  hinv <0|1>      invert halls");
    cliPrintln("  hmask <0..7>    hall XOR mask");
    cliPrintln("  offset <-5..5>  state offset");
    cliPrintln("  map <0..3>      hall profile");
    cliPrintln("  limits s h      current limits");
    cliPrintln("  uv l h s        uv cfg mV,mV,strikes");
    cliPrintln("  uven <0|1>      enable/disable uv fault");
    cliPrintln("  gain <1..1000>  INA gain for estA");
    cliPrintln("  zeroi           recalibrate offset");
    cliPrintln("  clear           clear fault");
    cliPrintln("  note: config cmds only in STOPPED");
    cliPrintln("  note: clear needs STOPPED + pwm 0");
    cliPrintln("  help|?          this help");
    cliPrintln("");
}

/* ====================================================================
 * Command dispatcher
 * ==================================================================== */

static void dispatch(char *line) {
    /* Lowercase the line */
    for (char *p = line; *p; ++p) {
        *p = (char)tolower((unsigned char)*p);
    }

    /* Tokenize */
    char *cmd = strtok(line, " \t");
    if (!cmd) return;

    char *arg1 = strtok(NULL, " \t");
    char *arg2 = strtok(NULL, " \t");
    char *arg3 = strtok(NULL, " \t");

    if      (!strcmp(cmd, "forward")  || !strcmp(cmd, "f")) cmdSetMode(RUN_FORWARD);
    else if (!strcmp(cmd, "backward") || !strcmp(cmd, "b")) cmdSetMode(RUN_BACKWARD);
    else if (!strcmp(cmd, "stop")     || !strcmp(cmd, "s")) cmdSetMode(RUN_STOPPED);
    else if (!strcmp(cmd, "pwm"))     cmdPwm(arg1);
    else if (!strcmp(cmd, "status"))  cmdStatus();
    else if (!strcmp(cmd, "hall"))    cmdHall();
    else if (!strcmp(cmd, "current")) cmdCurrent();
    else if (!strcmp(cmd, "hinv"))    cmdHinv(arg1);
    else if (!strcmp(cmd, "hmask"))   cmdHmask(arg1);
    else if (!strcmp(cmd, "offset"))  cmdOffset(arg1);
    else if (!strcmp(cmd, "map"))     cmdMap(arg1);
    else if (!strcmp(cmd, "limits"))  cmdLimits(arg1, arg2);
    else if (!strcmp(cmd, "uv"))      cmdUv(arg1, arg2, arg3);
    else if (!strcmp(cmd, "uven"))    cmdUven(arg1);
    else if (!strcmp(cmd, "gain"))    cmdGain(arg1);
    else if (!strcmp(cmd, "zeroi"))   cmdZeroi();
    else if (!strcmp(cmd, "clear"))   cmdClear();
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) cmdHelp();
    else {
        cliPrintln("Unknown. Type 'help'.");
    }
}

/* ====================================================================
 * Public API
 * ==================================================================== */

void CLI_Init(void) {
    linePos = 0;
    lastRxTick = HAL_GetTick();
    cmdHelp();
}

void CLI_Service(void) {
    uint8_t c;

    while (cliTransportRead(&c)) {
        lastRxTick = HAL_GetTick();

        if (c == '\r' || c == '\n') {
            if (linePos > 0) {
                lineBuf[linePos] = '\0';
                dispatch(lineBuf);
                linePos = 0;
            }
            continue;
        }

        if (c == 0x08 || c == 0x7F) {
            if (linePos > 0) --linePos;
            continue;
        }

        if (isprint(c) && linePos < (CLI_LINE_BUF - 1)) {
            lineBuf[linePos++] = (char)c;
        }
    }

    /* Idle timeout — auto-parse if no newline received */
    if (linePos > 0 && (HAL_GetTick() - lastRxTick) >= CLI_IDLE_PARSE_MS) {
        lineBuf[linePos] = '\0';
        dispatch(lineBuf);
        linePos = 0;
    }

}
