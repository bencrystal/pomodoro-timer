// LED + Button diagnostic sketch
// On boot: flashes RED → GREEN → BLUE (1 sec each) to verify wiring
// Then: holds WHITE while button is pressed, off when released

#define PIN_BTN 1
#define PIN_R   2
#define PIN_G   3
#define PIN_B   10

void setRGB(uint8_t r, uint8_t g, uint8_t b) {
  analogWrite(PIN_R, r);
  analogWrite(PIN_G, g);
  analogWrite(PIN_B, b);
}

void setup() {
  pinMode(PIN_BTN, INPUT_PULLUP);
  pinMode(PIN_R, OUTPUT);
  pinMode(PIN_G, OUTPUT);
  pinMode(PIN_B, OUTPUT);

  setRGB(255, 0, 0);  delay(1000);  // RED
  setRGB(0, 255, 0);  delay(1000);  // GREEN
  setRGB(0, 0, 255);  delay(1000);  // BLUE
  setRGB(0, 0, 0);                  // OFF
}

void loop() {
  if (digitalRead(PIN_BTN) == LOW) {
    setRGB(255, 255, 255);  // WHITE while held
  } else {
    setRGB(0, 0, 0);        // OFF when released
  }
}
