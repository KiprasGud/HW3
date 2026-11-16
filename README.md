# HW3 – Programmable Timer with Transistor Switch

An event-driven Arduino Uno timer. A **potentiometer** sets the duration (5–60 s), the **START** button switches on a load through a **TIP120 transistor**, and a **6-LED bar graph** shows the remaining time. When the time runs out the load turns off, the bar shows a running light and a piezo beeps. The last duration and the number of completed runs are stored in **EEPROM** and survive a power cycle.

All digital I/O is done through **memory-mapped I/O registers** (`DDRx`, `PORTx`, `PINx`). The firmware uses no `pinMode`, `digitalRead`, `digitalWrite` or `attachInterrupt`, and no `delay()`.

---

## Features

- Duration set by potentiometer (5–60 s in 5 s steps), with filtering and hysteresis
- Load switched by an NPN Darlington transistor (TIP120)
- 6-LED bar graph on PORTB, updated with a single register write
- START / PAUSE / RESUME and RESET buttons on external interrupts INT0 and INT1
- Timer1 interrupt at 100 Hz drives all periodic work
- Main-loop state machine: `BOOT → IDLE → RUNNING ⇄ PAUSED → DONE`
- EEPROM settings with magic byte, version and checksum, write-on-change
- Feedback: bar graph, load LED, heartbeat LED, piezo beeps, Serial log

---

## Hardware

| Part | Qty | Notes |
|---|---|---|
| Arduino Uno | 1 | ATmega328P, 16 MHz |
| Potentiometer 10 kΩ | 1 | Duration setting |
| TIP120 NPN Darlington transistor | 1 | Load switch |
| LED (red) | 6 | Bar graph |
| LED (green) | 1 | Load (switched by TIP120) |
| LED | 1 | Heartbeat |
| Resistor 1 kΩ | 7 | 6× bar graph, 1× TIP120 base |
| Resistor 330 Ω | 2 | Load LED, heartbeat LED |
| Push button (4-pin) | 2 | START/PAUSE, RESET |
| Piezo buzzer | 1 | Beeps |

---

## Wiring / Pinout

| Arduino pin | AVR bit | Connected to | Function |
|---|---|---|---|
| A0 | ADC0 | Potentiometer wiper | Duration input |
| D2 | PD2 / INT0 | Button → GND | START / PAUSE / RESUME |
| D3 | PD3 / INT1 | Button → GND | RESET |
| D4 | PD4 | Piezo (+), (−) → GND | Beeps |
| D5 | PD5 | 330 Ω → LED → GND | Heartbeat (toggles every 500 ms) |
| D7 | PD7 | 1 kΩ → TIP120 base | Load control |
| D8–D13 | PB0–PB5 | 1 kΩ → LED → GND (each) | Bar graph (D8 = first LED) |

### Schematic

```
 Potentiometer:   5V ── [pot] ── GND
                          │
                          A0

 Buttons (internal pull-up, no external resistor):
   D2 ──[START]── GND
   D3 ──[RESET]── GND

 Load switch:
   5V ── 330 Ω ── LED(green) ──┐
                               C
   D7 ── 1 kΩ ────────────── B  TIP120
                               E
                               │
                              GND

 Bar graph (×6, D8..D13):
   Dx ── 1 kΩ ── LED ── GND

 Heartbeat:  D5 ── 330 Ω ── LED ── GND
 Piezo:      D4 ── piezo ── GND
```

TIP120 pinout (label facing you, legs down): **B – C – E** from left to right. The metal tab is connected to the collector.

### Electrical notes

- **TIP120 base current:** (5 V − ~1.4 V) / 1 kΩ ≈ 3.6 mA. The Darlington gain (≥ 1000) means this is far more than needed to saturate the transistor for a few mA of load current.
- **Why a transistor:** an Arduino pin should not supply more than ~20 mA. The transistor lets a small pin current switch a larger load from the 5 V rail, and the same circuit works for higher-current loads (motor, relay coil, lamp).
- **Bar graph current:** (5 V − ~2 V) / 1 kΩ ≈ 3 mA per LED, 18 mA for all six, well within the PORTB and total chip limits.
- **Buttons** use the internal pull-up, so the interrupt pins never float. Pressed = LOW, interrupts trigger on the falling edge.

---

## Build & Upload

The project uses **PlatformIO**.

```
.
├── platformio.ini
├── src/
│   └── main.cpp
└── README.md
```

`platformio.ini`:

```ini
[env:uno]
platform = atmelavr
board = uno
framework = arduino
monitor_speed = 9600
```

Steps:

1. Open the project in CLion / VS Code with PlatformIO.
2. Build: `pio run`
3. Upload: `pio run -t upload` (close the Serial Monitor first, otherwise the COM port is busy)
4. Serial monitor: `pio device monitor`

`EEPROM` is part of the Arduino framework; no external libraries are needed.

---

## Usage

| State | Bar graph | Load LED | START (D2) | RESET (D3) |
|---|---|---|---|---|
| BOOT | Fills up over ~1 s | Off | ignored | ignored |
| IDLE | Shows set duration (1 LED ≈ 10 s) | Off | Start timer | – |
| RUNNING | Shrinks as time runs out | **On** | Pause | Cancel → IDLE |
| PAUSED | Blinks at 2 Hz | Off | Resume | Cancel → IDLE |
| DONE | Running light, 3 s of beeps | Off | → IDLE | → IDLE |

- Turn the potentiometer only in IDLE; it is ignored while the timer is running.
- After a power cycle the duration from EEPROM is kept until the potentiometer is actually moved.
- DONE returns to IDLE automatically after 5 s.

---

## Software Design

### Architecture

```
 Timer1 ISR (100 Hz) ─► pendingTicks++ ─┐
 INT0 ISR (START)   ─► startFlag       ─┼─► loop(): button handlers + tickTask() per tick
 INT1 ISR (RESET)   ─► resetFlag       ─┘
```

```cpp
void loop() {
    if (startFlag) { startFlag = false; if (btnArmed[0]) { ... handleStart(); } }
    if (resetFlag) { resetFlag = false; if (btnArmed[1]) { ... handleReset(); } }

    cli();                          // take the tick counter atomically
    uint8_t ticks = pendingTicks;
    pendingTicks = 0;
    sei();
    while (ticks--) tickTask();     // 10 ms of work per tick
}
```

Using a tick **counter** instead of a boolean flag means no time is lost if the loop is ever late: each missed 10 ms tick is processed afterwards, so the countdown stays accurate.

### State machine

```
 BOOT ──(1.1 s)──► IDLE ──START──► RUNNING ──(time = 0)──► DONE ──(5 s or START/RESET)──► IDLE
                    ▲                │  ▲                                    
                    │             START  START                               
                    │                ▼  │                                    
                    └────RESET─────  PAUSED                                  
                    └────RESET─────  RUNNING                                 
```

The load is controlled in one place: `setState()` switches it on only when entering `RUNNING` and off for every other state, so the load can never be left on by mistake.

### Interrupt (ISR) roles

All ISRs are in the marked `ISR SECTION` of `main.cpp`. They only update counters/flags; there is no Serial, ADC, EEPROM or I/O-heavy work inside them.

| ISR | Source | Trigger | Role |
|---|---|---|---|
| `TIMER1_COMPA_vect` | Timer1 compare match A | Every 10 ms | Increments `pendingTicks` |
| `INT0_vect` | External interrupt 0 (D2) | Falling edge | Sets `startFlag` (150 ms lockout, pin must read LOW) |
| `INT1_vect` | External interrupt 1 (D3) | Falling edge | Sets `resetFlag` (150 ms lockout, pin must read LOW) |

Shared variables are `volatile`. External interrupts are configured directly through registers, without `attachInterrupt`:

| Register | Value | Meaning |
|---|---|---|
| `EICRA` | `ISC01 \| ISC11` | INT0 and INT1 on falling edge |
| `EIFR` | `INTF0 \| INTF1` | Clear stale interrupt flags |
| `EIMSK` | `INT0 \| INT1` | Enable INT0 and INT1 |

### Button debouncing

Mechanical buttons bounce both when pressed and when released. Two layers prevent double triggers:

1. **In the ISR:** a new event is accepted only 150 ms after the previous one, and only if `PIND` still reads LOW.
2. **Release detection in `tickTask()`:** after a press is handled, the button is "disarmed" and is re-armed only after `PIND` reads HIGH for 3 consecutive ticks (30 ms). One physical press therefore produces exactly one action, even if the button is held.

### Timer configuration

Timer1 in **CTC mode** generates a 100 Hz interrupt:

```
f_tick = 16 MHz / 64 = 250 000 Hz
OCR1A  = 250 000 / 100 − 1 = 2499   →  exactly 100 Hz (10.000 ms)
```

| Register | Value | Meaning |
|---|---|---|
| `TCCR1A` | `0` | No PWM outputs (D9/D10 stay plain GPIO) |
| `TCCR1B` | `WGM12` | CTC mode (TOP = OCR1A) |
| `TCCR1B` | `CS11 \| CS10` | Prescaler 64 |
| `OCR1A` | `2499` | Compare value |
| `TIMSK1` | `OCIE1A` | Enable compare A interrupt |

Timer usage:

| Timer | Used by |
|---|---|
| Timer0 | `millis()` (used only for ISR debounce timestamps) |
| Timer1 | **100 Hz system tick** |
| Timer2 | `tone()` for the piezo |

Work derived from the 100 Hz tick:

| Task | Rate |
|---|---|
| Countdown, bar graph update, button release detection | 100 Hz |
| Potentiometer sampling | 50 Hz |
| Pause blink | 2 Hz |
| Heartbeat LED toggle | 2 Hz (1 Hz blink) |
| Serial status line | 1 Hz |

### MMIO (direct register access)

Each port has three registers, and each pin is one bit in them:

| Register | Role | Replaces |
|---|---|---|
| `DDRx` | Direction (1 = output) | `pinMode()` |
| `PORTx` | Output level / pull-up enable | `digitalWrite()` |
| `PINx` | Input level | `digitalRead()` |

Examples from the code:

```cpp
DDRD  |=  (1 << PD7);                        // D7 as output           (pinMode)
PORTD |=  (1 << PD2) | (1 << PD3);           // pull-ups on D2, D3     (INPUT_PULLUP)
PORTD |=  (1 << PD7);                        // load ON                (digitalWrite HIGH)
bool pressed = !((PIND >> PD2) & 1);         // read button            (digitalRead)
PIND  =   (1 << PD5);                        // toggle heartbeat LED (AVR: writing 1 to PINx toggles PORTx)
PORTB = (PORTB & ~0x3F) | (pattern & 0x3F);  // set all 6 bar LEDs in ONE write
```

The bar graph shows the main advantage of MMIO: all six LEDs change in a single instruction, while `digitalWrite` would need six calls (each ~3–4 µs) and the LEDs would not switch at exactly the same time. The `& ~0x3F` mask leaves PB6/PB7 (crystal pins) untouched.

### EEPROM layout

Settings are stored as one packed struct (6 bytes) starting at address 0.

| Address | Size | Field | Type | Default | Description |
|---|---|---|---|---|---|
| 0 | 1 B | `magic` | `uint8_t` | `0x5A` | Identifies this project's data |
| 1 | 1 B | `version` | `uint8_t` | `1` | Data format version |
| 2 | 1 B | `durationSec` | `uint8_t` | `30` | Last used duration, 5–60 s, multiple of 5 |
| 3–4 | 2 B | `completed` | `uint16_t` | `0` | Number of fully completed runs |
| 5 | 1 B | `checksum` | `uint8_t` | – | XOR of bytes 0–4 |

**Validation on boot:** data is accepted only if `magic`, `version` and `checksum` match and `durationSec` is in range and a multiple of 5. Otherwise defaults are written once.

**Write-on-change:** a RAM copy of what is currently in EEPROM (`stored`) is kept. EEPROM is written only when `durationSec` or `completed` differs from it: when a timer is started with a new duration, or when a run completes. `EEPROM.put()` additionally skips bytes that are unchanged. Turning the potentiometer alone never writes to EEPROM.

### Potentiometer filtering

- **IIR average:** `filtered = (filtered × 3 + raw) / 4` smooths ADC noise.
- **Hysteresis:** the duration changes only when the filtered value moves more than 30 ADC counts (~3 % of travel) from the last accepted position, so a pot resting on a step boundary does not flicker between two values.

---

## Timing Budget

| Task | Period | Approx. duration | CPU load |
|---|---|---|---|
| Timer1 ISR | 10 ms | < 5 µs | < 0.1 % |
| `tickTask()` without ADC | 10 ms | ~10–20 µs | ~0.2 % |
| `analogRead()` (potentiometer) | 20 ms | ~112 µs | ~0.6 % |
| Serial status line (buffered) | 1 s | ~0.3 ms | < 0.1 % |
| **Total** | | | **~1 %** |

Durations are estimates from the ATmega328P datasheet (ADC conversion = 13 ADC clocks at 125 kHz ≈ 104 µs) and instruction counts; they can be verified with `micros()` around each task.

---

## Testing

| Test | Expected result | Result |
|---|---|---|
| Boot | Bar fills up, Serial shows `EEPROM OK` / `EEPROM default` | ✅ |
| Potentiometer turned | Bar shows 1–6 LEDs, `dur=` 5–60 s | ✅ |
| Potentiometer untouched | `dur=` stable, no flicker | ✅ |
| START | Load LED on, bar shrinks | ✅ |
| PAUSE / RESUME | Bar blinks and load turns off; countdown continues on resume | _fill in_ |
| RESET while running | Load off, back to IDLE | _fill in_ |
| Run completes | Running light, beeps, `done=` +1, EEPROM write logged | _fill in_ |
| Single button press | Exactly one action, no double triggers | ✅ |
| Power cycle | Duration and `done=` counter retained | _fill in_ |
| Timer accuracy (60 s run vs. stopwatch) | Within ±0.1 s | _fill in_ |

---

## Known Issues / Limitations

- **Relay and 74HC595 were dropped.** The first version switched a relay (HK19F-DC5V) and drove an 8-LED bar through a 74HC595 shift register. Unreliable breadboard contacts made both hard to debug, so the relay was replaced by switching the load directly with the TIP120, and the shift register by six LEDs on PORTB. The TIP120 stage can drive a relay coil again by adding a flyback diode across the coil.
- **Only 6 bar LEDs:** one LED represents ~10 s, so the bar is coarse; exact remaining time is on Serial (`left=`).
- **Duration can only be set in 5 s steps** from 5 to 60 s.
- **Darlington voltage drop:** TIP120 has ~0.7–1 V saturation voltage, so the load receives slightly less than 5 V. Fine for an LED, a consideration for heavier loads.
- **Debounce timestamps use `millis()`** inside the ISR; this is acceptable here because only differences are compared, but a Timer1-tick based timestamp would remove the Timer0 dependency.
- A faulty potentiometer caused large random jumps during development; replacing it fixed the issue. Filtering only handles small noise, not a broken wiper contact.

---

## Demo Video

_Link to video (≤ 2 min): boot, setting the duration, start/pause/reset, timer completion, EEPROM values surviving a power cycle._
