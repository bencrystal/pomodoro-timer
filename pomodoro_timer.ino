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
#define PIN_BTN   1    // D1 — B3F button (other leg to GND, internal pullup)
#define PIN_R     2    // D2
#define PIN_G     3    // D3
#define PIN_B     10   // D10

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

// ── Runtime variables ─────────────────────────────
unsigned long timerDuration   = DURATION_SHORT;
unsigned long startMillis     = 0;
unsigned long pausedMillis    = 0;   // accumulated pause duration in ms
unsigned long pauseStarted    = 0;
unsigned long elapsed         = 0;

// Button state
int           btnState        = HIGH;
int           lastBtnState    = HIGH;
unsigned long btnPressedAt    = 0;
unsigned long lastReleaseAt   = 0;
int           clickCount      = 0;
bool          longPressHandled = false;

// ── Forward declarations ──────────────────────────
void goToSleep();
void startTimer(unsigned long duration);
void handleButtonEvent();
void updateLED();
void breathe(uint8_t r, uint8_t g, uint8_t b, float speed);
void setRGB(uint8_t r, uint8_t g, uint8_t b);
void flashBattery();
void blinkRGB(uint8_t r, uint8_t g, uint8_t b, int times, int onMs, int offMs);
float getBatteryVoltage();
int   getBatteryPercent();
void onSingleClick();
void onLongPress();

// ── Wake detection ────────────────────────────────
// SYSTEMOFF causes a full reboot; bit 16 of RESETREAS = GPIO wakeup
bool wokeFromGPIO() {
  uint32_t reason = NRF_POWER->RESETREAS;
  NRF_POWER->RESETREAS = 0xFFFFFFFF;   // clear after reading
  return (reason & 0x00010000) != 0;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  setRGB(0, 0, 0);

  if (wokeFromGPIO()) {
    // Time how long button is held to distinguish short vs long press
    unsigned long wakeTime = millis();
    while (digitalRead(PIN_BTN) == LOW) {
      if (millis() - wakeTime > LONG_PRESS_MS) {
        startTimer(DURATION_LONG);
        while (digitalRead(PIN_BTN) == LOW) delay(10);  // wait for release
        return;
      }
      delay(10);
    }
    startTimer(DURATION_SHORT);
  } else {
    // Cold boot / USB power — go straight to sleep
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

  // While held — detect long press (fires on hold, not release)
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
    if (!longPressHandled) {
      clickCount++;
      lastReleaseAt = millis();
    }
  }

  // Evaluate click count after triple-click window expires
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
      state = PAUSED;
      pauseStarted = millis();
      break;

    case PAUSED:
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
        breathe(255, 80, 0, 2.5);    // last 5 min — faster amber
      } else {
        breathe(0, 255, 40, 1.2);   // normal — slow green
      }
      break;
    }

    case PAUSED: {
      // 500ms on, 800ms off
      unsigned long t = millis() % 1300;
      if (t < 500) setRGB(255, 120, 0);
      else         setRGB(0, 0, 0);
      break;
    }

    case DONE: {
      // Fast strobe: 100ms white / 100ms red
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

// Smooth sine-wave breathing — brightness = sin²(t·π·speed)
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
  // Pull pin 14 HIGH to enable battery measurement on XIAO nRF52840
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);
  delay(10);

  analogReadResolution(12);
  int raw = analogRead(PIN_VBAT);
  float voltage = raw * 3.3f / 4096.0f * 2.0f;  // ×2 for onboard voltage divider

  digitalWrite(14, LOW);
  return voltage;
}

int getBatteryPercent() {
  float v = getBatteryVoltage();
  if (v >= 4.2f) return 100;
  if (v <= 3.2f) return 0;
  return (int)((v - 3.2f) / 1.0f * 100.0f);  // linear 3.2V–4.2V → 0–100%
}

void flashBattery() {
  int pct = getBatteryPercent();
  setRGB(0, 0, 0);
  delay(300);

  if (pct > 75) {
    blinkRGB(0, 255, 40, 4, 200, 150);   // 4× green  (>75%)
  } else if (pct > 50) {
    blinkRGB(0, 255, 40, 3, 200, 150);   // 3× green  (50–75%)
  } else if (pct > 25) {
    blinkRGB(255, 180, 0, 2, 200, 150);  // 2× yellow (25–50%)
  } else {
    blinkRGB(255, 0, 0, 1, 200, 0);      // 1× red    (<25%)
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

  // Configure GPIO SENSE wakeup BEFORE entering SYSTEMOFF.
  // Without this the device sleeps but never wakes on button press.
  NRF_GPIO->PIN_CNF[g_ADigitalPinMap[PIN_BTN]] =
    (GPIO_PIN_CNF_SENSE_Low    << GPIO_PIN_CNF_SENSE_Pos) |
    (GPIO_PIN_CNF_PULL_Pullup  << GPIO_PIN_CNF_PULL_Pos)  |
    (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
    (GPIO_PIN_CNF_DIR_Input    << GPIO_PIN_CNF_DIR_Pos);

  // Deep sleep — ~2.5µA. Button press triggers full reboot into setup().
  NRF_POWER->SYSTEMOFF = 1;
  while (1);  // unreachable; suppresses compiler warnings
}
