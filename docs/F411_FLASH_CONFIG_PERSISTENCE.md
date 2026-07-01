# F411 Flash Config Persistence

## Overview

STM32F411 has no hardware EEPROM. The last 128 KB flash sector (0x08060000) is used as persistent storage for:

1. **Hall map** — offset 0x0000, 14 bytes + 2 pad = 16 bytes (HMAP v1)
2. **Config records** — offset 0x0100, append-only CFG2 records with sequence numbers

## Storage Layout

```
0x08060000  +0x0000  HMAP record (16 bytes padded)
            +0x0100  CFG2 record slots (append-only)
                     Each CFG2 record = 80 bytes
                     Max records = (128K - 256 - 64) / 80 = ~1630
            ...
            +0x1FFFC End of sector
```

### CFG2 Record Format

| Field | Size | Description |
|-------|------|-------------|
| magic | 4 | 0x43464732 ("CFG2") |
| version | 2 | 1 |
| length | 2 | sizeof(Cfg2Record_t) |
| sequence | 4 | Monotonically increasing |
| kp_milli | 4 | Kp * 1000 (integer-scaled float) |
| ki_milli | 4 | Ki * 1000 |
| base_pwm[8] | 16 | Base PWM band table |
| boost_pwm[8] | 16 | Boost PWM band table |
| boost_ms | 2 | Boost duration ms |
| ramp_up_milli | 4 | ramp_up * 1000 (RPM/sec) |
| ramp_down_milli | 4 | ramp_down * 1000 |
| kick_duty | 2 | Kick duty (0..4000) |
| kick_ms | 2 | Kick duration ms |
| ramp_step | 2 | Duty ramp step |
| ramp_interval_ms | 2 | Duty ramp interval ms |
| default_pwm | 2 | Default start PWM |
| brake_hold_ms | 2 | Active brake hold time |
| telemetry_interval_ms | 4 | Telemetry output period ms |
| kick_enabled | 1 | 0 or 1 |
| ramp_enabled | 1 | 0 or 1 |
| reserved | 2 | Padding/alignment |
| crc32 | 4 | FNV-1a over all preceding bytes |

Total: 80 bytes (word-aligned).

## Safety Design

- **No 128 KB stack buffer** — records are small (~80 bytes), programmed word by word directly to flash.
- **Motor must be STOPPED** before any save/erase operation. `savecfg`, `loadcfg`, `erasecfg` all reject if motor is running.
- **No auto-save** — flash write only happens on explicit `savecfg`/`save`/`saveall` command.
- **Append-only** — each `savecfg` writes a new record; old records remain until area is full.
- **Hall map preservation** — when sector erase is needed (area full or explicit erase), the current hall map is re-read and rewritten first.
- **FNV-1a CRC32** on every record; load scans for the latest valid record.
- **Sequence numbers** — each save increments the sequence; load picks the highest valid sequence.
- **Float→int scaling** — kp, ki, ramp_up, ramp_down stored as `value * 1000` integers to avoid IEEE 754 portability issues.
- **Flash error flags** cleared before each erase/program operation.
- **HAL_FLASH_Unlock/Lock** are balanced in every code path.
- **FLASH_SIZE_DATA_REGISTER** runtime guard prevents access on sub-512 KB parts.

## Commands

| Command | Description |
|---------|-------------|
| `savecfg`, `save`, `saveall` | Snapshot runtime config and save to flash |
| `loadcfg` | Load config from flash into runtime |
| `erasecfg` | Erase all config records from flash |
| `cfg` | Display current RAM config + flash status (VALID/EMPTY) + saved sequence number |
| `defaults` | Reset all config to defaults (RAM only, no auto-save) |
| `map save` | Save hall map to flash (now enabled) |

## Test Procedure

### Test 1 — Default Boot

```
status
cfg
```

Expected: `[INFO] No valid config in Flash — defaults active` at boot. `cfg` shows default values. `Flash: EMPTY seq=0`.

### Test 2 — Tuning

```
mode speed
pi 0.8 0.05
base 640 660 680 700 720 700 670 640
boost 880 900 920 940 960 990 1020 1040 150
ramp 60 150
kick on
kickduty 960
kickms 50
ramprate 128
rampms 5
defpwm 1600
telper 100
cfg
```

Expected: `cfg` shows all tuned values. `Flash: EMPTY` (not yet saved).

### Test 3 — Save

```
stop
savecfg
```

Expected: `[OK] Config saved to Flash seq=1`. Motor must be stopped first. Post-write verify-read succeeds.

### Test 4 — Reset / Power-Cycle

Reset or power-cycle the board.

### Test 5 — Verify

```
cfg
status
```

Expected:
- Kp/Ki unchanged from test 2 values
- base[8] and boost[8] tables unchanged
- boost_ms unchanged
- ramp up/down unchanged
- kick ON, kickduty, kickms unchanged
- ramprate/rampms/default_pwm unchanged
- telper unchanged
- `Flash: VALID seq=1`

### Test 6 — Load After Defaults

```
defaults
cfg
loadcfg
cfg
```

Expected:
- `defaults` resets RAM to default values
- `cfg` after defaults shows default Kp/Ki, base, boost, etc.
- `loadcfg` restores flash values (`[OK] Config loaded from Flash seq=1`)
- `cfg` after loadcfg matches test 2 values

### Test 7 — Erase

```
erasecfg
reset
cfg
```

Expected:
- `erasecfg` clears config records from flash (`[OK] Config erased from Flash — runtime unchanged`)
- After reset, `[INFO] No valid config in Flash — defaults active`
- `cfg` shows defaults, `Flash: EMPTY seq=0`

### Test 8 — Config Save While Motor Running (rejection)

```
f500
savecfg
```

Expected: `[ERR] Stop motor first`

### Test 9 — Hall Map Preservation

```
map save
erasecfg
map
```

Expected: Hall map is still loaded from flash after `erasecfg` (it was preserved during sector erase).

## Wear Considerations

- Each `savecfg` writes one ~80 byte record.
- 128 KB sector / 80 bytes per record = ~1630 saves before compaction.
- Compaction erases the sector once, rewriting hall map + latest config.
- Typical flash endurance: 10K–30K erase cycles per sector.
- At 10 saves/day, sector life > 40 years.

## Float-Integer Conversion

| Runtime | Flash | Conversion |
|---------|-------|------------|
| kp (float) | kp_milli (int32) | store: (int32_t)(kp * 1000) |
| | | load: kp = kp_milli / 1000.0f |
| ki (float) | ki_milli (int32) | same |
| ramp_up (float, RPM/sec) | ramp_up_milli (int32) | same |
| ramp_down (float, RPM/sec) | ramp_down_milli (int32) | same |

This avoids IEEE 754 float representation issues across toolchains.
