#include <Arduino.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

#include "crossing_mp3.h"

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
//
// MAX98357A: LRC -> yellow 33, BCLK -> yellow 25, DIN -> yellow 26.
// VIN -> red 5V, GND -> black G. Leave GAIN and SD empty.
// The speaker connects to the module's speaker terminals.
// While any S is present the 4 second crossing clip loops.

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
static constexpr uint32_t kScanMs = 200;
static constexpr int kSamples = 8;
static constexpr int kCalRounds = 16;
static constexpr int kServoPin = 13;
static constexpr int kServoChannel = 0;
static constexpr int kServoBits = 16;
static constexpr int kServoPeriodUs = 20000;
static constexpr int kPulse0Us = 544;
static constexpr int kPulse180Us = 2400;
static constexpr int kLrcPin = 33;
static constexpr int kBclkPin = 25;
static constexpr int kDinPin = 26;

enum class Lamp : uint8_t { Off, Solid, Blink };

static int gIdleMv[kSensorCount];
static Lamp gLamp = Lamp::Off;
static const char *gLampName = "off";
static bool gAnySouth = false;

static AudioOutputI2S *gOut = nullptr;
static AudioFileSourcePROGMEM *gFile = nullptr;
static AudioGeneratorMP3 *gMp3 = nullptr;
static bool gSoundOn = false;
static uint32_t gNextStartMs = 0;

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

static void initSound() {
  gOut = new AudioOutputI2S();
  gFile = new AudioFileSourcePROGMEM(kCrossingMp3, kCrossingMp3Len);
  const int bytes = AudioGeneratorMP3::preAllocSize();
  void *space = malloc(bytes);
  gMp3 = new AudioGeneratorMP3(space, bytes);
  if (!gOut || !gFile || !gMp3 || space == nullptr) {
    Serial.println("sound alloc failed");
    return;
  }
  gOut->SetPinout(kBclkPin, kLrcPin, kDinPin);
  gOut->SetGain(0.7);
  gOut->SetOutputModeMono(true);
}

static bool startSound() {
  if (gOut == nullptr || gFile == nullptr || gMp3 == nullptr) {
    return false;
  }
  if (gMp3->isRunning()) {
    return true;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - gNextStartMs) < 0) {
    return false;
  }
  if (!gFile->isOpen()) {
    gFile->open(kCrossingMp3, kCrossingMp3Len);
  } else {
    gFile->seek(0, SEEK_SET);
  }
  gSoundOn = gMp3->begin(gFile, gOut);
  if (!gSoundOn) {
    gNextStartMs = now + 1000;
    Serial.println("sound begin failed");
  }
  return gSoundOn;
}

static void stopSound() {
  if (gMp3 != nullptr && gMp3->isRunning()) {
    gMp3->stop();
  }
  gSoundOn = false;
}

static void serviceSound(bool want) {
  if (!want) {
    if (gSoundOn) {
      stopSound();
    }
    return;
  }
  if (!gSoundOn || gMp3 == nullptr || !gMp3->isRunning()) {
    startSound();
    return;
  }
  if (!gMp3->loop()) {
    stopSound();
    startSound();
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
  initSound();
  Serial.print("idle");
  for (int i = 0; i < kSensorCount; ++i) {
    Serial.printf(" %d:%d", i + 1, gIdleMv[i]);
  }
  Serial.println();
}

void loop() {
  static uint32_t lastScanMs = 0;
  const uint32_t now = millis();

  if (now - lastScanMs >= kScanMs) {
    lastScanMs = now;
    bool anyNorth = false;
    gAnySouth = false;
    int mv[kSensorCount];

    for (int i = 0; i < kSensorCount; ++i) {
      mv[i] = readChannelMv(i);
      const int delta = mv[i] - gIdleMv[i];
      if (delta <= -kDeltaMv) {
        anyNorth = true;
      } else if (delta >= kDeltaMv) {
        gAnySouth = true;
      }
    }

    gLamp = Lamp::Off;
    gLampName = "off";
    if (anyNorth) {
      gLamp = Lamp::Solid;
      gLampName = "solid";
    } else if (gAnySouth) {
      gLamp = Lamp::Blink;
      gLampName = "blink";
    }
    writeServo(gAnySouth);
    serviceSound(gAnySouth);

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
    Serial.printf("  -> %s servo%d%s\n", gLampName, gAnySouth ? 180 : 0,
                  gSoundOn ? " snd" : "");
  }

  setLamp(gLamp);
  serviceSound(gAnySouth);
}
