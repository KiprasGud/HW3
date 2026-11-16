# HW3 – Programmable Timer with Transistor Switch

Arduino Uno timer: a potentiometer sets the duration (5–60 s), START switches on a load through a TIP120 transistor, and a 6-LED bar shows the remaining time. Duration and completed-run count are stored in EEPROM.

All digital I/O uses direct register access (MMIO: `DDRx`, `PORTx`, `PINx`) instead of `pinMode` / `digitalRead` / `digitalWrite`. No `delay()`.

---

## Build Steps

`platformio.ini`:

```ini
[env:uno]
platform = atmelavr
board = uno
framework = arduino
monitor_speed = 9600
```

1. Open the project with PlatformIO (code in `src/main.cpp`).
2. Build: `pio run`
3. Upload: `pio run -t upload` (close the Serial Monitor first)
4. Serial log: `pio device monitor`

---

## Wiring / Schematic

| Arduino pin | AVR bit | Connected to |
|---|---|---|
| A0 | ADC0 | Potentiometer wiper (outer pins to 5V and GND) |
| D2 | PD2 / INT0 | START/PAUSE button → GND (internal pull-up) |
| D3 | PD3 / INT1 | RESET button → GND (internal pull-up) |
| D4 | PD4 | Piezo → GND |
| D5 | PD5 | 330 Ω → heartbeat LED → GND |
| D7 | PD7 | 1 kΩ → TIP120 base |
| D8–D13 | PB0–PB5 | 1 kΩ → LED → GND (6-LED bar) |

```
 5V ── 330 Ω ── load LED ──┐
                           C
 D7 ── 1 kΩ ──────────── B   TIP120   (pins B-C-E, label facing you)
                           E
                           │
                          GND

 Dx (D8..D13) ── 1 kΩ ── LED ── GND
 D2 ──[START]── GND      D3 ──[RESET]── GND
```

TIP120 base current ≈ (5 − 1.4) V / 1 kΩ ≈ 3.6 mA; bar LEDs ≈ 3 mA each.

---

## Timer Configuration

Timer1, CTC mode, 100 Hz system tick:

```
16 MHz / 64 = 250 000 Hz
OCR1A = 250 000 / 100 − 1 = 2499  →  10 ms
```

| Register | Value | Meaning |
|---|---|---|
| `TCCR1A` | `0` | No PWM outputs |
| `TCCR1B` | `WGM12` | CTC mode |
| `TCCR1B` | `CS11 \| CS10` | Prescaler 64 |
| `OCR1A` | `2499` | Compare value |
| `TIMSK1` | `OCIE1A` | Compare A interrupt enabled |

The tick drives the countdown and bar graph (100 Hz), potentiometer sampling (50 Hz), heartbeat and pause blink (2 Hz) and the Serial log (1 Hz).

---

## ISR Roles

ISRs are in the `ISR SECTION` of `main.cpp` and only set flags/counters; all logic runs in the main-loop state machine (`BOOT → IDLE → RUNNING ⇄ PAUSED → DONE`).

| ISR | Source | Role |
|---|---|---|
| `TIMER1_COMPA_vect` | Timer1 compare A, every 10 ms | `pendingTicks++` |
| `INT0_vect` | D2, falling edge | `startFlag = true` (150 ms lockout) |
| `INT1_vect` | D3, falling edge | `resetFlag = true` (150 ms lockout) |

External interrupts are set via registers: `EICRA = ISC01 | ISC11` (falling edge), `EIMSK = INT0 | INT1`. A button is accepted again only after `PIND` reads it released for 30 ms, which prevents double presses.

---

## EEPROM Layout

6 bytes from address 0:

| Address | Field | Type | Default |
|---|---|---|---|
| 0 | `magic` | `uint8_t` | `0x5A` |
| 1 | `version` | `uint8_t` | `1` |
| 2 | `durationSec` | `uint8_t` | `30` (5–60, step 5) |
| 3–4 | `completed` | `uint16_t` | `0` |
| 5 | `checksum` | `uint8_t` | XOR of bytes 0–4 |

- **Validation:** on boot, magic, version, checksum and range are checked; on failure defaults are written.
- **Write-on-change:** EEPROM is written only when the duration or the completed count differs from the stored copy (on START with a new duration, or when a run completes).
