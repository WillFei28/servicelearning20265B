#include <Arduino.h>

// Test: GPIO18 drives the MOS module. Three thin LED strips flash twice a second.
//
// Wiring on the ESP32S 38P Goouuu expansion board:
// - Voltage jumper on 5V, so every red V pin is 5V.
// - MOS signal plugs into the yellow pin silkscreened P18.
// - Three strip red wires go straight into spare red V pins.
// - MOS right-hand [+] terminal stays empty.
// - MOS right-hand [LOAD] terminal takes the three strip black wires.
// - MOS right-hand [-] terminal takes one black wire to a black G pin.
//
// High turns the strips on. Upload with the USB socket on the ESP32 module.
// The expansion board USB and DC jack only supply power.

static constexpr int kMosPin = 18;
static constexpr int kFlashesPerSecond = 2;
static constexpr uint32_t kHalfPeriodMs = 1000UL / (kFlashesPerSecond * 2);

void setup() {
  digitalWrite(kMosPin, LOW);
  pinMode(kMosPin, OUTPUT);
  digitalWrite(kMosPin, LOW);

  Serial.begin(115200);
  Serial.printf("MOS P18: %d flashes/sec, %lu ms on, %lu ms off\n",
                kFlashesPerSecond, kHalfPeriodMs, kHalfPeriodMs);
}

void loop() {
  digitalWrite(kMosPin, HIGH);
  Serial.println("on");
  delay(kHalfPeriodMs);

  digitalWrite(kMosPin, LOW);
  Serial.println("off");
  delay(kHalfPeriodMs);
}
