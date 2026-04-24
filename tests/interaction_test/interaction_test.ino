// Interaction test — full button/LED logic, no sleep
// Boot: R-G-B flash then starts 25min timer
// Single click: pause/resume
// Long press: cancel (LED off, stays off)
// Triple click: battery check

#include <Arduino.h>

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
  // No SYSTEMOFF — just sit idle with LED off
}

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

void onSingleClick() {
  switch (state) {
    case SLEEPING:
      // Without real sleep, single click from idle starts 25min
      startTimer(DURATION_SHORT);
      break;
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
  }
}

void onLongPress() {
  switch (state) {
    case SLEEPING:
      // Long press from idle starts 45min
      startTimer(DURATION_LONG);
      break;
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

  if (reading == LOW && btnState == HIGH) {
    btnPressedAt = millis();
    longPressHandled = false;
    btnState = LOW;
  }

  if (reading == LOW && btnState == LOW && !longPressHandled) {
    if (millis() - btnPressedAt >= LONG_PRESS_MS) {
      longPressHandled = true;
      clickCount = 0;
      onLongPress();
    }
  }

  if (reading == HIGH && btnState == LOW) {
    btnState = HIGH;
    if (!longPressHandled) {
      clickCount++;
      lastReleaseAt = millis();
    }
  }

  if (clickCount > 0 && millis() - lastReleaseAt > TRIPLE_CLICK_MS) {
    if (clickCount >= 3) {
      flashBattery();
    } else {
      onSingleClick();
    }
    clickCount = 0;
  }
}

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

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  // Startup flash to confirm firmware
  setRGB(255, 0, 0); delay(300);
  setRGB(0, 255, 0); delay(300);
  setRGB(0, 0, 255); delay(300);
  setRGB(0, 0, 0);   delay(300);

  // Start 25min timer immediately
  startTimer(DURATION_SHORT);
}

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
