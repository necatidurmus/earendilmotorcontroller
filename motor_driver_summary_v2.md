# Custom BLDC Motor Driver Board Summary

## 1. Overview

This document summarizes the custom **STM32F411 Black Pill based sensored BLDC motor driver board** using the shared:

- PCB screenshot
- schematic screenshot
- working test firmware pin mapping
- BOM table provided later in the conversation

Where there is a conflict between drawing interpretation and software behavior, the **tested firmware pin mapping is treated as the active bring-up reference**.

This board is a **3-phase sensored BLDC inverter / driver board** built around:

- **STM32F411 Black Pill V3**
- **3 × L6388ED013TR** half-bridge gate drivers
- **6 × external N-channel MOSFETs**
- **Hall sensor input header**
- **Current sensing with INA181 + 0.5 mΩ shunt**
- **Voltage sensing**
- **Throttle header**
- **On-board 5 V regulator**

---

## 2. System Architecture

Main functional blocks:

1. **MCU block**
   - WeAct / Black Pill V3 STM32F411CE-class module

2. **Gate driver block**
   - 3 × L6388ED013TR
   - one driver per phase leg

3. **Power stage**
   - 6 × N-channel MOSFET full 3-phase bridge
   - gate resistors
   - DC bus capacitors
   - fuse
   - TVS protection

4. **Hall block**
   - 5-pin hall connector
   - pull-up + series conditioning network

5. **Current sense**
   - low-side shunt resistor
   - INA181 current-sense amplifier
   - analog output to MCU

6. **Voltage regulation**
   - main DC input
   - L7805 linear regulator for 5 V rail

7. **Throttle input**
   - dedicated connector and RC/resistor network

---

## 3. MCU Module

### 3.1 Module
- **Module name:** Black Pill V3 STM32F411
- **BOM line:** `MCU1 = BLACK PILL V3 STM32F411`
- **Manufacturer in BOM:** WeAct Studio
- **Supplier reference in BOM:** Adafruit 4877

### 3.2 MCU responsibilities
The MCU currently handles:

- hall reading
- 6-step commutation
- high-side PWM generation
- low-side digital phase selection
- ADC reading
- USB CDC serial bring-up and testing

---

## 4. Confirmed Pin Mapping

The following mapping is taken from the working code used to rotate the motor.

## 4.1 Hall Inputs

| Function | MCU Pin | Notes |
|---|---:|---|
| Hall A | **PB6** | digital input |
| Hall B | **PB7** | digital input |
| Hall C | **PB8** | digital input |

### Hall encoding
The firmware forms the hall word as:

```c
hall = A + 2*B + 4*C
```

So:

- bit0 = Hall A
- bit1 = Hall B
- bit2 = Hall C

Valid hall states are expected to be:

- `001`
- `101`
- `100`
- `110`
- `010`
- `011`

Invalid states:

- `000`
- `111`

---

## 4.2 Gate Driver Control Pins

### High-side controls

| Function | MCU Pin | Role |
|---|---:|---|
| CTRL_AH | **PA8** | phase A high-side PWM/control |
| CTRL_BH | **PA9** | phase B high-side PWM/control |
| CTRL_CH | **PA10** | phase C high-side PWM/control |

### Low-side controls

| Function | MCU Pin | Role |
|---|---:|---|
| CTRL_AL | **PA7** | phase A low-side control |
| CTRL_BL | **PB0** | phase B low-side control |
| CTRL_CL | **PB1** | phase C low-side control |

### Current bring-up drive style
Current working code uses:

- **high-side = PWM**
- **low-side = static ON/OFF**
- third phase floating

This is a **sensored 6-step asynchronous / non-synchronous** drive style.

---

## 4.3 Analog Inputs

| Function | MCU Pin | Notes |
|---|---:|---|
| ISENSE | **PA0** | INA181 output |
| VSENSE | **PA4** | bus voltage sense |
| Throttle | **A1 in schematic context** | not yet used in tested firmware |

### Note on throttle
The schematic clearly includes a throttle block, but the working motor firmware currently uses **USB CDC serial commands** instead of the throttle input.

So throttle exists in hardware, but final software validation of its ADC pin is still pending.

---

## 4.4 Communication

| Interface | Pin / Path | Notes |
|---|---:|---|
| UART TX | **PA2** | earlier discussed mapping |
| UART RX | **PA3** | earlier discussed mapping |
| USB CDC | onboard Type-C | used successfully for testing |

### Current practical test interface
The board has already been tested using:

- **USB CDC via the Black Pill Type-C port**

Used for:

- forward / backward / stop
- PWM changes
- hall printout
- ADC printout

---

## 5. Gate Driver Stage

## 5.1 Gate driver ICs
- **Part:** L6388ED013TR
- **Quantity:** 3
- **Designators:** `U8, U9, U10`

These are **half-bridge high/low-side gate drivers**, one for each motor phase leg.

## 5.2 Driver mapping by phase
From schematic labeling:

- one driver for phase A
- one driver for phase B
- one driver for phase C

Driver inputs are labeled with control nets:

- `CTRL_AL`, `CTRL_AH`
- `CTRL_BL`, `CTRL_BH`
- `CTRL_CL`, `CTRL_CH`

Driver outputs go to gate-drive nets:

- `DRV_AH`, `DRV_AL`
- `DRV_BH`, `DRV_BL`
- `DRV_CH`, `DRV_CL`

## 5.3 Bootstrap support
Each high-side driver includes:

- bootstrap diode
- bootstrap capacitor

### BOM-related parts
- `D2, D3, D4` = SOD-123 diodes
- `C1, C2, C3` = 1 µF capacitors

These are consistent with the visible bootstrap/support network around the gate drivers.

---

## 6. Power Stage

## 6.1 Bridge topology
The board uses a classic **3-phase bridge inverter**:

- 3 high-side MOSFETs
- 3 low-side MOSFETs

Motor phase outputs are labeled:

- **COM_A**
- **COM_B**
- **COM_C**

## 6.2 MOSFET population
- **Q1, Q2, Q3, Q4, Q5, Q6**
- TO-220 flat package footprint
- BOM entry is generic and does **not** reliably identify the exact fitted MOSFET

### Important note
You previously mentioned **IRFB7730** in discussion, but the BOM line is generic.  
Therefore the documentation should treat the MOSFET line as:

- **6 external N-channel MOSFETs**
- **exact installed part number should be confirmed from the physical board or sourcing record**

## 6.3 Gate resistors
- **R14, R15, R16, R17, R18, R19 = 22 Ω**
- one per MOSFET gate path

## 6.4 Bus capacitor network

### Bulk electrolytics
- **C4, C5 = 470 µF**
- radial polarized capacitors

### Additional decoupling / support capacitors
- **C11, C12, C15, C16, C17, C18, C19, C20 = 1 µF**
- **C21, C22 = 100 µF electrolytic**
- **C9 = 22 nF**

## 6.5 Protection and input items
- **J3 = fuse footprint / automotive-style fuse**
- **D1 = TVS diode**
- **U15 = 2-pin power input**

---

## 7. Current Sense Block

## 7.1 Architecture
The board uses:

- **low-side shunt resistor**
- **INA181 current-sense amplifier**
- output routed to MCU ADC pin **PA0**

## 7.2 Components

| Component | Value / Part | Designator | Notes |
|---|---|---|---|
| Shunt resistor | **0.0005 Ω** | `R9` | 0.5 mΩ, 2512 |
| Current amplifier | **INA181** | `U2` | exact gain suffix still unknown |
| ADC input | **PA0** | - | ISENSE input |

## 7.3 Current sense equations

### Shunt voltage
\[
V_{shunt} = I \times R_{shunt}
\]

With:
- \(R_{shunt} = 0.0005\ \Omega\)

Examples:

- 1 A → 0.5 mV
- 2 A → 1.0 mV
- 2.5 A → 1.25 mV
- 10 A → 5.0 mV

### Amplified output
\[
V_{out} = Gain \times V_{shunt}
\]

### INA181 gain variants
The BOM says only `INA181`, not the suffix, so gain is still unknown.

Possible variants:

| Variant | Gain |
|---|---:|
| INA181A1 | 20 V/V |
| INA181A2 | 50 V/V |
| INA181A3 | 100 V/V |
| INA181A4 | 200 V/V |

### ADC reference note
For 3.3 V / 12-bit ADC:

\[
LSB \approx 3.3 / 4095 \approx 0.806\ \text{mV}
\]

So current measurement resolution depends heavily on the exact INA181 gain suffix.

## 7.4 Firmware status
The current tested code:

- **reads ISENSE**
- can **print ISENSE ADC**
- does **not** yet implement:
  - soft current limiting
  - hard overcurrent trip
  - current-mode control

---

## 8. Voltage Sense Block

## 8.1 Signal path
- **VSENSE** is routed to **PA4**

## 8.2 Divider / conditioning components
From schematic + BOM correlation, the voltage sense area includes:

- `R12 = 47 kΩ`
- `R13 = 2.2 kΩ`
- nearby filtering capacitors

### Important note
The exact final conversion ratio should still be verified against the original design file or measured board before relying on absolute voltage readings in firmware.

---

## 9. Hall Input Block

## 9.1 Connector
- **U14 = Header5**

Likely hall connector carrying:

- Hall A
- Hall B
- Hall C
- 5 V
- GND

## 9.2 Conditioning network
The hall input block includes:

- pull-up resistors to 5 V
- series resistors into the MCU-side nets

From BOM:

- `R1, R2, R3 = 2.2 kΩ`
- `R4, R5, R6 = 2.2 kΩ`
- `R10, R11 = 47 kΩ`

Based on schematic layout, these belong to the hall conditioning/pull network.

### Practical implication
The motor hall sensors are expected to interface through a protected/conditioned network rather than directly into MCU pins.

---

## 10. Throttle Block

## 10.1 Connector
- **J1 = 4-pin throttle connector**

## 10.2 Associated passive components
The throttle section includes:

- `R7 = 47 kΩ`
- `R8 = 47 kΩ`
- `C9 = 22 nF`

This is consistent with a filtered analog throttle input stage.

## 10.3 Status
Hardware exists, but the currently working BLDC test firmware does not yet use it for speed command input.

---

## 11. Voltage Regulation

## 11.1 Power input
- **U15 = POWER2-1**
- two-pin main power connector

## 11.2 Regulator
- **U11 = L7805**

This generates the **5 V rail** from the main input bus.

## 11.3 5 V rail support capacitors
From BOM:

- `C21, C22 = 100 µF`
- `C15, C16 = 1 µF`

These match the regulator support / local rail decoupling area.

## 11.4 Rail overview
Visible rails include:

- raw DC bus
- 5 V logic rail
- 3.3 V logic rail associated with MCU module

---

## 12. External Connections

## 12.1 Motor phase outputs
Large output pads:

- **COM_A**
- **COM_B**
- **COM_C**

## 12.2 Hall header
- 5-pin hall connector

## 12.3 Throttle header
- 4-pin throttle connector

## 12.4 Main power input
- 2-pin input connector

## 12.5 Fuse
- one external / automotive fuse position

---

## 13. Present Firmware Behavior Summary

The shared tested code currently implements a **basic sensored 6-step bring-up controller**.

### Features currently used
- hall-based commutation
- USB CDC serial control
- forward / backward / stop
- PWM manual stepping
- hall state print
- VSENSE / ISENSE ADC print

### Example command set
- `f` → forward
- `b` → backward
- `s` → stop
- `+` → PWM up
- `-` → PWM down
- `h` → print hall state
- `p` → print VSENSE / ISENSE ADC
- `?` → help

### Current firmware limitations
This is **bring-up code**, not final production firmware.

Not yet implemented:

- active current limit
- overcurrent cutoff
- undervoltage protection in code
- thermal derating
- automatic hall mapping calibration
- FOC / sinusoidal control
- advanced synchronous drive logic

---

## 14. 6-Step Commutation Summary

Current conceptual state table:

| State | Low-side ON | High-side PWM |
|---:|---|---|
| 0 | A low | B high |
| 1 | A low | C high |
| 2 | B low | C high |
| 3 | B low | A high |
| 4 | C low | A high |
| 5 | C low | B high |

This is standard **sensored trapezoidal 6-step commutation**.

### Practical dependency
Correct operation depends on:

- correct phase wiring
- correct hall ordering
- correct hall polarity
- correct hall-to-state mapping

If mismatched, symptoms include:

- lockup
- vibration
- loud operation
- high no-load current
- one phase heating more than others

---

## 15. Verified BOM Highlights

The following BOM details are now explicitly confirmed from the provided table:

| Designator(s) | Part / Value |
|---|---|
| U8, U9, U10 | **L6388ED013TR** |
| U2 | **INA181** |
| U11 | **L7805** |
| R9 | **0.0005 Ω** |
| R14–R19 | **22 Ω** |
| R1–R6, R13 | **2.2 kΩ** |
| R7, R8, R10, R11, R12 | **47 kΩ** |
| C4, C5 | **470 µF** |
| C21, C22 | **100 µF** |
| C9 | **22 nF** |
| C1, C2, C3, C11, C12, C15, C16, C17, C18, C19, C20 | **1 µF** |
| MCU1 | **Black Pill V3 STM32F411** |
| U14 | **Header5** |
| U15 | **POWER2-1** |
| U16 | **A_HEADER3** |
| J1 | **4-pin throttle connector** |
| J3 | **Fuse** |
| D1 | **TVS** |
| D2, D3, D4 | **SOD-123 diode** |

---

## 16. Confirmed / Likely / Unknown Items

## 16.1 Confirmed
- MCU module: Black Pill V3 STM32F411
- hall pins: PB6, PB7, PB8
- high-side control pins: PA8, PA9, PA10
- low-side control pins: PA7, PB0, PB1
- ISENSE pin: PA0
- VSENSE pin: PA4
- 3 × L6388ED013TR
- INA181 current amplifier
- 0.5 mΩ shunt
- 22 Ω gate resistors
- 470 µF bulk caps
- on-board L7805 5 V regulator

## 16.2 Likely but still should be confirmed
- exact populated MOSFET part number
- exact INA181 suffix
- exact VSENSE divider conversion ratio
- final throttle ADC pin used in firmware
- final intended input voltage range under load/thermal limits

## 16.3 Open questions
1. Exact installed MOSFET part number on the assembled board
2. Exact INA181 suffix: A1 / A2 / A3 / A4
3. Final throttle signal pin confirmation in firmware
4. Final bus input operating range
5. Whether final firmware target is asynchronous 6-step only or synchronous 6-step
6. Whether final firmware will remain Arduino-style or move to HAL/TIM1

---

## 17. Recommended Short Description

This board is a custom **STM32F411-based sensored 3-phase BLDC motor driver** using **3 × L6388ED013TR gate drivers**, a **6-MOSFET discrete inverter stage**, **hall inputs**, **current sensing through a 0.5 mΩ shunt and INA181**, **bus voltage sensing**, a **throttle interface**, and **USB CDC serial bring-up support**.

---

## 18. Quick Reference Table

| Category | Value |
|---|---|
| MCU module | Black Pill V3 STM32F411 |
| Motor type | Sensored 3-phase BLDC |
| Gate driver | 3 × L6388ED013TR |
| Inverter stage | 6 × external N-MOSFETs |
| Hall inputs | PB6 / PB7 / PB8 |
| High-side controls | PA8 / PA9 / PA10 |
| Low-side controls | PA7 / PB0 / PB1 |
| Current sense ADC | PA0 |
| Voltage sense ADC | PA4 |
| Test interface | USB CDC over Type-C |
| Shunt resistor | 0.5 mΩ |
| Current amplifier | INA181 |
| Regulator | L7805 |
| Current control in firmware | not yet implemented |
| Present control method | Sensored asynchronous 6-step |

---

## 19. Final Notes

This document reflects the current **bring-up state** of the driver.

It is suitable as:
- a design summary
- a pinout reference
- a firmware bring-up reference
- an internal handoff note

Before turning it into final production documentation, the following should still be verified on real hardware:

- MOSFET exact part number
- INA181 gain suffix
- throttle ADC routing used in final firmware
- bus voltage scaling
- validated current limits
- final protection strategy
