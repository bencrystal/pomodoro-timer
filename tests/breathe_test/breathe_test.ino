// Breathing test — confirms which firmware is running and tests PWM breathing
// Boot: flashes RED-GREEN-BLUE quickly, then breathes green in loop

#include <Arduino.h>

#define PIN_BTN 1
#define PIN_R   2
#define PIN_G   3
#define PIN_B   10

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  // Startup flash — if you see R-G-B, this firmware is loaded
  analogWrite(PIN_R, 255); analogWrite(PIN_G, 0);   analogWrite(PIN_B, 0);   delay(400);
  analogWrite(PIN_R, 0);   analogWrite(PIN_G, 255); analogWrite(PIN_B, 0);   delay(400);
  analogWrite(PIN_R, 0);   analogWrite(PIN_G, 0);   analogWrite(PIN_B, 255); delay(400);
  analogWrite(PIN_R, 0);   analogWrite(PIN_G, 0);   analogWrite(PIN_B, 0);   delay(400);
}

void loop() {
  // Breathe green only — pin 3 should pulse, pins 2 and 10 should stay off
  float t = millis() / 1000.0 * 1.2;
  float brightness = (sin(t * 3.14159) + 1.0) / 2.0;
  brightness = brightness * brightness;

  analogWrite(PIN_R, 0);
  analogWrite(PIN_G, (int)(255.0 * brightness));
  analogWrite(PIN_B, 0);
}
