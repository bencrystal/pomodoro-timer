// =====================================================
// Keychain Pomodoro Timer — XIAO nRF52840
// =====================================================
// Button interaction map:
//   SLEEPING  + single click  → wake + start 25min
//   SLEEPING  + long press    → wake + start 45min
//   RUNNING   + single click  → pause
//   RUNNING   + long press    → cancel + sleep
//   PAUSED    + single click  → resume
//   PAUSED    + long press    → cancel + sleep
//   DONE      + single click  → acknowledge + sleep
//   ANYTIME   + triple click  → battery check
//
// LED states:
//   Running (normal)  → slow green breathing
//   Running (last 5m) → faster amber breathing
//   Paused            → slow yellow/orange blink
//   Done              → fast red/white strobe
//   Battery check     → color tier flashes
// =====================================================

#include <Arduino.h>

// ── Pin definitions ───────────────────────────────
#define PIN_BTN   D1   // B3F button (other leg to GND)
#define PIN_R     D2
#define PIN_G     D3
#define PIN_B     D10

// ── Timer durations ───────────────────────────────
#define DURATION_SHORT  (25UL * 60 * 1000)   // 25 min in ms
#define DURATION_LONG   (45UL * 60 * 1000)   // 45 min in ms
#define PAUSE_TIMEOUT   (10UL * 60 * 1000)   // auto-cancel if paused 10 min

// ── Button timing ─────────────────────────────────
#define LONG_PRESS_MS     600
#define TRIPLE_CLICK_MS   500   // window for triple click
#define DEBOUNCE_MS       50

// ── States ────────────────────────────────────────
enum State { SLEEPING, RUNNING, PAUSED, DONE };
State state = SLEEPING;

// ── Retained RAM (survives deep sleep) ────────────
// nRF52840 has 32 retained registers (4 bytes each)
// We use two to persist state across deep sleep wakeup
#define MAGIC 0xDEADBEEFUL
volatile uint32_t* retainedMagic    = (uint32_t*)0x20000000;
volatile uint32_t* retainedDuration = (uint32_t*)0x20000004;

// ── Runtime variables ─────────────────────────────
unsigned long timerDuration   = DURATION_SHORT;
unsigned long startMillis     = 0;
unsigned long pausedMillis    = 0;   // how long spent paused
unsigned long pauseStarted    = 0;
unsigned long elapsed         = 0;

// Button state
int           btnState        = HIGH;
int           lastBtnState    = HIGH;
unsigned long btnPressedAt    = 0;
unsigned long lastReleaseAt   = 0;
int           clickCount      = 0;
bool          longPressHandled = false;

// LED breathing
unsigned long ledPhase        = 0;

// ── Forward declarations ──────────────────────────
void goToSleep();
void startTimer(unsigned long duration);
void handleButtonEvent();
void updateLED();
void breathe(uint8_t r, uint8_t g, uint8_t b, float speed);
void setRGB(uint8_t r, uint8_t g, uint8_t b);
void flashBattery();
void strobeRGB(uint8_t r, uint8_t g, uint8_t b);
void blinkRGB(uint8_t r, uint8_t g, uint8_t b, int times, int onMs, int offMs);
float getBatteryVoltage();
int   getBatteryPercent();

// =====================================================
// SETUP
// =====================================================
void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  setRGB(0, 0, 0);

  // Check if we woke from deep sleep (magic value in retained RAM)
  // If so, determine long vs short press at wakeup
  bool wokeFromSleep = (*retainedMagic == MAGIC);

  if (wokeFromSleep) {
    // Detect long press at wakeup: button will still be held
    // Wait to see if it's held for LONG_PRESS_MS
    unsigned long wakeTime = millis();
    while (digitalRead(PIN_BTN) == LOW) {
      if (millis() - wakeTime > LONG_PRESS_MS) {
        // Long press → 45 min
        startTimer(DURATION_LONG);
        // Wait for release
        while (digitalRead(PIN_BTN) == LOW) delay(10);
        return;
      }
      delay(10);
    }
    // Short press → 25 min
    startTimer(DURATION_SHORT);
  } else {
    // Cold boot — just sleep and wait for button
    *retainedMagic = MAGIC;
    goToSleep();
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  handleButtonEvent();

  if (state == RUNNING) {
    elapsed = (millis() - startMillis) - pausedMillis;

    if (elapsed >= timerDuration) {
      state = DONE;
      setRGB(0, 0, 0);
      return;
    }
  }

  if (state == PAUSED) {
    // Auto-cancel if paused too long
    if (millis() - pauseStarted > PAUSE_TIMEOUT) {
      goToSleep();
      return;
    }
  }

  updateLED();
}

// =====================================================
// BUTTON HANDLING
// =====================================================
void handleButtonEvent() {
  int reading = digitalRead(PIN_BTN);

  // Debounce
  static unsigned long lastDebounce = 0;
  if (reading != lastBtnState) {
    lastDebounce = millis();
  }
  lastBtnState = reading;
  if (millis() - lastDebounce < DEBOUNCE_MS) return;

  // Press detected
  if (reading == LOW && btnState == HIGH) {
    btnPressedAt = millis();
    longPressHandled = false;
    btnState = LOW;
  }

  // While held — detect long press
  if (reading == LOW && btnState == LOW && !longPressHandled) {
    if (millis() - btnPressedAt >= LONG_PRESS_MS) {
      longPressHandled = true;
      clickCount = 0;
      onLongPress();
    }
  }

  // Release detected
  if (reading == HIGH && btnState == LOW) {
    btnState = HIGH;
    unsigned long heldFor = millis() - btnPressedAt;

    if (!longPressHandled) {
      // Count as a click
      clickCount++;
      lastReleaseAt = millis();
    }
  }

  // Evaluate click count after window expires
  if (clickCount > 0 && millis() - lastReleaseAt > TRIPLE_CLICK_MS) {
    if (clickCount >= 3) {
      flashBattery();
    } else {
      onSingleClick();
    }
    clickCount = 0;
  }
}

void onSingleClick() {
  switch (state) {
    case RUNNING:
      // Pause
      state = PAUSED;
      pauseStarted = millis();
      break;

    case PAUSED:
      // Resume
      pausedMillis += millis() - pauseStarted;
      state = RUNNING;
      break;

    case DONE:
      goToSleep();
      break;

    default:
      break;
  }
}

void onLongPress() {
  switch (state) {
    case RUNNING:
    case PAUSED:
      // Cancel → sleep
      goToSleep();
      break;

    default:
      break;
  }
}

// =====================================================
// LED
// =====================================================
void updateLED() {
  switch (state) {
    case RUNNING: {
      unsigned long remaining = timerDuration - elapsed;
      if (remaining <= 5UL * 60 * 1000) {
        // Last 5 min — faster amber breathing
        breathe(255, 80, 0, 2.5);
      } else {
        // Normal — slow green breathing
        breathe(0, 255, 40, 1.2);
      }
      break;
    }

    case PAUSED: {
      // Slow yellow/orange blink (500ms on, 800ms off)
      unsigned long t = millis() % 1300;
      if (t < 500) setRGB(255, 120, 0);
      else         setRGB(0, 0, 0);
      break;
    }

    case DONE: {
      // Fast red/white strobe
      unsigned long t = millis() % 200;
      if (t < 100) setRGB(255, 255, 255);
      else         setRGB(255, 0, 0);
      break;
    }

    default:
      setRGB(0, 0, 0);
      break;
  }
}

// Smooth sine-wave breathing effect
void breathe(uint8_t r, uint8_t g, uint8_t b, float speed) {
  float t = millis() / 1000.0 * speed;
  float brightness = (sin(t * PI) + 1.0) / 2.0;  // 0.0 – 1.0
  brightness = brightness * brightness;            // ease-in curve
  setRGB(
    (uint8_t)(r * brightness),
    (uint8_t)(g * brightness),
    (uint8_t)(b * brightness)
  );
}

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

void blinkRGB(uint8_t r, uint8_t g, uint8_t b, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    setRGB(r, g, b);
    delay(onMs);
    setRGB(0, 0, 0);
    if (i < times - 1) delay(offMs);
  }
}

// =====================================================
// BATTERY
// =====================================================
float getBatteryVoltage() {
  // Enable battery measurement on XIAO nRF52840
  // Pull pin 14 high to enable battery ADC on XIAO nRF52840
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);
  delay(10);

  analogReadResolution(12);
  int raw = analogRead(PIN_VBAT);
  float voltage = raw * 3.3f / 4096.0f * 2.0f;  // x2 for voltage divider

  digitalWrite(14, LOW);
  return voltage;
}

int getBatteryPercent() {
  float v = getBatteryVoltage();
  if (v >= 4.2f) return 100;
  if (v <= 3.2f) return 0;
  return (int)((v - 3.2f) / 1.0f * 100.0f);
}

void flashBattery() {
  int pct = getBatteryPercent();
  setRGB(0, 0, 0);
  delay(300);

  if (pct > 75) {
    blinkRGB(0, 255, 40, 4, 200, 150);   // 4x green
  } else if (pct > 50) {
    blinkRGB(0, 255, 40, 3, 200, 150);   // 3x green
  } else if (pct > 25) {
    blinkRGB(255, 180, 0, 2, 200, 150);  // 2x yellow
  } else {
    blinkRGB(255, 0, 0, 1, 400, 0);      // 1x red
  }

  delay(300);
}

// =====================================================
// TIMER + SLEEP
// =====================================================
void startTimer(unsigned long duration) {
  timerDuration = duration;
  startMillis   = millis();
  pausedMillis  = 0;
  pauseStarted  = 0;
  elapsed       = 0;
  state         = RUNNING;
}

void goToSleep() {
  setRGB(0, 0, 0);
  state = SLEEPING;

  // Ensure magic is set so next wakeup knows it came from sleep
  *retainedMagic = MAGIC;

  // Configure button pin as wakeup source
  pinMode(PIN_BTN, INPUT_PULLUP);

  // Enter system off (deepest sleep, ~2.5µA)
  // Button press on PIN_BTN will trigger wakeup (full reboot into setup())
  NRF_POWER->SYSTEMOFF = 1;
  // Execution never reaches here
  while (1);
}
