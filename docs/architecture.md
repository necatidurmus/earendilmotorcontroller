# Mimari Genel Bakış

## Saat Ağacı

```
HSE 25 MHz
  └─ PLL (M=25, N=192, P=2) → SYSCLK = 96 MHz
       ├─ AHB  /1 → HCLK  = 96 MHz
       ├─ APB1 /2 → PCLK1 = 48 MHz → TIM3 saati = 96 MHz (x2)
       └─ APB2 /1 → PCLK2 = 96 MHz → TIM1 saati = 96 MHz (x1)
```

## TIM1 PWM Yapısı

```
TIM1 (96 MHz APB2 timer saati, PSC=0)
  tick = 96 MHz (10.4 ns/tick)
  ARR = 3199 → PWM frekansı = 96 MHz / 3200 = 30 kHz

  CH1  (PA8)  ─┐
  CH1N (PA7)  ─┤─ Komplementer çift A + DEADTIME_COUNTS = ~521 ns
  CH2  (PA9)  ─┤
  CH2N (PB0)  ─┤─ Komplementer çift B + ~521 ns
  CH3  (PA10) ─┤
  CH3N (PB1)  ─┘─ Komplementer çift C + ~521 ns
```

## TIM3 Kontrol Zamanlayıcısı

```
TIM3 (96 MHz APB1 timer saati, APB1=48 MHz x2)
  Prescaler = 95 → 96 MHz / 96 = 1 MHz tick (1 µs)
  ARR = 79 → ISR frekansı = 1 MHz / 80 = 12.5 kHz (80 µs/tick)
  Update interrupt → TIM3_IRQHandler → MotorControl_Tick()
```

## ISR Hot Path Akışı

```
TIM3_IRQHandler() [80 µs dönem]
  │
  ├─ Prot_SampleTick()
  │    ├─ decimator++ (her 4. tick'te ADC)
  │    ├─ BoardIO_ReadADC(ISENSE) — ~4 µs blocking (EOC timeout ~10 µs)
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
       ├─ *CCR_xxx_PTR[state] = pwmDuty
       └─ TIM1->CCER = (TIM1->CCER & ~CCER_COMM_MASK) | CCER_xxx[state]
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
