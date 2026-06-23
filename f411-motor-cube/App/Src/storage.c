/* ============================================================
 * App/Src/storage.c
 * Flash storage for STM32F411 (Hall map + config).
 *
 * STATUS: write/save is DISABLED on purpose.
 *
 * The previous implementation read the whole 128 KB flash sector into
 * a stack buffer (`uint8_t sector_buf[128*1024]`) before erasing and
 * rewriting it.  That is a guaranteed stack overflow on the F411
 * (default stack 1 KB) — ISSUE-011.
 *
 * A safe implementation must either:
 *   - keep a static (static/.bss) sector buffer, or
 *   - use two alternating flash sectors (wear-levelling / append-only),
 *     or
 *   - mirror the small records (Hall map = 14 bytes, config = ~15
 *     bytes) in RAM and program only those words after erase.
 *
 * Until one of those is implemented, Storage_SaveHallMap() and
 * Storage_SaveConfig() return false without touching flash.  The
 * command handlers report "[ERR] Flash storage disabled until safe
 * implementation".
 *
 * Load is SAFE and kept enabled: it reads the small records directly
 * from flash memory (memory-mapped) with no large buffer.
 * ============================================================ */

#include "storage.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* Last sector of 512 KB flash (STM32F411CE) */
#define FLASH_SECTOR_ADDR   0x08060000UL

/* Hall map block at offset 0 */
#define MAP_OFFSET          0U
#define MAP_MAGIC           0x484D4150UL   /* "HMAP" */
#define MAP_VERSION         1U

/* Config block at offset 64 */
#define CFG_OFFSET          64U
#define CFG_MAGIC           0x43464731UL   /* "CFG1" */
#define CFG_VERSION         4U

/* ---- Checksum helpers (unchanged, used by load) ---- */

static uint8_t calc_map_checksum(const uint8_t map[8])
{
    uint8_t x = 0x5A;
    for (uint8_t i = 0; i < 8; i++)
        x ^= (uint8_t)(map[i] + (i * 17U));
    return x;
}

static uint8_t calc_cfg_checksum(uint8_t kickDuty, uint16_t kickMs,
                                  uint8_t rampStep, uint16_t rampIntervalMs,
                                  uint8_t defaultPwm, uint16_t brakeHoldMs)
{
    uint8_t x = 0x33;
    x ^= kickDuty;
    x ^= (uint8_t)(kickMs & 0xFF);
    x ^= (uint8_t)(kickMs >> 8);
    x ^= rampStep;
    x ^= (uint8_t)(rampIntervalMs & 0xFF);
    x ^= (uint8_t)(rampIntervalMs >> 8);
    x ^= defaultPwm;
    x ^= (uint8_t)(brakeHoldMs & 0xFF);
    x ^= (uint8_t)(brakeHoldMs >> 8);
    return x;
}

static bool validate_map(const uint8_t map[8])
{
    bool seen[6] = {false,false,false,false,false,false};
    uint8_t valid = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (map[i] == 255U) continue;
        if (map[i] > 5U) return false;
        if (seen[map[i]]) return false;
        seen[map[i]] = true;
        valid++;
    }
    return (valid == 6U);
}

static const uint8_t *flash_ptr(uint32_t offset)
{
    return (const uint8_t *)(FLASH_SECTOR_ADDR + offset);
}

/* ---- Public API ---- */

/* Save is disabled — see file header.  No 128 KB buffer is allocated. */
bool Storage_SaveHallMap(const uint8_t map[8])
{
    (void)map;
    return false;
}

bool Storage_SaveConfig(uint8_t kickDuty, uint16_t kickMs,
                        uint8_t rampStep, uint16_t rampIntervalMs,
                        uint8_t defaultPwm, uint16_t brakeHoldMs)
{
    (void)kickDuty; (void)kickMs; (void)rampStep; (void)rampIntervalMs;
    (void)defaultPwm; (void)brakeHoldMs;
    return false;
}

bool Storage_LoadHallMap(uint8_t map[8])
{
    const uint8_t *base = flash_ptr(MAP_OFFSET);
    uint32_t magic;
    memcpy(&magic, base, 4);
    if (magic != MAP_MAGIC) return false;

    uint8_t version = base[4];
    if (version != MAP_VERSION) return false;

    uint8_t stored_map[8];
    memcpy(stored_map, base + 5, 8);
    uint8_t cksum = base[13];

    if (cksum != calc_map_checksum(stored_map)) return false;
    if (!validate_map(stored_map)) return false;

    memcpy(map, stored_map, 8);
    return true;
}

bool Storage_LoadConfig(uint8_t *kickDuty, uint16_t *kickMs,
                        uint8_t *rampStep, uint16_t *rampIntervalMs,
                        uint8_t *defaultPwm, uint16_t *brakeHoldMs)
{
    const uint8_t *base = flash_ptr(CFG_OFFSET);
    uint32_t magic;
    memcpy(&magic, base, 4);
    if (magic != CFG_MAGIC) return false;

    uint8_t version = base[4];
    if (version != CFG_VERSION) return false;

    uint32_t off = 5;
    uint8_t  kd = base[off++];
    uint16_t km;  memcpy(&km, base + off, 2); off += 2;
    uint8_t  rs = base[off++];
    uint16_t ri;  memcpy(&ri, base + off, 2); off += 2;
    uint8_t  dp = base[off++];
    uint16_t bh;  memcpy(&bh, base + off, 2); off += 2;
    uint8_t  cksum = base[off];

    if (cksum != calc_cfg_checksum(kd, km, rs, ri, dp, bh)) return false;

    *kickDuty = kd;
    *kickMs = km;
    *rampStep = rs;
    *rampIntervalMs = ri;
    *defaultPwm = dp;
    *brakeHoldMs = bh;
    return true;
}
