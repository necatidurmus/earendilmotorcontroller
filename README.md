# BLDC Motor Driver Module

> STM32 Black Pill F411CE — Sensor-based 3-phase BLDC motor driver with UART control

---

## Project Summary

This project implements a single-motor BLDC driver module on an STM32F411CE (Black Pill) microcontroller. The driver uses 6-step commutation with Hall sensor feedback and accepts motion commands via UART.

The module is designed as a building block for a 4-motor skid-steer vehicle. Multiple modules connect to a hub STM32 (separate project) which multiplexes commands from a Python host.

**Current Status:** Prototype working with legacy WASD protocol. Transitioning to f/b/s motion protocol.

---

## Hardware Target

| Component | Specification |
|-----------|---------------|
| MCU | STM32F411CE (Black Pill V3) |
| Motor | 3-phase BLDC with 3 Hall sensors |
| Gate Drivers | 3× L6388ED013TR (half-bridge) |
| MOSFETs | 6× IRFB7730 N-channel (22Ω gate resistors) |
| Current Sense | 0.5mΩ shunt + INA181A1 (20V/V gain) → PA0 |
| Voltage Sense | Resistive divider (47kΩ/2.2kΩ) → PA4 |
| Hall Conditioning | Pull-up + series network (R1-R6, R10, R11) |
| Bus Capacitors | 2× 470µF bulk + 2× 100µF support |
| Bootstrap | 1µF caps + diodes per phase (D2-D4, C1-C3) |
| Regulation | L7805 (5V) + Black Pill onboard 3.3V |
| Protection | Fuse (J3) + TVS (D1) |
| Throttle | J1 connector (47kΩ/47kΩ/22nF filter) — hardware ready |
| Communication | UART 115200 baud (PA2 TX, PA3 RX) + USB Serial |
| PWM | 8-bit resolution, ~8kHz target frequency |

**Note:** Current sense (PA0) and voltage sense (PA4) are available on hardware but not yet used in firmware. Throttle input (J1) hardware is present but firmware usage is TBD.

---

## Software Components

| File | Purpose |
|------|---------|
| `src/main.cpp` | Firmware: motor control, commutation, CLI, telemetry |
| `tools/wasd_controller.py` | Python host: curses-based terminal UI (legacy, being replaced) |
| `platformio.ini` | Build configuration |
| `AGENTS.md` | Agent rules for AI-assisted development |
| `ISSUES.md` | Verified issues and false claim analysis |
| `ARCHITECTURE.md` | Technical architecture documentation |
| `ROADMAP.md` | Development roadmap (7 phases) |

---

## Current State

### What Works
- 6-step BLDC commutation with Hall sensor feedback
- Hall sensor debouncing and transition validation
- Kick (initial torque burst) and ramp (gradual duty increase)
- EEPROM-persistent hall map, config, and operating mode
- Three operating modes: Normal (CLI), Python (WASD), Settings (clean monitor)
- UART command processing via ring buffer and command queue
- RPM calculation from Hall sensor transitions
- Telemetry output (RPM, duty, direction, phase, PWM)
- Service tasks: scan, test, identify (hall map auto-detection)
- Software watchdog in Normal/Settings modes (800ms timeout)

### What Needs Work
- Python mode watchdog is disabled (safety risk)
- No hardware watchdog (IWDG)
- Command queue processes only 1 command per loop iteration
- Default PWM is hardcoded to 60 (should be configurable)
- Protocol is WASD-based (transitioning to f/b/s)
- No software dead-time in MOSFET switching

See [ISSUES.md](ISSUES.md) for full issue list.

---

## Control Flow

```
loop() {
  1. runMotorControlScheduler()    // 60µs motor tick (highest priority)
  2. uartDrainToRing()             // collect serial data into ring buffers
  3. processRxRingToLines()        // parse lines from ring buffers
  4. processQueuedCommands()       // execute commands (mode-dependent)
  5. updateServiceTask()           // handle scan/test/identify
  6. sendTelemetry()               // send status to host
  7. checkCommandWatchdog()        // failsafe (Normal/Settings only)
}
```

The motor control tick runs at 60µs intervals (~16.6kHz). It handles Hall sensor reading, duty ramping, and MOSFET commutation. All other tasks run in the main loop at whatever speed the loop completes.

---

## Motor State Machine

```
Stopped → (f/b command) → Kick → (kickMs timeout) → Running
Running → (direction change) → NeutralWait → Kick → Running
Any → (stop command / watchdog) → Stopped
Any → (hall fault / transition spam) → Fault
```

| Phase | Description | Duration |
|-------|-------------|----------|
| Stopped | All outputs off | Until command |
| Kick | High-torque startup burst | 120ms (configurable) |
| Running | Normal commutation with ramp | Until stop/fault |
| NeutralWait | Off, waiting for current decay | 80ms |
| Fault | Error state, outputs off | Until reset |

---

## UART Protocol

### Current (Legacy WASD)

| Command | Action |
|---------|--------|
| `w` | Forward (persistent) |
| `s` | Backward (persistent) |
| `x` | Stop |
| `d` | PWM +10 |
| `a` | PWM -10 |

### Target (f/b/s)

| Command | Action |
|---------|--------|
| `f` | Forward at default PWM |
| `f<duty>` | Forward at specified duty (0-255) |
| `b` | Backward at default PWM |
| `b<duty>` | Backward at specified duty |
| `s` | Stop |

**Lease semantics:** Each motion command refreshes a timestamp. Motor stops if no command received within 800ms.

### Telemetry

```
RPM:0,D:0,DIR:F,PH:2,PWM:150,PDIR:1,H:3
```

| Field | Meaning |
|-------|---------|
| RPM | Calculated revolutions per minute |
| D | Current duty cycle (0-255) |
| DIR | Direction (F=forward, R=reverse) |
| PH | Motor phase (0=Stopped, 1=Kick, 2=Running, 3=NeutralWait, 4=Fault) |
| PWM | Target/set PWM value |
| PDIR | Python direction (1=forward, -1=reverse, 0=stopped) |
| H | Raw Hall sensor value (1-6) |

---

## Safety / Failsafe

### Hardware Protection (Implemented)
- **Fuse (J3):** Board-level overcurrent protection
- **TVS diode (D1):** Transient voltage suppression
- **Gate resistors (22Ω):** Limits dV/dt, reduces ringing
- **L6388 internal dead-time:** Gate driver cross-conduction protection
- **Current sense (INA181A1):** 0.5mΩ shunt + 20V/V gain → PA0 ADC (hardware ready, firmware TBD)
- **Voltage sense:** 47kΩ/2.2kΩ divider → PA4 ADC (hardware ready, firmware TBD)

### Software Protection (Implemented)
- Software watchdog: 800ms timeout, stops motor if no command received (Normal/Settings modes)
- Hall timeout: faults if no valid Hall within 400ms of start
- Transition spam protection: faults if >20 invalid Hall transitions
- EEPROM validation: magic number + checksum on all stored data

### Missing
- **Hardware watchdog (IWDG):** Not implemented. If firmware hangs, motor continues running.
- **Python mode watchdog:** Disabled. If Python host crashes, motor runs indefinitely.
- **Software dead-time:** Not implemented. L6388 has internal dead-time, but software-level DT not added.
- **Host connection monitor:** Not implemented. Serial disconnect doesn't trigger stop.
- **Current-based protection:** Hardware ready (PA0), firmware not implemented.
- **Voltage monitoring:** Hardware ready (PA4), firmware not implemented.

---

## Known Limitations

1. **Single motor only.** Current code controls one motor. 4-motor support requires hub STM32 (separate project).
2. **WASD protocol legacy.** Python host currently uses WASD commands. Transitioning to f/b/s.
3. **No hardware watchdog.** Firmware hang = motor continues running.
4. **Hardcoded pin mapping.** Different board layouts require code changes.
5. **8kHz PWM not guaranteed.** `analogWriteFrequency()` support depends on STM32 core implementation.
6. **Current/voltage sense unused.** Hardware ready (INA181A1, divider), firmware not utilizing.
7. **Throttle unused.** Hardware present (J1), firmware not connected.

See [HARDWARE_OVERVIEW.md](HARDWARE_OVERVIEW.md) for complete hardware documentation.

---

## Development Notes

### Build

```bash
# Using PlatformIO
pio run                    # build
pio run -t upload          # flash
pio device monitor         # serial monitor
```

### Serial Connection

```bash
# USB Serial (direct)
pio device monitor -b 115200

# FTDI adapter
# Connect FTDI TX → PA3 (RX), FTDI RX → PA2 (TX), GND → GND
# Then use any serial terminal at 115200 baud
```

### CLI Commands (Normal Mode)

```
f / forward         — run forward (default PWM)
f<duty>             — run forward at duty (0-255)
b / backward        — run backward (default PWM)
b<duty>             — run backward at duty
s / stop            — stop motor
pwm                 — show current PWM
pwm <0-255>         — set PWM
kick on/off         — enable/disable kick
ramp on/off         — enable/disable ramp
kickduty <n>        — set kick duty
kickms <n>          — set kick duration (ms)
ramprate <n>        — set ramp step
rampms <n>          — set ramp interval (ms)
savecfg             — save config to EEPROM
loadcfg             — load config from EEPROM
defaults            — load default config
saveall             — save all to EEPROM
hall                — read Hall sensors
map                 — show Hall map
save                — save Hall map
reload              — reload Hall map
mapreset            — reset Hall map to default
scan                — scan Hall signals (10s)
test                — test each commutation step
identify            — auto-detect Hall map
status              — show full status
debug on/off        — toggle verbose debug
clrerr              — clear error flags
mode                — show current mode
mode python         — switch to Python mode
mode settings       — switch to Settings mode
mode normal         — switch to Normal mode
```

---

## File Structure

```
asenkroncode/
├── src/
│   └── main.cpp              — firmware (all motor logic)
├── tools/
│   └── wasd_controller.py    — Python host (legacy WASD UI)
├── docs/
│   ├── README.md             — project documentation
│   ├── ARCHITECTURE.md       — technical architecture
│   ├── ROADMAP.md            — development roadmap
│   ├── ISSUES.md             — verified issues
│   └── problems.md           — legacy issue report
├── platformio.ini            — build config
├── AGENTS.md                 — agent rules
└── README.md                 — quick links to docs
```

---

## Future Goals

1. **Phase 0-1:** Stabilize code, implement f/b/s protocol
2. **Phase 2-3:** FTDI test, Python host rewrite
3. **Phase 4:** Hardware watchdog, safety layers
4. **Phase 5:** Hub STM32 integration preparation
5. **Phase 6:** 4-motor skid-steer vehicle

See [ROADMAP.md](ROADMAP.md) for detailed phase plans.

---

## Warnings

- **Safety:** This code controls a motor. Always have a physical emergency stop (power disconnect) available during testing.
- **Dead-time:** L6388 gate drivers have internal dead-time, but software-level dead-time is not implemented. Test on bench with current-limited power supply.
- **Watchdog:** Python mode has no watchdog. If the Python host crashes, the motor will continue running.
- **Prototype:** This is a prototype. Not suitable for production use without further testing and safety hardening.
- **Current sense:** INA181A1 hardware is ready but not used in firmware. No overcurrent protection in software.

---

## Documentation

| Document | Purpose |
|----------|---------|
| [ISSUES.md](ISSUES.md) | Verified issues, false claims, evidence-based analysis |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Technical architecture, protocol, state machine, safety |
| [HARDWARE_OVERVIEW.md](HARDWARE_OVERVIEW.md) | Complete hardware documentation, pin map, components |
| [ROADMAP.md](ROADMAP.md) | 8-phase development plan with tasks, risks, test items |
| [AGENTS.md](../AGENTS.md) | Agent rules for AI-assisted development |
