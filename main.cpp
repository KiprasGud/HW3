#include <Arduino.h>
#include <EEPROM.h>

#define BTN_START  PD2   // D2  - INT0, START / PAUSE
#define BTN_RESET  PD3   // D3  - INT1, RESET
#define BUZZ_BIT   PD4   // D4  - pjezo
#define LED_HEART  PD5   // D5  - sirdies plakimo LED (sistema gyva)
#define RELAY      PD7   // D7  - 1 kOhm -> TIP120 baze; TIP120 jungia apkrovos LED
#define BAR_MASK   0x3F  // PB0..PB5 = D8..D13 - 6 LED juosta
#define BAR_LEDS   6

const uint8_t BUZZ_ARDUINO_PIN = 4;
const uint8_t POT_PIN = A0;

#define SET_BIT(reg, bit)  ((reg) |=  (1 << (bit)))
#define CLR_BIT(reg, bit)  ((reg) &= ~(1 << (bit)))
#define READ_BIT(reg, bit) (((reg) >> (bit)) & 1)

enum State { BOOT, IDLE, RUNNING, PAUSED, DONE };
State state = BOOT;
const char *stateName[] = { "BOOT", "IDLE", "RUNNING", "PAUSED", "DONE" };

volatile uint8_t  pendingTicks = 0; 
volatile bool     startFlag = false;    
volatile bool     resetFlag = false; 
volatile uint32_t lastStart = 0, lastReset = 0;
struct Settings {
    uint8_t  magic;
    uint8_t  version;
    uint8_t  durationSec;
    uint16_t completed;
    uint8_t  checksum;
};
const uint8_t MAGIC = 0x5A, VERSION = 1;

Settings cfg;  
Settings stored;  
bool eepromWasValid = false;

uint16_t totalTicks = 0, remainingTicks = 0;  
uint16_t stateTicks = 0;          
uint8_t  heartTicks = 0, potTicks = 0, serialTicks = 0;
uint8_t  pausedBar = 0;

const uint16_t POT_HYST = 30; 
uint16_t potFilt = 0;     
uint16_t potAccepted = 0;  

bool    btnArmed[2]   = { true, true };
uint8_t btnRelease[2] = { 0, 0 };

ISR(TIMER1_COMPA_vect) {
    if (pendingTicks < 255) pendingTicks++;
}

ISR(INT0_vect) {
    uint32_t now = millis();
    if (now - lastStart > 150 && READ_BIT(PIND, BTN_START) == 0) {
        startFlag = true;
        lastStart = now;
    }
}

ISR(INT1_vect) {
    uint32_t now = millis();
    if (now - lastReset > 150 && READ_BIT(PIND, BTN_RESET) == 0) {
        resetFlag = true;
        lastReset = now;
    }
}

void showPattern(uint8_t p) {
    PORTB = (PORTB & ~BAR_MASK) | (p & BAR_MASK);
}

uint8_t barPattern(uint8_t n) {
    if (n >= BAR_LEDS) return BAR_MASK;
    return (uint8_t)((1u << n) - 1);
}

void relayOn()  { SET_BIT(PORTD, RELAY); }
void relayOff() { CLR_BIT(PORTD, RELAY); }

uint8_t calcChecksum(const Settings &s) {
    const uint8_t *p = (const uint8_t *)&s;
    uint8_t c = 0;
    for (uint8_t i = 0; i < sizeof(Settings) - 1; i++) c ^= p[i];
    return c;
}

bool loadSettings() {
    EEPROM.get(0, cfg);
    bool ok = cfg.magic == MAGIC &&
              cfg.version == VERSION &&
              cfg.checksum == calcChecksum(cfg) &&
              cfg.durationSec >= 5 && cfg.durationSec <= 60 &&
              cfg.durationSec % 5 == 0;
    if (!ok) {
        cfg.magic = MAGIC;
        cfg.version = VERSION;
        cfg.durationSec = 30;
        cfg.completed = 0;
        cfg.checksum = calcChecksum(cfg);
        EEPROM.put(0, cfg);
    }
    stored = cfg;
    return ok;
}

void saveIfChanged() {
    if (cfg.durationSec == stored.durationSec && cfg.completed == stored.completed) return;
    cfg.checksum = calcChecksum(cfg);
    EEPROM.put(0, cfg);
    stored = cfg;
    Serial.print(F("EEPROM write: dur="));
    Serial.print(cfg.durationSec);
    Serial.print(F(" done="));
    Serial.println(cfg.completed);
}

uint8_t potToDuration(uint16_t raw) {
    uint8_t steps = 1 + (uint32_t)raw * 12 / 1024; 
    return steps * 5;                              
}

uint8_t durationToLeds(uint8_t sec) {
    return (sec * BAR_LEDS + 59) / 60;             
}

uint8_t remainingLeds() {
    return (remainingTicks * (uint32_t)BAR_LEDS + totalTicks - 1) / totalTicks;
}

void setState(State s) {
    state = s;
    stateTicks = 0;
    if (s == IDLE) remainingTicks = 0;
    if (s == RUNNING) relayOn(); else relayOff();
    Serial.print(F("State -> "));
    Serial.println(stateName[s]);
}

void handleStart() {
    switch (state) {
        case IDLE:
            totalTicks = (uint16_t)cfg.durationSec * 100;
            remainingTicks = totalTicks;
            saveIfChanged();              
            tone(BUZZ_ARDUINO_PIN, 2000, 60);
            setState(RUNNING);
            break;
        case RUNNING:
            pausedBar = barPattern(remainingLeds());
            setState(PAUSED);
            break;
        case PAUSED:
            setState(RUNNING);
            break;
        case DONE:
            setState(IDLE);
            break;
        default: break;  
    }
}

void handleReset() {
    if (state == BOOT || state == IDLE) return;
    tone(BUZZ_ARDUINO_PIN, 600, 80);
    setState(IDLE);
}

void updateButtons() {
    const uint8_t bits[2] = { BTN_START, BTN_RESET };
    for (uint8_t i = 0; i < 2; i++) {
        if (READ_BIT(PIND, bits[i])) {  
            if (btnRelease[i] < 3 && ++btnRelease[i] == 3) btnArmed[i] = true;
        } else {
            btnRelease[i] = 0; 
        }
    }
}

void updatePot() {
    uint16_t raw = analogRead(POT_PIN);
    potFilt = (potFilt * 3 + raw) / 4;
    if (abs((int)potFilt - (int)potAccepted) > POT_HYST) {
        potAccepted = potFilt;
        cfg.durationSec = potToDuration(potAccepted);
    }
}

void tickTask() {
    if (stateTicks < 60000) stateTicks++;
    updateButtons();

    if (++heartTicks >= 50) {
        heartTicks = 0;
        PIND = (1 << LED_HEART); 
    }

    switch (state) {
        case BOOT:
            showPattern(barPattern(stateTicks / 16));
            if (stateTicks >= 110) setState(IDLE);
            break;

        case IDLE:
            if (++potTicks >= 2) {   
                potTicks = 0;
                updatePot();
            }
            showPattern(barPattern(durationToLeds(cfg.durationSec)));
            break;

        case RUNNING:
            if (remainingTicks > 0) remainingTicks--;
            showPattern(barPattern(remainingLeds()));
            if (remainingTicks == 0) {
                cfg.completed++;
                saveIfChanged();
                setState(DONE);
            }
            break;

        case PAUSED:
            showPattern((stateTicks / 25) % 2 ? pausedBar : 0);
            break;

        case DONE:
            showPattern(1 << ((stateTicks / 8) % BAR_LEDS));
            if (stateTicks % 30 == 1 && stateTicks < 300) tone(BUZZ_ARDUINO_PIN, 2000, 150);
            if (stateTicks >= 500) setState(IDLE);
            break;
    }

    if (++serialTicks >= 100) {
        serialTicks = 0;
        Serial.print(stateName[state]);
        Serial.print(F(" dur="));   Serial.print(cfg.durationSec);
        Serial.print(F("s left=")); Serial.print(remainingTicks / 100);
        Serial.print(F("s done=")); Serial.print(cfg.completed);
        Serial.print(F(" raw="));   Serial.println(potFilt);
    }
}

// ---------------- Konfiguracija ----------------
void setupGpio() {
    // PORTD: isejimai - rele, LED, pjezo
    DDRD |= (1 << RELAY) | (1 << LED_HEART) | (1 << BUZZ_BIT);
    // PORTD: iejimai su vidiniu pull-up - mygtukai
    DDRD  &= ~((1 << BTN_START) | (1 << BTN_RESET));
    PORTD |=  (1 << BTN_START) | (1 << BTN_RESET);
    relayOff();

    // PORTB: D8..D13 - LED juosta, isejimai, pradzioje visos uzgesusios
    DDRB  |=  BAR_MASK;
    PORTB &= ~BAR_MASK;
}

// Timer1, CTC, 100 Hz: 16 MHz / 64 = 250 000 Hz; 250 000 / 100 = 2500 -> OCR1A = 2499
void setupTimer1() {
    TCCR1A = 0;                                         // jokiu PWM isejimu (D9, D10 lieka GPIO)
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10);  // CTC, prescaler 64
    OCR1A  = 2499;                                      // tiksliai 10 ms
    TIMSK1 = (1 << OCIE1A);                             // Compare A pertraukimas
}

// Isoriniai pertraukimai per registrus
void setupExtInterrupts() {
    EICRA = (1 << ISC01) | (1 << ISC11);   // INT0 ir INT1: krentantis frontas
    EIFR  = (1 << INTF0) | (1 << INTF1);   // isvalyti senas veliavas
    EIMSK = (1 << INT0)  | (1 << INT1);    // ijungti INT0 ir INT1
}


// =====================================================================
void setup() {
    Serial.begin(9600);
    setupGpio();

    eepromWasValid = loadSettings();
    potFilt = analogRead(POT_PIN);
    potAccepted = potFilt;

    Serial.println(F("HW3 relay timer"));
    Serial.println(eepromWasValid ? F("EEPROM OK") : F("EEPROM default"));
    Serial.print(F("dur=")); Serial.print(cfg.durationSec);
    Serial.print(F("s done=")); Serial.println(cfg.completed);

    cli();
    setupTimer1();
    setupExtInterrupts();
    sei();

    setState(BOOT);
}

void loop() {
    // 1) Mygtuku ivykiai
    // Priimam tik jei mygtukas buvo atleistas po praeito paspaudimo
    if (startFlag) {
        startFlag = false;
        if (btnArmed[0]) { btnArmed[0] = false; btnRelease[0] = 0; handleStart(); }
    }
    if (resetFlag) {
        resetFlag = false;
        if (btnArmed[1]) { btnArmed[1] = false; btnRelease[1] = 0; handleReset(); }
    }

    // 2) Timer1 tick'ai (atomiskai paimam ir nunulinam)
    cli();
    uint8_t ticks = pendingTicks;
    pendingTicks = 0;
    sei();
    while (ticks--) tickTask();
}
