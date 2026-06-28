# UART protocol — f411-motor-cube

The F411 command UART is **USART2 on PA2 (TX) / PA3 (RX), 115200 8N1**.
Lines are terminated with `\n` or `\r\n`. Replies are line-based and
prefixed with `\r\n`. Commands are case-insensitive. Unknown commands
return `[ERR] Unknown command`.

This protocol is what the **H7 upper controller** (`h7-main/src/motor_dispatcher.cpp`)
speaks to the F411, and what **`tools/terminal.py`** expects via the
H7 wheelbridge.

## Command source

* **From H7** (`MotorDispatcher::sendMotorCommand`):
  * speed mode: `rpm <signed>`
  * duty mode: `f<pwm>` / `b<pwm>` / `stop`
  * `stopAll()`: `rpm 0` then `stop`
  * `identifyAll()`: `identify`
  * arbitrary text: `sendTextCommand` / `sendToAll`
* **From direct FTDI / `tools/terminal.py`**: any command below, sent
  straight to the F411 UART (or relayed by the H7).

## Commands

### Motion / mode

| Command | Meaning |
|---------|---------|
| `mode` | show current mode |
| `mode duty` / `pid off` | manual duty mode (no PI) |
| `mode speed` / `pid on` | speed PI mode |
| `mode normal` | legacy alias for `mode duty` (ISSUE-037) |
| `mode control` | legacy alias for `mode speed` (ISSUE-037) |
| `f` / `forward` | run forward at `default_pwm` (set via `defpwm <n>`) |
| `b` / `backward` | run reverse at `default_pwm` |
| `f<n>` / `b<n>` | run with duty `n` (0..4000, clamped), forward / reverse |
| `s` / `stop` | coast (all gates off) |
| `x` / `brake` | **active brake** (all low-side MOSFETs ON, windings shorted). **Warning:** no current sense; use only with current-limited bench supply and at low speed. |
| `pwm` | query target/current duty |
| `pwm <n>` | set target duty (0..4000). Does NOT change bare `f`/`b` default. Use `defpwm <n>` to change bare `f`/`b` default, or `f<n>`/`b<n>` for specific duty. |

### Speed PI

| Command | Meaning |
|---------|---------|
| `rpm` | show target / ramped / measured RPM |
| `rpm <signed>` | set RPM target (clamped to ±500); `+` forward, `-` reverse, `0` stop. **Must be refreshed periodically** (see below) |
| `pi <kp> <ki>` | set both PI gains (clamped 0..10) |
| `kp <v>` / `ki <v>` | set one gain (compat) |
| `base <lo> <mid> <hi>` | feed-forward base PWM per RPM band (≤30 / ≤150 / >150), each 0..4000 |
| `boost <lo> <mid> <hi> <ms>` | start boost values (PWM 0..4000, ms 0..1000) |
| `ramp <up> <down>` | target ramp rates (RPM/s) |
| `spstat` | speed-PI status block |

### Duty-mode kick / ramp (ISSUE-038)

| Command | Meaning |
|---------|---------|
| `kick on` / `kick off` | enable / disable the startup kick pulse |
| `ramp on` / `ramp off` | enable / disable the duty ramp |
| `kickduty <n>` | kick pulse duty (0..4000) |
| `kickms <n>` | kick pulse duration ms (0..1000) |
| `ramprate <n>` | ramp step size (0..256) |
| `rampms <n>` | ramp step interval ms (0..1000) |
| `defpwm <n>` | default PWM for bare `f` / `b` (0..4000) |

### Diagnostics / config

| Command | Meaning |
|---------|---------|
| `hall` / `h` | raw + mapped Hall |
| `status` | full status block |
| `map` | show active Hall map with source/validity |
| `map validate` | validate active map |
| `map edit` | copy active map to candidate |
| `map set <raw> <sec>` | edit candidate entry (raw 0..7, sector 0..5 or `invalid`) |
| `map candidate` | show candidate map |
| `map apply` | apply candidate to active (validates first) |
| `map discard` | discard candidate |
| `map default` | load default compile-time map |
| `map load` / `reload` | load Hall map from flash |
| `map save` / `save` / `savecfg` / `saveall` | **disabled** — `[ERR] Persistent storage disabled in this build` |
| `loadcfg` | load duty-mode config (kick, ramp, default_pwm, brake_hold_ms) from flash. Motor must be stopped. |
| `defaults` | reset duty-mode config to safe bring-up defaults in RAM (kick OFF, ramp ON, default_pwm=100). Motor must be stopped. |
| `mapreset` | legacy alias for `map default` |
| `identify` | Hall-map identify routine (**requires service arming**) |
| `scan` | monitor Hall signals for 10 s |
| `test` | drive each of 6 sectors, report Hall (motor disconnected for scope test, **requires service arming**) |
| `gatetest <0-5> <1-4000>` | single-sector scope test, 100 ms timeout (motor disconnected only, requires gate arming) |
| `clrerr` | clear fault flags and force STOPPED (normally no longer required; motion commands auto-clear faults) |
| `debug on` / `debug off` | verbose debug toggle |
| `dbg on` / `dbg off` | telemetry debug format |
| `telper <ms>` | telemetry interval (20..5000 ms) |
| `help` / `?` | command help |

### Safety arming

Service commands (`identify`, `test`) require explicit arming:

```
arm service CURRENT_LIMITED_BENCH_SUPPLY
```

Arming expires after 30 seconds. `stop` / `estop` / fault resets
arming.

Gate test (`gatetest`) requires separate arming:

```
arm gatetest MOTOR_DISCONNECTED_I_UNDERSTAND
```

## Response format

* `[OK] ...`, `[ERR] ...`, `[INFO] ...`, `[WARN] ...` lines for
  command acknowledgements.
* `--- STATUS ---` / `--- SPEED PI STATUS ---` blocks for `status`
  and `spstat`.
* `[MAP] n n n n n n n n` for `map`.
* `[ID] step=<n> hall=<n> -> state=<n>` lines during `identify`.

## Telemetry

Default cadence 100 ms (`telper <ms>` to change).

### Compact (default)

```
RPM:<measured>,T:<target>,D:<duty>,DIR:<F|R|N>,APP_PH:<phase>,SP:<0|1>,BRAKE:<0|1>,FC:<fault>,H:<hall>,PWM_SET:<targetDuty>,PWM_ACT:<actualDuty>,QDROP:<cmdDrops>
```

| Field | Meaning |
|-------|---------|
| `RPM` | measured mechanical RPM |
| `T` | `|target rpm|` in speed mode, else 0 |
| `D` | current applied duty (0..4000) |
| `DIR` | `F` / `R` / `N` |
| `APP_PH` | app motor phase (0=STOP,1=RUN,2=BRAKE,3=NEUTRAL,4=FAULT) |
| `QDROP` | command queue overflow drop count |
| `SP` | 1 if speed (PI) mode, else 0 |
| `BRAKE` | 1 if brake phase, else 0 |
| `FC` | fault code (FaultManager) |
| `H` | raw Hall (0..7) |
| `PWM_SET` | target duty (0..4000) |
| `PWM_ACT` | actual applied duty (0..4000, never CCR ticks) |

### Debug (`dbg on`)

```
RPM:<m>,RF:<f>,Tcmd:<c>,Trmp:<r>,ERR:<e>,D:<d>,SPD_PH:<p>,FC:<c>,PWM_SET:<s>,PWM_ACT:<a>
```

## H7 wheelbridge compatibility

The H7 (`h7-main/src/main.cpp`, `updateWheelTelemetryBridge`) reads
each F411 motor UART line, strips `\r`, splits on `\n`, and prefixes
the line with the motor name before forwarding to the PC:

```
FL|RPM:12,T:10,D:34,DIR:F,APP_PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:34,PWM_ACT:34,QDROP:0
FR|...
RL|...
RR|...
```

`tools/terminal.py` (`_parse_telemetry`) splits on `|`, takes the
prefix (`FL`/`FR`/`RL`/`RR`), then parses the payload as
comma-separated `KEY:VALUE` pairs. Recognised keys: `RPM`, `T`,
`D`, `DIR`, `APP_PH`, `SP`, `BRAKE`, `FC`, `H`, `PWM_SET`, `PWM_ACT`,
`QDROP` (debug also: `RF`, `TCMD` (mapped to target), `TRMP`, `ERR`,
`SPD_PH`).

## Speed mode heartbeat requirement (ISSUE-020)

Both duty and speed modes require real command heartbeat:

* **Duty mode**: `f`, `b`, `f<n>`, `b<n>`, `pwm <n>` refresh the watchdog.
* **Speed mode**: `rpm <signed>` refreshes the watchdog.
* `mode speed` alone does NOT start the motor or refresh the watchdog.
* If no command refresh arrives within `CMD_WATCHDOG_MS` (800 ms),
  the motor stops/coasts and `FAULT_WATCHDOG` is raised.
* If the H7/terminal UART goes silent for `HOST_DISCONNECT_TIMEOUT_MS`
  (2000 ms), `FAULT_HOST_LOST` is raised.
* `FAULT_WATCHDOG` and `FAULT_HOST_LOST` are **not latched**; the next
  valid motion command clears them and resumes motion.

The H7 `motor_dispatcher` sends `rpm <signed>` periodically when a
speed target is active, which keeps the F411 alive. `tools/terminal.py`
also sends periodic commands when a speed slider is active.

## Fault handling (ISSUE-021)

When any fault occurs (NO_HALL, WATCHDOG, HOST_LOST, HW_BREAK,
INVALID_HALL, ILLEGAL_TRANSITION, ESTOP, etc.):

1. Motor outputs go to allOff/coast immediately.
2. SpeedPI is disabled.
3. Telemetry `FC` field shows the fault code.
4. Motion commands are **accepted**; the command clears the displayed
   fault, releases the safety lock, and resumes motion.
5. `clrerr` can still be used to manually clear the fault and force the
   app state to STOPPED.
6. Mode change (`mode duty` / `mode speed`) is rejected while motor
   is RUNNING or NEUTRAL. Stop the motor first.
7. Motion commands are rejected while a service/gate test is active.
   `stop` is always accepted.

## Examples

Direct FTDI to F411:

```
> mode speed
[OK] Mode=SPEED
> pi 0.6 0.0
[OK] Kp_m=600 Ki_m=0
> base 20 35 30
[OK] Base L=20 M=35 H=30
> boost 35 45 50 60
[OK] Boost L=35 M=45 H=50 ms=60
> ramp 30 100
[OK] Ramp up=30 down=100
> rpm 10
[OK] RPM=10
RPM:9,T:10,D:36,DIR:F,APP_PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:36,PWM_ACT:36,QDROP:0
> rpm -10
[OK] RPM=-10
> rpm 0
[OK] RPM=0 stop
> stop
[OK] Stop
> spstat
--- SPEED PI STATUS ---
...
> hall
[INFO] Hall=5 State=0
> status
--- STATUS ---
...
> clrerr
[OK] Errors cleared
```

Via H7 wheelbridge (PC sees):

```
FL|RPM:9,T:10,D:36,DIR:F,APP_PH:1,SP:1,BRAKE:0,FC:0,H:5,PWM_SET:36,PWM_ACT:36,QDROP:0
```
