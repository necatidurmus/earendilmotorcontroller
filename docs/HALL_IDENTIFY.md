# Hall Map & Identify — f411-motor-cube

## What is a Hall map?

A Hall map translates the 3-bit raw Hall sensor code (0..7) to the
electrical commutation sector (0..5). Each BLDC motor/hall-sensor
combination has a unique mapping depending on physical wiring.

The firmware uses an 8-entry lookup table:
`map[raw_hall_code] -> electrical_sector`

### Why raw 0 and 7 are invalid

With three Hall sensors (A, B, C), raw code 0 (0b000 = all low) and
raw code 7 (0b111 = all high) are physically impossible during normal
rotation. They indicate a wiring fault, disconnected sensor, or noise.
These entries must always be mapped to `255` (invalid).

### Sectors 0..5

The 6-step commutation uses 6 electrical sectors (0..5). Each sector
defines which motor phase is driven high-side (PWM) and which is
driven low-side (forced-on). The sector sequence depends on the
physical motor wiring.

## Default map

The compile-time default map (from `app_config.h`) is:

```
raw:    0   1   2   3   4   5   6   7
state: 255   1   3   2   5   0   4   255
```

This was copied from the legacy Arduino firmware. If your motor's
wiring differs, you must re-derive the map with `identify`.

## What `identify` does

`identify` is an automated routine that:

1. Energizes each of 6 electrical sectors in sequence (toggling
   between adjacent sectors).
2. After settling, reads the Hall sensor raw code.
3. Maps each observed raw code to the sector that was applied.
4. Builds a candidate map.
5. Validates the candidate:
   - raw 0 and 7 must be 255
   - raw 1..6 must each map to a unique sector 0..5
   - no duplicate sectors allowed
   - no missing sectors allowed
6. If valid: applies the candidate to the active RAM map.
7. If invalid: rejects the candidate and leaves the active map
   unchanged.

### Why `identify` requires a current-limited bench supply

`identify` **energizes motor phases** to determine the Hall mapping.
Without current sensing, a stalled motor or incorrect wiring could
draw excessive current. The service arming mechanism ensures the
operator acknowledges the risk.

### Why `identify` does NOT auto-save

An incorrect test environment (motor under load, wrong wiring, noisy
Hall signals) could produce a bad map. Saving a bad map to flash
would make it persist across resets. The operator must explicitly
verify the map and save it.

## Validation rules

The `Commutation_ValidateHallMap()` function checks:

| Rule | Error code |
|------|-----------|
| `map[0]` must be 255 | `raw0_not_invalid` |
| `map[7]` must be 255 | `raw7_not_invalid` |
| `map[1]..map[6]` must be 0..5 | `sector_out_of_range` |
| Each sector 0..5 appears exactly once | `duplicate_sector` / `missing_sector` |
| Map pointer must not be NULL | `null_map` |

## Candidate map workflow

To safely edit a Hall map without disrupting the active map:

1. `map edit` — copies the active map to a candidate buffer.
2. `map set <raw> <sector>` — modifies an entry in the candidate.
3. `map candidate` — inspects the candidate map.
4. `map validate` — checks the candidate (or active) map.
5. `map apply` — if the candidate is valid, atomically replaces the
   active map.
6. `map discard` — throws away the candidate.

The active map is never modified during editing. Only `map apply`
(or `map default` / `identify`) changes the active map.

## Map source tracking

The firmware tracks where the active map came from:

| Source | Meaning |
|--------|---------|
| `DEFAULT` | Compile-time default, never changed |
| `RAM_IDENTIFY` | Produced by `identify` |
| `RAM_MANUAL` | Manually edited via `map apply` |
| `FLASH` | Loaded from flash storage |

Use `status` to see the current map source and validity.

## Symptoms of a wrong Hall map

- Motor vibrates instead of spinning
- High current at startup (PSU current-limit trips)
- Motor struggles in one direction
- Uneven RPM or jerky rotation
- Frequent `FAULT_INVALID_HALL` or `FAULT_ILLEGAL_TRANSITION`
- Motor spins but torque is weak

## Safe test sequence

1. `status` — check firmware state, safety info, map validity.
2. `hall` — verify Hall sensors are producing valid codes when you
   rotate the wheel by hand.
3. `map` — inspect the current active map.
4. `map validate` — confirm the map passes all checks.
5. `arm service CURRENT_LIMITED_BENCH_SUPPLY` — arm for identify.
6. `identify` — run the identification routine.
7. `map` — inspect the new map.
8. Low-duty test: `f10`, watch motor behavior, check PSU current.
9. `map save` — only after verification, and only if storage is
   enabled (currently disabled in this build).

## Map commands reference

| Command | Description |
|---------|-------------|
| `map` | Show active map with source and validity |
| `map validate` | Validate active map |
| `map edit` | Copy active map to candidate |
| `map set <raw> <sec>` | Edit candidate entry (raw 0..7, sector 0..5 or `invalid`) |
| `map candidate` | Show candidate map |
| `map apply` | Apply candidate to active (validates first) |
| `map discard` | Discard candidate |
| `map default` | Load default compile-time map |
| `map load` | Load from flash |
| `map save` | Save to flash (currently disabled) |
| `mapreset` | Legacy alias for `map default` |
| `reload` | Legacy alias for `map load` |
