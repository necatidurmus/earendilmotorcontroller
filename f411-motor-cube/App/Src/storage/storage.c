/* ============================================================
 * App/Src/storage/storage.c
 * Flash storage for STM32F411 (Hall map + persistent config).
 *
 * SECTOR LAYOUT (last 128 KB sector at 0x08060000):
 *   Offset 0x0000:  Hall map record (14 bytes, HMAP v1)
 *   Offset 0x0100:  Config record area (append-only CFG2 slots)
 *                    Each slot = sizeof(PersistentConfigRecord_t).
 *                    On load, the sector is scanned for the latest
 *                    valid CFG2 record (highest sequence number).
 *                    When the area is full, the entire sector is
 *                    erased, hall map is rewritten, and a fresh
 *                    config record is written at offset 0x0100.
 *
 * SAFETY:
 *   - No 128 KB stack buffer; records are small (~80 bytes).
 *   - Caller must ensure motor is STOPPED before calling save/erase.
 *   - HAL_FLASH_Unlock/Lock are balanced in every path.
 *   - FNV-1a CRC32 on every config record.
 *   - When config area is full and sector erase is needed,
 *     the hall map at offset 0 is preserved by rewriting it.
 * ============================================================ */

#include "storage.h"
#include "app_config.h"
#include "stm32f4xx_hal.h"

#include <string.h>

/* ---- Flash geometry ---- */
#define FLASH_SECTOR_ADDR      0x08060000UL
#define FLASH_SECTOR_NUM       FLASH_SECTOR_7
#define FLASH_SECTOR_SIZE      (128U * 1024U)

#if defined(FLASH_SIZE) && (FLASH_SIZE < 0x80000UL)
#error "Storage address 0x08060000 requires STM32F411CE (512 KB flash)"
#endif

/* ---- Hall map record layout ---- */
#define MAP_OFFSET             0U
#define MAP_MAGIC              0x484D4150UL   /* "HMAP" */
#define MAP_VERSION            1U
/* Pad HMAP record to word (4-byte) boundary for flash programming.
 * 14 bytes data + 2 bytes padding = 16 bytes. */
#define MAP_RECORD_WORDS       4U
#define MAP_RECORD_Padded_SIZE (MAP_RECORD_WORDS * 4U)  /* 16 */

/* ---- Config record layout ---- */
#define CFG_AREA_OFFSET        0x0100U
#define CFG2_MAGIC             0x43464732UL   /* "CFG2" */
#define CFG2_VERSION           1U

/* ---- Config record on flash (packed, integer-scaled) ---- */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;

    int32_t  kp_milli;
    int32_t  ki_milli;

    uint16_t base_pwm[8];
    uint16_t boost_pwm[8];
    uint16_t boost_ms;

    int32_t  ramp_up_milli;
    int32_t  ramp_down_milli;

    uint16_t kick_duty;
    uint16_t kick_ms;
    uint16_t ramp_step;
    uint16_t ramp_interval_ms;
    uint16_t default_pwm;
    uint16_t brake_hold_ms;
    uint32_t telemetry_interval_ms;

    uint8_t  kick_enabled;
    uint8_t  ramp_enabled;
    uint8_t  reserved8[2];

    uint32_t crc32;
} Cfg2Record_t;

/* Word-alignment and size checks */
#ifndef static_assert
#define static_assert(cond, msg) _Static_assert(cond, msg)
#endif
static_assert(sizeof(Cfg2Record_t) <= 128U, "CFG2 record too large");
static_assert(sizeof(Cfg2Record_t) % 4U == 0U, "CFG2 record not word-aligned");

/* How many config records fit in the area. */
#define CFG_MAX_RECORDS  ((FLASH_SECTOR_SIZE - CFG_AREA_OFFSET) / sizeof(Cfg2Record_t))

/* ---- FNV-1a 32-bit hash ---- */
#define FNV_OFFSET_BASIS  0x811C9DC5UL
#define FNV_PRIME         0x01000193UL

static uint32_t fnv1a_32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t h = FNV_OFFSET_BASIS;
    for (uint32_t i = 0U; i < len; i++) {
        h ^= (uint32_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ---- Hall map checksum ---- */
static uint8_t calc_map_checksum(const uint8_t map[8])
{
    uint8_t x = 0x5A;
    for (uint8_t i = 0U; i < 8; i++)
        x ^= (uint8_t)(map[i] + (i * 17U));
    return x;
}

static bool validate_map(const uint8_t map[8])
{
    if (map[0] != 255U) return false;
    if (map[7] != 255U) return false;
    bool seen[6] = {false,false,false,false,false,false};
    uint8_t valid = 0;
    for (uint8_t i = 1U; i <= 6U; i++) {
        if (map[i] == 255U) continue;
        if (map[i] > 5U) return false;
        if (seen[map[i]]) return false;
        seen[map[i]] = true;
        valid++;
    }
    return (valid == 6U);
}

/* ---- Flash helpers ---- */

static const uint8_t *flash_ptr(uint32_t offset)
{
    return (const uint8_t *)(FLASH_SECTOR_ADDR + offset);
}

static bool flash_program_words(uint32_t dest_addr,
                                 const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i += 4U) {
        uint32_t word;
        memcpy(&word, buf + i, 4U);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              dest_addr + i, word) != HAL_OK) {
            return false;
        }
    }
    return true;
}

static bool flash_erase_sector(void)
{
    FLASH_EraseInitTypeDef erase;
    uint32_t sector_err = 0U;
    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    erase.Sector       = FLASH_SECTOR_NUM;
    erase.NbSectors    = 1U;
    return (HAL_FLASHEx_Erase(&erase, &sector_err) == HAL_OK)
           && (sector_err == 0xFFFFFFFFU);
}

/* ---- Flash size runtime guard ---- */
static bool flash_size_ok(void)
{
#if defined(FLASH_SIZE_DATA_REGISTER)
    uint16_t flash_kb = *(volatile uint16_t *)FLASH_SIZE_DATA_REGISTER;
    return (flash_kb >= 512U);
#else
    return true;
#endif
}

/* ---- Config record helpers ---- */

static bool is_cfg2_valid(const Cfg2Record_t *rec)
{
    if (rec->magic != CFG2_MAGIC) return false;
    if (rec->version != CFG2_VERSION) return false;
    if (rec->length != sizeof(Cfg2Record_t)) return false;
    /* Verify CRC: everything except the crc32 field itself. */
    uint32_t crc = fnv1a_32(rec, sizeof(Cfg2Record_t) - sizeof(uint32_t));
    return (crc == rec->crc32);
}

/* Find the latest valid config record and return its sequence.
 * Returns 0 if no valid record found. */
static uint32_t find_latest_sequence(void)
{
    uint32_t best_seq = 0U;
    const uint8_t *area = flash_ptr(CFG_AREA_OFFSET);
    for (uint32_t i = 0U; i < CFG_MAX_RECORDS; i++) {
        const Cfg2Record_t *rec =
            (const Cfg2Record_t *)(area + i * sizeof(Cfg2Record_t));
        if (is_cfg2_valid(rec) && rec->sequence > best_seq) {
            best_seq = rec->sequence;
        }
    }
    return best_seq;
}

/* Find the latest valid config record. Returns true if found. */
static bool find_latest_config(Cfg2Record_t *out)
{
    const Cfg2Record_t *best = NULL;
    uint32_t best_seq = 0U;
    const uint8_t *area = flash_ptr(CFG_AREA_OFFSET);

    for (uint32_t i = 0U; i < CFG_MAX_RECORDS; i++) {
        const Cfg2Record_t *rec =
            (const Cfg2Record_t *)(area + i * sizeof(Cfg2Record_t));
        if (is_cfg2_valid(rec) && rec->sequence > best_seq) {
            best = rec;
            best_seq = rec->sequence;
        }
    }

    if (best != NULL) {
        memcpy(out, best, sizeof(Cfg2Record_t));
        return true;
    }
    return false;
}

/* Convert a flash config record to a user-facing PersistentConfig_t. */
static void record_to_config(const Cfg2Record_t *rec, PersistentConfig_t *cfg)
{
    cfg->kp                   = (float)rec->kp_milli / 1000.0f;
    cfg->ki                   = (float)rec->ki_milli / 1000.0f;
    memcpy(cfg->base_pwm,   rec->base_pwm,   sizeof(cfg->base_pwm));
    memcpy(cfg->boost_pwm,  rec->boost_pwm,   sizeof(cfg->boost_pwm));
    cfg->boost_ms             = rec->boost_ms;
    cfg->ramp_up              = (float)rec->ramp_up_milli / 1000.0f;
    cfg->ramp_down            = (float)rec->ramp_down_milli / 1000.0f;
    cfg->kick_enabled         = (rec->kick_enabled != 0U);
    cfg->ramp_enabled         = (rec->ramp_enabled != 0U);
    cfg->kick_duty            = rec->kick_duty;
    cfg->kick_ms              = rec->kick_ms;
    cfg->ramp_step            = rec->ramp_step;
    cfg->ramp_interval_ms     = rec->ramp_interval_ms;
    cfg->default_pwm          = rec->default_pwm;
    cfg->brake_hold_ms        = rec->brake_hold_ms;
    cfg->telemetry_interval_ms = rec->telemetry_interval_ms;
}

/* Convert a user-facing PersistentConfig_t to a flash record. */
static void config_to_record(const PersistentConfig_t *cfg,
                               uint32_t sequence, Cfg2Record_t *rec)
{
    memset(rec, 0, sizeof(Cfg2Record_t));
    rec->magic           = CFG2_MAGIC;
    rec->version         = CFG2_VERSION;
    rec->length          = (uint16_t)sizeof(Cfg2Record_t);
    rec->sequence        = sequence;

    rec->kp_milli        = (int32_t)(cfg->kp * 1000.0f);
    rec->ki_milli        = (int32_t)(cfg->ki * 1000.0f);
    memcpy(rec->base_pwm,   cfg->base_pwm,   sizeof(rec->base_pwm));
    memcpy(rec->boost_pwm,  cfg->boost_pwm,   sizeof(rec->boost_pwm));
    rec->boost_ms        = cfg->boost_ms;
    rec->ramp_up_milli   = (int32_t)(cfg->ramp_up * 1000.0f);
    rec->ramp_down_milli = (int32_t)(cfg->ramp_down * 1000.0f);
    rec->kick_duty       = cfg->kick_duty;
    rec->kick_ms         = cfg->kick_ms;
    rec->ramp_step       = cfg->ramp_step;
    rec->ramp_interval_ms = cfg->ramp_interval_ms;
    rec->default_pwm     = cfg->default_pwm;
    rec->brake_hold_ms   = cfg->brake_hold_ms;
    rec->telemetry_interval_ms = cfg->telemetry_interval_ms;
    rec->kick_enabled    = cfg->kick_enabled ? 1U : 0U;
    rec->ramp_enabled    = cfg->ramp_enabled ? 1U : 0U;
    rec->reserved8[0]    = 0U;
    rec->reserved8[1]    = 0U;

    rec->crc32 = fnv1a_32(rec, sizeof(Cfg2Record_t) - sizeof(uint32_t));
}

/* Find the next free slot index in the config area.
 * Returns CFG_MAX_RECORDS if the area is full. */
static uint32_t find_next_free_slot(void)
{
    const uint8_t *area = flash_ptr(CFG_AREA_OFFSET);
    for (uint32_t i = 0U; i < CFG_MAX_RECORDS; i++) {
        const Cfg2Record_t *rec =
            (const Cfg2Record_t *)(area + i * sizeof(Cfg2Record_t));
        /* An erased flash word reads as 0xFFFFFFFF. */
        if (rec->magic == 0xFFFFFFFFUL) {
            return i;
        }
    }
    return CFG_MAX_RECORDS;  /* full */
}

/* ============================================================
 * Public API — Hall Map
 * ============================================================ */

bool Storage_LoadHallMap(uint8_t map[8])
{
    if (!flash_size_ok()) return false;

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

bool Storage_SaveHallMap(const uint8_t map[8])
{
    if (!flash_size_ok()) return false;
    if (!validate_map(map)) return false;

    /* Build HMAP record in RAM, padded to word boundary. */
    uint8_t record[MAP_RECORD_Padded_SIZE];
    memset(record, 0xFF, sizeof(record));
    uint32_t magic = MAP_MAGIC;
    memcpy(record, &magic, 4U);
    record[4] = MAP_VERSION;
    memcpy(record + 5, map, 8U);
    record[13] = calc_map_checksum(map);
    /* bytes 14,15 remain 0xFF (erased state) */

    /* Preserve existing config by reading it first. */
    Cfg2Record_t saved_cfg;
    bool have_cfg = find_latest_config(&saved_cfg);
    Cfg2Record_t new_cfg;
    if (have_cfg) {
        memcpy(&new_cfg, &saved_cfg, sizeof(Cfg2Record_t));
    }

    HAL_FLASH_Unlock();
    /* Clear any pending flash error flags before erase/program. */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    if (!flash_erase_sector()) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Write hall map at offset 0 (word-aligned, 16 bytes). */
    bool ok = flash_program_words(FLASH_SECTOR_ADDR + MAP_OFFSET,
                                   record, MAP_RECORD_Padded_SIZE);

    /* Rewrite config if we had one (preserve sequence). */
    if (ok && have_cfg) {
        new_cfg.crc32 = fnv1a_32(&new_cfg,
                                   sizeof(Cfg2Record_t) - sizeof(uint32_t));
        ok = flash_program_words(FLASH_SECTOR_ADDR + CFG_AREA_OFFSET,
                                  (const uint8_t *)&new_cfg,
                                  sizeof(Cfg2Record_t));
    }

    HAL_FLASH_Lock();
    return ok;
}

/* ============================================================
 * Public API — Config
 * ============================================================ */

bool Storage_LoadConfig(PersistentConfig_t *cfg)
{
    if (!flash_size_ok()) return false;

    Cfg2Record_t rec;
    if (!find_latest_config(&rec)) return false;

    record_to_config(&rec, cfg);
    return true;
}

bool Storage_SaveConfig(const PersistentConfig_t *cfg)
{
    if (!flash_size_ok()) return false;

    /* Find next free slot. */
    uint32_t slot = find_next_free_slot();

    /* If area is full, we need to erase and compact. */
    if (slot >= CFG_MAX_RECORDS) {
        /* Read current hall map. */
        uint8_t hall_map[8];
        bool have_map = Storage_LoadHallMap(hall_map);

        /* Build new config record. */
        Cfg2Record_t rec;
        uint32_t seq = find_latest_sequence() + 1U;
        config_to_record(cfg, seq, &rec);

        HAL_FLASH_Unlock();
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

        if (!flash_erase_sector()) {
            HAL_FLASH_Lock();
            return false;
        }

        /* Rewrite hall map first. */
        bool ok = true;
        if (have_map) {
            uint8_t map_record[MAP_RECORD_Padded_SIZE];
            memset(map_record, 0xFF, sizeof(map_record));
            uint32_t m = MAP_MAGIC;
            memcpy(map_record, &m, 4U);
            map_record[4] = MAP_VERSION;
            memcpy(map_record + 5, hall_map, 8U);
            map_record[13] = calc_map_checksum(hall_map);
            ok = flash_program_words(FLASH_SECTOR_ADDR + MAP_OFFSET,
                                      map_record, MAP_RECORD_Padded_SIZE);
        }

        /* Write config at first slot. */
        if (ok) {
            ok = flash_program_words(FLASH_SECTOR_ADDR + CFG_AREA_OFFSET,
                                      (const uint8_t *)&rec,
                                      sizeof(Cfg2Record_t));
        }

        HAL_FLASH_Lock();
        return ok;
    }

    /* There's room: write the new record at the next free slot. */
    uint32_t seq = find_latest_sequence() + 1U;
    Cfg2Record_t rec;
    config_to_record(cfg, seq, &rec);

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    bool ok = flash_program_words(FLASH_SECTOR_ADDR + CFG_AREA_OFFSET +
                                    slot * sizeof(Cfg2Record_t),
                                    (const uint8_t *)&rec,
                                    sizeof(Cfg2Record_t));

    HAL_FLASH_Lock();
    return ok;
}

bool Storage_EraseConfig(void)
{
    if (!flash_size_ok()) return false;

    /* Read current hall map to preserve it. */
    uint8_t hall_map[8];
    bool have_map = Storage_LoadHallMap(hall_map);

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR |
                           FLASH_FLAG_WRPERR | FLASH_FLAG_PGAERR |
                           FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

    if (!flash_erase_sector()) {
        HAL_FLASH_Lock();
        return false;
    }

    /* Rewrite hall map if it existed. */
    bool ok = true;
    if (have_map) {
        uint8_t map_record[MAP_RECORD_Padded_SIZE];
        memset(map_record, 0xFF, sizeof(map_record));
        uint32_t m = MAP_MAGIC;
        memcpy(map_record, &m, 4U);
        map_record[4] = MAP_VERSION;
        memcpy(map_record + 5, hall_map, 8U);
        map_record[13] = calc_map_checksum(hall_map);
        ok = flash_program_words(FLASH_SECTOR_ADDR + MAP_OFFSET,
                                  map_record, MAP_RECORD_Padded_SIZE);
    }

    HAL_FLASH_Lock();
    return ok;
}

bool Storage_HasValidConfig(void)
{
    if (!flash_size_ok()) return false;
    return (find_latest_sequence() > 0U);
}

/* ---- Legacy API wrappers ---- */

bool Storage_SaveConfigLegacy(uint16_t kickDuty, uint16_t kickMs,
                               uint16_t rampStep, uint16_t rampIntervalMs,
                               uint16_t defaultPwm, uint16_t brakeHoldMs)
{
    PersistentConfig_t cfg;
    /* Fill with current runtime values since legacy API is incomplete. */
    /* Load current from flash if any, then override the legacy fields. */
    if (!Storage_LoadConfig(&cfg)) {
        /* No saved config: use hard-coded defaults. */
        cfg.kp = DEFAULT_SPEED_KP;
        cfg.ki = DEFAULT_SPEED_KI;
        const uint16_t base_defaults[8] = {
            DEFAULT_BASE_PWM_1, DEFAULT_BASE_PWM_2, DEFAULT_BASE_PWM_3, DEFAULT_BASE_PWM_4,
            DEFAULT_BASE_PWM_5, DEFAULT_BASE_PWM_6, DEFAULT_BASE_PWM_7, DEFAULT_BASE_PWM_8
        };
        const uint16_t boost_defaults[8] = {
            DEFAULT_BOOST_PWM_1, DEFAULT_BOOST_PWM_2, DEFAULT_BOOST_PWM_3, DEFAULT_BOOST_PWM_4,
            DEFAULT_BOOST_PWM_5, DEFAULT_BOOST_PWM_6, DEFAULT_BOOST_PWM_7, DEFAULT_BOOST_PWM_8
        };
        memcpy(cfg.base_pwm, base_defaults, sizeof(cfg.base_pwm));
        memcpy(cfg.boost_pwm, boost_defaults, sizeof(cfg.boost_pwm));
        cfg.boost_ms = DEFAULT_BOOST_TIME_MS;
        cfg.ramp_up = DEFAULT_RAMP_UP_RPM_SEC;
        cfg.ramp_down = DEFAULT_RAMP_DOWN_RPM_SEC;
        cfg.kick_enabled = false;
        cfg.ramp_enabled = true;
        cfg.telemetry_interval_ms = TELEMETRY_INTERVAL_MS;
    }
    cfg.kick_duty = kickDuty;
    cfg.kick_ms = kickMs;
    cfg.ramp_step = rampStep;
    cfg.ramp_interval_ms = rampIntervalMs;
    cfg.default_pwm = defaultPwm;
    cfg.brake_hold_ms = brakeHoldMs;

    return Storage_SaveConfig(&cfg);
}

bool Storage_LoadConfigLegacy(uint16_t *kickDuty, uint16_t *kickMs,
                               uint16_t *rampStep, uint16_t *rampIntervalMs,
                               uint16_t *defaultPwm, uint16_t *brakeHoldMs)
{
    PersistentConfig_t cfg;
    if (!Storage_LoadConfig(&cfg)) return false;
    *kickDuty = cfg.kick_duty;
    *kickMs = cfg.kick_ms;
    *rampStep = cfg.ramp_step;
    *rampIntervalMs = cfg.ramp_interval_ms;
    *defaultPwm = cfg.default_pwm;
    *brakeHoldMs = cfg.brake_hold_ms;
    return true;
}
