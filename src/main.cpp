#include <Arduino.h>

// Seven 49E Hall sensors through one 74HC4051. GPIO18 drives the MOS.
//
// 74HC4051, powered at 3.3V:
//   VCC -> 3.3V.  GND, VEE, E -> black G.
//   S0 -> yellow 27, S1 -> yellow 14, S2 -> yellow 12, Z -> yellow 34.
//   Y0..Y6 -> 49E OUT 1..7.  Y7 unused.
// Each 49E: VCC -> 3.3V, GND -> black G, marked face outward.
//
// Each sensor is compared with its own idle voltage, taken at boot with
// no magnet nearby. N is a drop, S is a rise. Idle levels differ per
// channel, so a fixed threshold falsely treats the low channels as N.
//
// SG90 on GPIO13: orange wire to yellow 13, red wire to a red 5V pin,
// brown wire to black G. S present turns it to 180 degrees. Otherwise 0.

static constexpr int kMosPin = 18;
static constexpr int kZPin = 34;
static constexpr int kS0Pin = 27;
static constexpr int kS1Pin = 14;
static constexpr int kS2Pin = 12;
static constexpr int kSensorCount = 7;

static constexpr int kAdcMax = 4095;
static constexpr int kAdcFullScaleMv = 3300;
static constexpr int kDeltaMv = 250;
static constexpr uint32_t kBlinkHalfMs = 250;
static constexpr int kSamples = 8;
static constexpr int kCalRounds = 16;
static constexpr int kServoPin = 13;
static constexpr int kServoChannel = 0;
static constexpr int kServoBits = 16;
static constexpr int kServoPeriodUs = 20000;
static constexpr int kPulse0Us = 544;
static constexpr int kPulse180Us = 2400;

enum class Lamp : uint8_t { Off, Solid, Blink };

static int gIdleMv[kSensorCount];

static void selectChannel(int channel) {
  digitalWrite(kS0Pin, channel & 0x1);
  digitalWrite(kS1Pin, (channel >> 1) & 0x1);
  digitalWrite(kS2Pin, (channel >> 2) & 0x1);
}

static int readChannelMv(int channel) {
  selectChannel(channel);
  delayMicroseconds(800);
  analogRead(kZPin);

  long sum = 0;
  for (int i = 0; i < kSamples; ++i) {
    sum += analogRead(kZPin);
    delayMicroseconds(80);
  }
  return static_cast<int>((sum / kSamples) * kAdcFullScaleMv / kAdcMax);
}

static void calibrateIdle() {
  long sum[kSensorCount] = {};
  for (int round = 0; round < kCalRounds; ++round) {
    for (int i = 0; i < kSensorCount; ++i) {
      sum[i] += readChannelMv(i);
    }
  }
  for (int i = 0; i < kSensorCount; ++i) {
    gIdleMv[i] = static_cast<int>(sum[i] / kCalRounds);
  }
}

static void writeServo(bool southPresent) {
  static int lastAngle = -1;
  const int angle = southPresent ? 180 : 0;
  if (angle == lastAngle) {
    return;
  }
  lastAngle = angle;
  const int pulseUs = southPresent ? kPulse180Us : kPulse0Us;
  const uint32_t duty =
      static_cast<uint32_t>(pulseUs) * ((1u << kServoBits) - 1) / kServoPeriodUs;
  ledcWrite(kServoChannel, duty);
}

static void setLamp(Lamp lamp) {
  static bool blinkOn = false;
  static uint32_t lastToggleMs = 0;

  if (lamp == Lamp::Solid) {
    digitalWrite(kMosPin, HIGH);
    return;
  }
  if (lamp == Lamp::Off) {
    digitalWrite(kMosPin, LOW);
    blinkOn = false;
    return;
  }

  const uint32_t now = millis();
  if (now - lastToggleMs >= kBlinkHalfMs) {
    lastToggleMs = now;
    blinkOn = !blinkOn;
    digitalWrite(kMosPin, blinkOn ? HIGH : LOW);
  }
}

void setup() {
  digitalWrite(kS2Pin, LOW);
  pinMode(kS2Pin, OUTPUT);
  digitalWrite(kS2Pin, LOW);

  digitalWrite(kS0Pin, LOW);
  digitalWrite(kS1Pin, LOW);
  pinMode(kS0Pin, OUTPUT);
  pinMode(kS1Pin, OUTPUT);

  digitalWrite(kMosPin, LOW);
  pinMode(kMosPin, OUTPUT);
  digitalWrite(kMosPin, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(kZPin, ADC_11db);

  ledcSetup(kServoChannel, 50, kServoBits);
  ledcAttachPin(kServoPin, kServoChannel);
  writeServo(false);

  Serial.begin(115200);
  Serial.println("Calibrating. Keep magnets away.");
  calibrateIdle();
  Serial.print("idle");
  for (int i = 0; i < kSensorCount; ++i) {
    Serial.printf(" %d:%d", i + 1, gIdleMv[i]);
  }
  Serial.println();
}

void loop() {
  int mv[kSensorCount];
  bool anyNorth = false;
  bool anySouth = false;

  for (int i = 0; i < kSensorCount; ++i) {
    mv[i] = readChannelMv(i);
    const int delta = mv[i] - gIdleMv[i];
    if (delta <= -kDeltaMv) {
      anyNorth = true;
    } else if (delta >= kDeltaMv) {
      anySouth = true;
    }
  }

  Lamp lamp = Lamp::Off;
  const char *lampName = "off";
  if (anyNorth) {
    lamp = Lamp::Solid;
    lampName = "solid";
  } else if (anySouth) {
    lamp = Lamp::Blink;
    lampName = "blink";
  }
  setLamp(lamp);
  writeServo(anySouth);

  for (int i = 0; i < kSensorCount; ++i) {
    const int delta = mv[i] - gIdleMv[i];
    Serial.printf("%d:%d", i + 1, mv[i]);
    if (delta <= -kDeltaMv) {
      Serial.print("N");
    } else if (delta >= kDeltaMv) {
      Serial.print("S");
    }
    Serial.print(i + 1 < kSensorCount ? "  " : "");
  }
  Serial.printf("  -> %s servo%d\n", lampName, anySouth ? 180 : 0);

  delay(200);
}
