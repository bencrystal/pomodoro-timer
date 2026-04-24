// =====================================================
// Keychain Pomodoro Timer — XIAO nRF52840 (v2)
// =====================================================
// Wake: click = 25min, hold = 45min
// Running: click = pause, hold = cancel+sleep
// Paused: click = resume, hold = cancel+sleep
// Done: click = sleep
// Triple click: battery check
// =====================================================

#include <Arduino.h>

// ── Wake detection via GPREGRET ───────────────────
// GPREGRET survives SYSTEMOFF. Read it in a constructor that runs
// before main() / Arduino core init — otherwise SoftDevice clears it.
#define SLEEP_MAGIC 0xAB
static volatile bool _wokeFromSleep = false;

void earlyWakeCheck() __attribute__((constructor));
void earlyWakeCheck() {
  if (NRF_POWER->GPREGRET == SLEEP_MAGIC) {
    _wokeFromSleep = true;
    NRF_POWER->GPREGRET = 0;
  }
}

#define PIN_BTN   1
#define PIN_R     2
#define PIN_G     3
#define PIN_B     10

#define DURATION_SHORT  (25UL * 60 * 1000)
#define DURATION_LONG   (45UL * 60 * 1000)
#define PAUSE_TIMEOUT   (10UL * 60 * 1000)

#define LONG_PRESS_MS     600
#define TRIPLE_CLICK_MS   500
#define DEBOUNCE_MS       50

enum State { SLEEPING, RUNNING, PAUSED, DONE };
State state = SLEEPING;

unsigned long timerDuration   = DURATION_SHORT;
unsigned long startMillis     = 0;
unsigned long pausedMillis    = 0;
unsigned long pauseStarted    = 0;
unsigned long elapsed         = 0;

int           btnState        = HIGH;
int           lastBtnState    = HIGH;
unsigned long btnPressedAt    = 0;
unsigned long lastReleaseAt   = 0;
int           clickCount      = 0;
bool          longPressHandled = false;

// Saved state for triple-click restore
State         savedState      = SLEEPING;
unsigned long savedPausedMillis  = 0;
unsigned long savedPauseStarted  = 0;

// ── LED ───────────────────────────────────────────
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

void ledsOff() {
  // pinMode(OUTPUT) disconnects any running PWM peripheral from the pin.
  // Without this, digitalWrite alone can't override active PWM and the
  // LED stays frozen at whatever color it was showing.
  pinMode(PIN_R, OUTPUT); digitalWrite(PIN_R, LOW);
  pinMode(PIN_G, OUTPUT); digitalWrite(PIN_G, LOW);
  pinMode(PIN_B, OUTPUT); digitalWrite(PIN_B, LOW);
}

void blinkRGB(uint8_t r, uint8_t g, uint8_t b, int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    setRGB(r, g, b);
    delay(onMs);
    setRGB(0, 0, 0);
    if (i < times - 1) delay(offMs);
  }
}

void breathe(uint8_t r, uint8_t g, uint8_t b, float speed) {
  float t = millis() / 1000.0 * speed;
  float brightness = (sin(t * 3.14159) + 1.0) / 2.0;
  brightness = brightness * brightness;
  setRGB(
    (uint8_t)(r * brightness),
    (uint8_t)(g * brightness),
    (uint8_t)(b * brightness)
  );
}

// ── Timer ─────────────────────────────────────────
void startTimer(unsigned long duration) {
  timerDuration = duration;
  startMillis   = millis();
  pausedMillis  = 0;
  pauseStarted  = 0;
  elapsed       = 0;
  state         = RUNNING;
}

// ── Sleep ─────────────────────────────────────────
void goToSleep() {
  state = SLEEPING;

  ledsOff();
  pinMode(PIN_R, INPUT_PULLDOWN);
  pinMode(PIN_G, INPUT_PULLDOWN);
  pinMode(PIN_B, INPUT_PULLDOWN);

  // Wait for button release — otherwise the held button immediately
  // triggers GPIO SENSE wakeup and the device restarts
  while (digitalRead(PIN_BTN) == LOW) delay(10);
  delay(50);  // debounce

  // Mark that we're entering sleep (read by earlyWakeCheck constructor on boot)
  NRF_POWER->GPREGRET = SLEEP_MAGIC;

  sd_softdevice_disable();

  nrf_gpio_cfg_sense_input(g_ADigitalPinMap[PIN_BTN],
                           NRF_GPIO_PIN_PULLUP,
                           NRF_GPIO_PIN_SENSE_LOW);

  NRF_POWER->SYSTEMOFF = 1;
  while (1);
}

// ── Battery ───────────────────────────────────────
float getBatteryVoltage() {
  pinMode(14, OUTPUT);
  digitalWrite(14, HIGH);
  delay(10);
  analogReadResolution(12);
  int raw = analogRead(PIN_VBAT);
  float voltage = raw * 3.3f / 4096.0f * 2.0f;
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
  if (pct > 75)      blinkRGB(0, 255, 40, 4, 200, 150);
  else if (pct > 50) blinkRGB(0, 255, 40, 3, 200, 150);
  else if (pct > 25) blinkRGB(255, 180, 0, 2, 200, 150);
  else               blinkRGB(255, 0, 0, 1, 200, 0);
  delay(300);
}

// ── Button handling ───────────────────────────────
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

void handleButtonEvent() {
  int reading = digitalRead(PIN_BTN);

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

  // Long press fires on hold
  if (reading == LOW && btnState == LOW && !longPressHandled) {
    if (millis() - btnPressedAt >= LONG_PRESS_MS) {
      longPressHandled = true;
      clickCount = 0;
      onLongPress();
    }
  }

  // Release detected — fire single click immediately
  if (reading == HIGH && btnState == LOW) {
    btnState = HIGH;
    if (!longPressHandled) {
      clickCount++;
      lastReleaseAt = millis();

      if (clickCount == 1) {
        // Save state before first click so triple-click can restore it
        savedState = state;
        savedPausedMillis = pausedMillis;
        savedPauseStarted = pauseStarted;
      }

      // Fire immediately — no waiting for triple-click window
      onSingleClick();
    }
  }

  // Triple click — restore state and show battery
  if (clickCount >= 3) {
    state = savedState;
    pausedMillis = savedPausedMillis;
    pauseStarted = savedPauseStarted;
    flashBattery();
    clickCount = 0;
  }

  // Reset click count after window expires
  if (clickCount > 0 && millis() - lastReleaseAt > TRIPLE_CLICK_MS) {
    clickCount = 0;
  }
}

// ── LED state ─────────────────────────────────────
void updateLED() {
  switch (state) {
    case RUNNING: {
      unsigned long remaining = timerDuration - elapsed;
      if (remaining <= 5UL * 60 * 1000) {
        breathe(255, 80, 0, 2.5);
      } else {
        breathe(0, 255, 40, 1.2);
      }
      break;
    }
    case PAUSED: {
      unsigned long t = millis() % 1300;
      if (t < 500) setRGB(255, 120, 0);
      else         setRGB(0, 0, 0);
      break;
    }
    case DONE: {
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

// =====================================================
// SETUP
// =====================================================
void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  ledsOff();

  delay(10);

  if (_wokeFromSleep) {
    // Woke from SYSTEMOFF. Check if button is still held for long/short.
    if (digitalRead(PIN_BTN) == LOW) {
      unsigned long wakeTime = millis();
      while (digitalRead(PIN_BTN) == LOW) {
        if (millis() - wakeTime > LONG_PRESS_MS) {
          startTimer(DURATION_LONG);
          while (digitalRead(PIN_BTN) == LOW) delay(10);
          return;
        }
        delay(10);
      }
    }
    // Button already released (short press) or held < 600ms
    startTimer(DURATION_SHORT);
  } else {
    // Cold boot / USB power — go to sleep
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
    if (millis() - pauseStarted > PAUSE_TIMEOUT) {
      goToSleep();
      return;
    }
  }

  updateLED();
}
