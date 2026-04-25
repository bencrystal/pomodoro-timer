// =====================================================
// Keychain Pomodoro Timer — XIAO nRF52840 (v2)
// =====================================================
// Wake: click = 25min, hold = 45min
// Running: click = pause, hold = cancel+sleep
// Paused: click = resume, hold = cancel+sleep
// Done: click = sleep
// Triple click: battery check
// 5 rapid clicks on wake: BLE config mode
// =====================================================

#include <Arduino.h>
#include <bluefruit.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

// ── Wake detection via GPREGRET ───────────────────
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

#define LONG_PRESS_MS     600
#define TRIPLE_CLICK_MS   500
#define DEBOUNCE_MS       50

#define CONFIG_CLICKS     5
#define CONFIG_WINDOW_MS  2500
#define CONFIG_TIMEOUT_MS (2UL * 60 * 1000)

// ── Settings ─────────────────────────────────────
#define SETTINGS_FILENAME "/pomodoro.cfg"
#define SETTINGS_VERSION  1

struct __attribute__((packed)) PomodoroSettings {
  uint8_t  version;
  uint16_t shortDurationMin;
  uint16_t longDurationMin;
  uint8_t  warningMin;
  uint8_t  shortR, shortG, shortB;
  uint8_t  longR, longG, longB;
  uint8_t  warnR, warnG, warnB;
  uint8_t  breatheSpeed10;
  uint8_t  warnBreatheSpeed10;
  uint8_t  pauseTimeoutMin;
};

PomodoroSettings settings;
File cfgFile(InternalFS);

// Derived values (computed from settings)
unsigned long durationShort;
unsigned long durationLong;
unsigned long warningThreshold;
unsigned long pauseTimeout;

void loadDefaults() {
  settings.version            = SETTINGS_VERSION;
  settings.shortDurationMin   = 25;
  settings.longDurationMin    = 45;
  settings.warningMin         = 5;
  settings.shortR = 0;   settings.shortG = 255; settings.shortB = 40;
  settings.longR  = 40;  settings.longG  = 80;  settings.longB  = 255;
  settings.warnR  = 255; settings.warnG  = 80;  settings.warnB  = 0;
  settings.breatheSpeed10     = 12;
  settings.warnBreatheSpeed10 = 25;
  settings.pauseTimeoutMin    = 10;
}

void saveSettings() {
  if (InternalFS.exists(SETTINGS_FILENAME)) {
    InternalFS.remove(SETTINGS_FILENAME);
  }
  cfgFile.open(SETTINGS_FILENAME, FILE_O_WRITE);
  cfgFile.write((uint8_t*)&settings, sizeof(settings));
  cfgFile.close();
}

void loadSettings() {
  InternalFS.begin();
  if (InternalFS.exists(SETTINGS_FILENAME)) {
    cfgFile.open(SETTINGS_FILENAME, FILE_O_READ);
    if (cfgFile) {
      cfgFile.read(&settings, sizeof(settings));
      cfgFile.close();
      if (settings.version == SETTINGS_VERSION) return;
    }
  }
  loadDefaults();
  saveSettings();
}

void applySettings() {
  durationShort    = (unsigned long)settings.shortDurationMin * 60000UL;
  durationLong     = (unsigned long)settings.longDurationMin  * 60000UL;
  warningThreshold = (unsigned long)settings.warningMin       * 60000UL;
  pauseTimeout     = (unsigned long)settings.pauseTimeoutMin  * 60000UL;
}

// ── State ────────────────────────────────────────
enum State { SLEEPING, RUNNING, PAUSED, DONE };
State state = SLEEPING;

unsigned long timerDuration   = 0;
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

State         savedState      = SLEEPING;
unsigned long savedPausedMillis  = 0;
unsigned long savedPauseStarted  = 0;

// Config mode detection: count clicks in first few seconds after wake
unsigned long bootTime          = 0;
int           bootClickCount    = 0;
bool          configWindowOpen  = false;

// ── LED ───────────────────────────────────────────
void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

void ledsOff() {
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

  while (digitalRead(PIN_BTN) == LOW) delay(10);
  delay(50);

  NRF_POWER->GPREGRET = SLEEP_MAGIC;

  uint8_t sd_en = 0;
  sd_softdevice_is_enabled(&sd_en);
  if (sd_en) {
    sd_softdevice_disable();
  }

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

// ── BLE Config Mode ──────────────────────────────
#define POMO_SERVICE_UUID      "19b10000-e8f2-537e-4f6c-d104768a1214"
#define CHAR_SHORT_DUR_UUID    "19b10001-e8f2-537e-4f6c-d104768a1214"
#define CHAR_LONG_DUR_UUID     "19b10002-e8f2-537e-4f6c-d104768a1214"
#define CHAR_WARN_THRESH_UUID  "19b10003-e8f2-537e-4f6c-d104768a1214"
#define CHAR_SHORT_COLOR_UUID  "19b10004-e8f2-537e-4f6c-d104768a1214"
#define CHAR_LONG_COLOR_UUID   "19b10005-e8f2-537e-4f6c-d104768a1214"
#define CHAR_WARN_COLOR_UUID   "19b10006-e8f2-537e-4f6c-d104768a1214"
#define CHAR_BREATHE_UUID      "19b10007-e8f2-537e-4f6c-d104768a1214"
#define CHAR_WARN_BREATHE_UUID "19b10008-e8f2-537e-4f6c-d104768a1214"
#define CHAR_PAUSE_TOUT_UUID   "19b10009-e8f2-537e-4f6c-d104768a1214"
#define CHAR_SAVE_UUID         "19b1000a-e8f2-537e-4f6c-d104768a1214"

BLEService        pomoService(POMO_SERVICE_UUID);
BLECharacteristic charShortDur(CHAR_SHORT_DUR_UUID);
BLECharacteristic charLongDur(CHAR_LONG_DUR_UUID);
BLECharacteristic charWarnThresh(CHAR_WARN_THRESH_UUID);
BLECharacteristic charShortColor(CHAR_SHORT_COLOR_UUID);
BLECharacteristic charLongColor(CHAR_LONG_COLOR_UUID);
BLECharacteristic charWarnColor(CHAR_WARN_COLOR_UUID);
BLECharacteristic charBreathe(CHAR_BREATHE_UUID);
BLECharacteristic charWarnBreathe(CHAR_WARN_BREATHE_UUID);
BLECharacteristic charPauseTout(CHAR_PAUSE_TOUT_UUID);
BLECharacteristic charSave(CHAR_SAVE_UUID);

void onShortDurWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 2) {
    uint16_t val = data[0] | (data[1] << 8);
    if (val >= 1 && val <= 120) settings.shortDurationMin = val;
  }
}

void onLongDurWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 2) {
    uint16_t val = data[0] | (data[1] << 8);
    if (val >= 1 && val <= 120) settings.longDurationMin = val;
  }
}

void onWarnThreshWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 1 && data[0] <= 30) settings.warningMin = data[0];
}

void onShortColorWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 3) { settings.shortR = data[0]; settings.shortG = data[1]; settings.shortB = data[2]; }
}

void onLongColorWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 3) { settings.longR = data[0]; settings.longG = data[1]; settings.longB = data[2]; }
}

void onWarnColorWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 3) { settings.warnR = data[0]; settings.warnG = data[1]; settings.warnB = data[2]; }
}

void onBreatheWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 1 && data[0] >= 5 && data[0] <= 50) settings.breatheSpeed10 = data[0];
}

void onWarnBreatheWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 1 && data[0] >= 5 && data[0] <= 50) settings.warnBreatheSpeed10 = data[0];
}

void onPauseToutWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  if (len >= 1 && data[0] >= 1 && data[0] <= 60) settings.pauseTimeoutMin = data[0];
}

void onSaveWrite(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  saveSettings();
  applySettings();
  blinkRGB(0, 255, 0, 3, 150, 100);
}

void setupBLEChar(BLECharacteristic &chr, uint8_t fixedLen, BLECharacteristic::write_cb_t writeCb) {
  chr.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE);
  chr.setPermission(SECMODE_OPEN, SECMODE_OPEN);
  chr.setFixedLen(fixedLen);
  chr.setWriteCallback(writeCb);
  chr.begin();
}

void enterConfigMode() {
  // Visual confirmation: blue triple blink
  blinkRGB(0, 80, 255, 3, 150, 100);

  Bluefruit.begin();
  Bluefruit.setName("Pomodoro Config");
  Bluefruit.setTxPower(0);

  pomoService.begin();

  // Duration characteristics (2 bytes each)
  setupBLEChar(charShortDur, 2, onShortDurWrite);
  charShortDur.write16(settings.shortDurationMin);

  setupBLEChar(charLongDur, 2, onLongDurWrite);
  charLongDur.write16(settings.longDurationMin);

  // Single-byte characteristics
  setupBLEChar(charWarnThresh, 1, onWarnThreshWrite);
  charWarnThresh.write8(settings.warningMin);

  // Color characteristics (3 bytes each)
  setupBLEChar(charShortColor, 3, onShortColorWrite);
  uint8_t sc[] = { settings.shortR, settings.shortG, settings.shortB };
  charShortColor.write(sc, 3);

  setupBLEChar(charLongColor, 3, onLongColorWrite);
  uint8_t lc[] = { settings.longR, settings.longG, settings.longB };
  charLongColor.write(lc, 3);

  setupBLEChar(charWarnColor, 3, onWarnColorWrite);
  uint8_t wc[] = { settings.warnR, settings.warnG, settings.warnB };
  charWarnColor.write(wc, 3);

  setupBLEChar(charBreathe, 1, onBreatheWrite);
  charBreathe.write8(settings.breatheSpeed10);

  setupBLEChar(charWarnBreathe, 1, onWarnBreatheWrite);
  charWarnBreathe.write8(settings.warnBreatheSpeed10);

  setupBLEChar(charPauseTout, 1, onPauseToutWrite);
  charPauseTout.write8(settings.pauseTimeoutMin);

  // Save characteristic (write-only trigger)
  charSave.setProperties(CHR_PROPS_WRITE);
  charSave.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  charSave.setFixedLen(1);
  charSave.setWriteCallback(onSaveWrite);
  charSave.begin();

  // Start advertising — service UUID in ad packet, name in scan response
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addService(pomoService);
  Bluefruit.ScanResponse.addName();
  Bluefruit.Advertising.start(0);

  // Config mode loop: cyan breathing + long-press exit
  unsigned long configStart = millis();
  while (millis() - configStart < CONFIG_TIMEOUT_MS) {
    // Bluetooth blue double-pulse pattern
    unsigned long pat = millis() % 1200;
    if (pat < 80)        setRGB(0, 80, 255);
    else if (pat < 160)  setRGB(0, 0, 0);
    else if (pat < 240)  setRGB(0, 80, 255);
    else                 setRGB(0, 0, 0);

    if (digitalRead(PIN_BTN) == LOW) {
      delay(DEBOUNCE_MS);
      if (digitalRead(PIN_BTN) == LOW) {
        unsigned long pressStart = millis();
        while (digitalRead(PIN_BTN) == LOW) delay(10);
        if (millis() - pressStart > LONG_PRESS_MS) break;
      }
    }
    delay(20);
  }

  // Exit config mode
  Bluefruit.Advertising.stop();
  if (Bluefruit.connected()) {
    Bluefruit.disconnect(Bluefruit.connHandle());
  }
  delay(100);

  goToSleep();
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

      if (clickCount == 1) {
        savedState = state;
        savedPausedMillis = pausedMillis;
        savedPauseStarted = pauseStarted;
      }

      onSingleClick();
    }
  }

  if (clickCount >= 3) {
    state = savedState;
    pausedMillis = savedPausedMillis;
    pauseStarted = savedPauseStarted;
    flashBattery();
    clickCount = 0;
  }

  if (clickCount > 0 && millis() - lastReleaseAt > TRIPLE_CLICK_MS) {
    clickCount = 0;
  }
}

// ── LED state ─────────────────────────────────────
void updateLED() {
  switch (state) {
    case RUNNING: {
      unsigned long remaining = timerDuration - elapsed;
      if (remaining <= warningThreshold) {
        breathe(settings.warnR, settings.warnG, settings.warnB,
                settings.warnBreatheSpeed10 / 10.0f);
      } else if (timerDuration == durationShort) {
        breathe(settings.shortR, settings.shortG, settings.shortB,
                settings.breatheSpeed10 / 10.0f);
      } else {
        breathe(settings.longR, settings.longG, settings.longB,
                settings.breatheSpeed10 / 10.0f);
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

  loadSettings();
  applySettings();

  delay(10);

  if (_wokeFromSleep) {
    if (digitalRead(PIN_BTN) == LOW) {
      unsigned long wakeTime = millis();
      while (digitalRead(PIN_BTN) == LOW) {
        if (millis() - wakeTime > LONG_PRESS_MS) {
          startTimer(durationLong);
          while (digitalRead(PIN_BTN) == LOW) delay(10);
          return;
        }
        delay(10);
      }
    }
    // Start short timer, but open config detection window
    startTimer(durationShort);
    bootTime = millis();
    bootClickCount = 1;  // the wake press counts as click 1
    configWindowOpen = true;
  } else {
    goToSleep();
  }
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  // Config detection window: simple polling, bypass normal button handler
  if (configWindowOpen) {
    if (millis() - bootTime > CONFIG_WINDOW_MS) {
      configWindowOpen = false;  // window expired, resume normal operation
    } else {
      int r = digitalRead(PIN_BTN);
      if (r == LOW && btnState == HIGH) {
        btnState = LOW;
      }
      if (r == HIGH && btnState == LOW) {
        btnState = HIGH;
        bootClickCount++;
        if (bootClickCount >= CONFIG_CLICKS) {
          configWindowOpen = false;
          enterConfigMode();
          return;
        }
      }
      updateLED();  // keep green breathing visible
      return;       // skip normal button/timer logic
    }
  }

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
    if (millis() - pauseStarted > pauseTimeout) {
      goToSleep();
      return;
    }
  }

  updateLED();
}
