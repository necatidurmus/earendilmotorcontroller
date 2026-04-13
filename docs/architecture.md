# Mimari Genel Bakış

## Saat Ağacı

```
HSE 25 MHz
  └─ PLL (M=25, N=200, P=2) → SYSCLK = 100 MHz
       ├─ AHB  /1 → HCLK  = 100 MHz
       ├─ APB1 /2 → PCLK1 =  50 MHz → TIM3 saati = 100 MHz
       └─ APB2 /1 → PCLK2 = 100 MHz → TIM1 saati = 200 MHz
```

## TIM1 PWM Yapısı

```
TIM1 (200 MHz giriş)
  Prescaler = 1 → tick = 100 MHz (10 ns/tick)
  ARR = 3332 → PWM frekansı ≈ 30 kHz

  CH1  (PA8)  ─┐
  CH1N (PA7)  ─┤─ Komplementer çift A + DEADTIME_COUNTS = 500 ns
  CH2  (PA9)  ─┤
  CH2N (PB0)  ─┤─ Komplementer çift B + 500 ns
  CH3  (PA10) ─┤
  CH3N (PB1)  ─┘─ Komplementer çift C + 500 ns
```

## TIM3 Kontrol Zamanlayıcısı

```
TIM3 (100 MHz giriş)
  Prescaler = 99 → 1 MHz tick
  ARR = 79 → ISR frekansı = 12.5 kHz (80 µs/tick)
  Update interrupt → TIM3_IRQHandler → MotorControl_Tick()
```

## ISR Hot Path Akışı

```
TIM3_IRQHandler() [80 µs dönem]
  │
  ├─ Prot_SampleTick()
  │    ├─ decimator++ (her 4. tick'te ADC)
  │    ├─ BoardIO_ReadADC(ISENSE) — ~3.84 µs blocking
  │    ├─ BoardIO_ReadADC(VSENSE)
  │    └─ EMA filtre → currentDelta güncelle
  │
  ├─ Prot_CheckHardLimit()
  │    └─ delta >= hardLimit × 3 ardışık → Prot_LatchFault()
  │         └─ BoardIO_AllOff() + g_runMode = STOPPED
  │
  ├─ [g_runMode == STOPPED?] → Comm_AllOff(), return
  │
  ├─ Hall_ResolveState(nowUs)
  │    ├─ 7× GPIO okuma → çoğunluk oyu
  │    ├─ debounce kontrolü
  │    └─ hallToState[profile][corrected] → state 0..5
  │
  ├─ [state > 5?] → Comm_AllOff(), return
  │
  ├─ Prot_ApplySoftLimit(cmdDuty)
  │    └─ delta > softLimit → duty azalt
  │
  ├─ Prot_SlewDuty() → rampa uygula
  │
  └─ Comm_ApplyStep(state, pwmDuty, mode)
       ├─ TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = 0
       ├─ *CCR_FWD_PTR[state] = pwmDuty
       └─ TIM1->CCER = CCER_FWD[state]
```

## Bellek Haritası (yaklaşık)

- Flash: ~20-30 KB (tablo ve kütüphanelerle)
- RAM: <2 KB (global + stack)
- Stack: 512 B yeterli (ISR derinliği sığ)

## ISR Öncelik Tablosu

| Kaynak | Öncelik |
|---|---|
| TIM3 Update (motor control) | 0 (en yüksek) |
| SysTick (HAL_GetTick) | 15 (en düşük) |
