# STM32F446RE FL/FR + ARM Bridge + E-STOP Fixed v2

This build keeps the last working rover setup:

- FL wheel: USART1 TX = PA9
- FR wheel: USART3 TX = PC10
- RL/RR: fully disabled
- Arm bridge: USART6 TX = PC6, RX = PC7; arm RX bridge defaults OFF
- PC terminal: USART2 PA2/PA3

## E-STOP wiring in this build

`PC10` cannot be used for E-STOP anymore because it is the FR motor TX line.
E-STOP is now on `PB0`.

Default wiring:

```text
PB0 ---- E-STOP contact ---- GND
```

Default logic:

- PB0 HIGH = normal/released
- PB0 LOW  = E-STOP active

This is `INPUT_PULLUP + active LOW`. It prevents an unconnected PB0 from locking the robot during bring-up.

If your E-STOP is fail-safe NC-to-GND where normal is LOW and pressed/open is HIGH, edit `include/config.h`:

```cpp
#define ESTOP_ACTIVE_LEVEL HIGH
```

Use this command in terminal.py:

```text
estop
```

It prints the raw PB0 level and whether the firmware considers E-STOP active.

## Important test

1. Upload the firmware.
2. Run terminal.py and connect.
3. Send `estop`.
4. Toggle the physical E-STOP.
5. Send `estop` again.

If raw level does not change, the switch is not wired to PB0/GND or the board wiring is wrong.
