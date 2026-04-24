// Sleep + wake test — detect wake by checking if button is held on boot
// Cold boot (button not held): red 2s → off (sleeping)
// Wake (button held):          white LED stays on

#include <Arduino.h>

#define PIN_BTN 1
#define PIN_R   2
#define PIN_G   3
#define PIN_B   10

void goToSleep() {
  digitalWrite(PIN_R, LOW);
  digitalWrite(PIN_G, LOW);
  digitalWrite(PIN_B, LOW);
  pinMode(PIN_R, INPUT_PULLDOWN);
  pinMode(PIN_G, INPUT_PULLDOWN);
  pinMode(PIN_B, INPUT_PULLDOWN);

  sd_softdevice_disable();
  nrf_gpio_cfg_sense_input(g_ADigitalPinMap[PIN_BTN],
                           NRF_GPIO_PIN_PULLUP,
                           NRF_GPIO_PIN_SENSE_LOW);
  NRF_POWER->SYSTEMOFF = 1;
  while (1);
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_R, LOW);
  digitalWrite(PIN_G, LOW);
  digitalWrite(PIN_B, LOW);

  delay(10);  // let pullup stabilize

  if (digitalRead(PIN_BTN) == LOW) {
    // Button is held on boot = woke from sleep
    // Show WHITE
    digitalWrite(PIN_R, HIGH);
    digitalWrite(PIN_G, HIGH);
    digitalWrite(PIN_B, HIGH);
    // Wait for release, then next press → back to sleep
    while (digitalRead(PIN_BTN) == LOW) delay(10);
    delay(500);
    while (digitalRead(PIN_BTN) == HIGH) delay(10);
    goToSleep();
  } else {
    // No button held = cold boot → red flash then sleep
    digitalWrite(PIN_R, HIGH);
    delay(2000);
    digitalWrite(PIN_R, LOW);
    delay(300);
    goToSleep();
  }
}

void loop() {}
