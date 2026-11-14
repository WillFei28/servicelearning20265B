#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <MFRC522.h>

#include "crossing_mp3.h"

// Seven 49E Hall sensors through one 74HC4051. GPIO18 drives the MOS.
// The expansion-board jumper is set to 5V, so every red pin is 5V.
//
// 74HC4051, single 3.3V supply:
//   VCC -> the 3.3V pin at the board edge.
//   VEE -> GND directly under that 3.3V pin. VEE is not 3.3V.
//   GND -> SD2 black.  E -> P27 black.  Y7 unused.
//   S0 -> yellow 12, S1 -> yellow 14, S2 -> yellow 27, Z -> yellow 13.
//   Y0..Y6 -> 49E OUT 1..7. Sensor VCC taps the 4051 VCC pin,
//   sensor GND taps the 4051 GND pin. Marked face outward.
// GPIO12 is a strap pin, so S0 is driven low before it becomes an output.
// GPIO13 is ADC2. Wi-Fi is unused, so that analog read is available.
//
// Each sensor is compared with its own idle voltage, taken at boot with
// no magnet nearby. N is a drop, S is a rise. Idle levels differ per
// channel, so a fixed threshold falsely treats the low channels as N.
//
// SG90 on GPIO32, same column: yellow 32, red 5V, black GND.
// S present turns it to 180 degrees. Otherwise 0.
// GPIO34 and GPIO35 are input-only, so the servo cannot use them.
//
// MAX98357A: LRC -> yellow 17, BCLK -> yellow 16, DIN -> yellow 4.
// VIN -> P4 red, GND -> P4 black. Leave GAIN and SD empty.
// While any S is present the crossing clip loops.
// A card tap interrupts it with one error or octopus beep, then the loop resumes.
//
// 0.96 inch SSD1306, 128x64, I2C, unchanged:
//   VCC -> VCC, GND -> GND, SCL -> D21, SDA -> D22.
// S present draws a cross. N present draws a tick. Neither leaves the screen blank.
//
// RC522, 3.3V only. Do not use a red 5V pin.
//   3.3V -> the middle 3.3V pin. GND -> black under that pin.
//   Both 3.3V pins are taken, so RST is GPIO23, not tied to 3.3V.
//   SDA -> yellow 26, SCK -> yellow 33, MOSI -> yellow 25, MISO -> yellow 34.
//   IRQ stays empty. P35 is input-only and stays empty.
// The latest N or S stays saved after the magnet leaves. A tap uses that
// saved record, then clears it. S shows -$2.0 and the error clip.
// N shows +$2.0 and the octopus beep. No saved record does neither.

static constexpr int kMosPin = 18;
static constexpr int kZPin = 13;
static constexpr int kS0Pin = 12;
static constexpr int kS1Pin = 14;
static constexpr int kS2Pin = 27;
static constexpr int kSensorCount = 7;

static constexpr int kAdcMax = 4095;
static constexpr int kAdcFullScaleMv = 3300;
static constexpr int kDeltaMv = 250;
static constexpr uint32_t kBlinkHalfMs = 250;
static constexpr uint32_t kScanMs = 200;
static constexpr int kSamples = 8;
static constexpr int kCalRounds = 16;
static constexpr int kServoPin = 32;
static constexpr int kServoChannel = 0;
static constexpr int kServoBits = 16;
static constexpr int kServoPeriodUs = 20000;
static constexpr int kPulse0Us = 544;
static constexpr int kPulse180Us = 2400;
static constexpr int kLrcPin = 17;
static constexpr int kBclkPin = 16;
static constexpr int kDinPin = 4;
static constexpr int kSdaPin = 22;
static constexpr int kSclPin = 21;
static constexpr int kScreenWidth = 128;
static constexpr int kScreenHeight = 64;
static constexpr uint8_t kScreenAddr = 0x3C;
static constexpr int kRfidSsPin = 26;
static constexpr int kRfidRstPin = 23;
static constexpr int kRfidSckPin = 33;
static constexpr int kRfidMosiPin = 25;
static constexpr int kRfidMisoPin = 34;
static constexpr uint32_t kFareMs = 2200;
static constexpr uint32_t kCardGoneMs = 800;
static constexpr uint32_t kCardPollMs = 40;

enum class Lamp : uint8_t { Off, Solid, Blink };

static int gIdleMv[kSensorCount];
static Lamp gLamp = Lamp::Off;
static const char *gLampName = "off";
static bool gAnySouth = false;

enum class Clip : uint8_t { None, Crossing, Error, Octopus };

class HeldOutput : public AudioOutputI2S {
 public:
  HeldOutput() : AudioOutputI2S(0, EXTERNAL_I2S, 32, APLL_DISABLE) {}

  bool stop() override {
    return i2sOn;
  }
};

static HeldOutput *gOut = nullptr;
static AudioFileSourcePROGMEM *gFile = nullptr;
static AudioGeneratorMP3 *gMp3 = nullptr;
static bool gSoundOn = false;
static Clip gClip = Clip::None;
static uint32_t gNextStartMs = 0;
static Adafruit_SSD1306 gDisplay(kScreenWidth, kScreenHeight, &Wire, -1);
static bool gDisplayOk = false;
static MFRC522 gRfid(kRfidSsPin, kRfidRstPin);
static bool gRfidOk = false;
static int gSavedPole = 0;
static bool gAcceptPole = true;
static int gPay = 0;
static uint32_t gPayUntil = 0;
static bool gCardHeld = false;
static uint32_t gLastCardMs = 0;

static void selectChannel(int channel) {
  digitalWrite(kS0Pin, channel & 0x1);
  digitalWrite(kS1Pin, (channel >> 1) & 0x1);
  digitalWrite(kS2Pin, (channel >> 2) & 0x1);
}

static void feedAudio() {
  if (gMp3 != nullptr && gMp3->isRunning()) {
    gMp3->loop();
  }
}

static int readChannelMv(int channel) {
  selectChannel(channel);
  feedAudio();
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

static void drawBoldLine(int x0, int y0, int x1, int y1, int radius) {
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy <= radius * radius) {
        gDisplay.drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, SSD1306_WHITE);
      }
    }
  }
}

static void drawTick(int x0, int y0, int x1, int y1, int x2, int y2, int radius) {
  drawBoldLine(x0, y0, x1, y1, radius);
  drawBoldLine(x1, y1, x2, y2, radius);
}

static void drawCross(int left, int top, int right, int bottom, int radius) {
  drawBoldLine(left, top, right, bottom, radius);
  drawBoldLine(right, top, left, bottom, radius);
}

static void initDisplay() {
  Wire.begin(kSdaPin, kSclPin);
  gDisplayOk = gDisplay.begin(SSD1306_SWITCHCAPVCC, kScreenAddr);
  if (!gDisplayOk) {
    Serial.println("display begin failed");
    return;
  }
  gDisplay.clearDisplay();
  gDisplay.display();
}

static void drawFare(const char *text) {
  const int size = 3;
  const int width = static_cast<int>(strlen(text)) * 6 * size;
  const int height = 8 * size;
  const int left = (kScreenWidth - width) / 2;
  const int top = (kScreenHeight - height) / 2;
  gDisplay.setTextSize(size);
  gDisplay.setTextColor(SSD1306_WHITE);
  gDisplay.setCursor(left < 0 ? 0 : left, top < 0 ? 0 : top);
  gDisplay.print(text);
}

static bool paymentActive() {
  return gClip == Clip::Error || gClip == Clip::Octopus;
}

static void showPoles(bool north, bool south) {
  if (!gDisplayOk) {
    return;
  }
  const uint32_t now = millis();
  if (gPay != 0 && static_cast<int32_t>(now - gPayUntil) >= 0 && !paymentActive()) {
    gPay = 0;
  }

  static int shown = -1;
  int mark = (north ? 2 : 0) | (south ? 1 : 0);
  if (gPay > 0) {
    mark = 8;
  } else if (gPay < 0) {
    mark = 16;
  }
  if (mark == shown) {
    return;
  }
  shown = mark;

  gDisplay.clearDisplay();
  if (gPay > 0) {
    drawFare("+$2.0");
  } else if (gPay < 0) {
    drawFare("-$2.0");
  } else if (south && north) {
    drawCross(34, 4, 94, 28, 2);
    drawTick(40, 42, 56, 58, 92, 34, 2);
  } else if (south) {
    drawCross(26, 8, 102, 56, 3);
  } else if (north) {
    drawTick(30, 34, 54, 56, 102, 10, 3);
  }
  gDisplay.display();
}

static void initSound() {
  gOut = new HeldOutput();
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

static bool beginClip(const uint8_t *data, unsigned len, Clip clip) {
  if (gOut == nullptr || gFile == nullptr || gMp3 == nullptr) {
    return false;
  }
  const uint32_t now = millis();
  if (clip == Clip::Crossing && static_cast<int32_t>(now - gNextStartMs) < 0) {
    return false;
  }
  if (gMp3->isRunning()) {
    gMp3->stop();
  }
  if (!gFile->open(data, static_cast<uint32_t>(len))) {
    gSoundOn = false;
    gClip = Clip::None;
    return false;
  }
  gSoundOn = gMp3->begin(gFile, gOut);
  if (!gSoundOn) {
    gNextStartMs = now + 1000;
    gClip = Clip::None;
    Serial.println("sound begin failed");
    return false;
  }
  gClip = clip;
  feedAudio();
  return true;
}

static bool startSound() {
  if (gClip == Clip::Crossing && gMp3 != nullptr && gMp3->isRunning()) {
    return true;
  }
  return beginClip(kCrossingMp3, kCrossingMp3Len, Clip::Crossing);
}

static void stopSound() {
  if (gMp3 != nullptr && gMp3->isRunning()) {
    gMp3->stop();
  }
  gSoundOn = false;
}

static void serviceSound(bool want) {
  if (paymentActive()) {
    if (gMp3 != nullptr && gMp3->isRunning() && gMp3->loop()) {
      return;
    }
    stopSound();
    gClip = Clip::None;
  }
  if (!want) {
    if (gSoundOn || gClip == Clip::Crossing) {
      stopSound();
      gClip = Clip::None;
    }
    return;
  }
  if (gClip != Clip::Crossing || !gSoundOn || gMp3 == nullptr || !gMp3->isRunning()) {
    startSound();
    return;
  }
  if (!gMp3->loop()) {
    stopSound();
    gClip = Clip::None;
    startSound();
  }
}

static void initRfid() {
  pinMode(kRfidRstPin, OUTPUT);
  digitalWrite(kRfidRstPin, HIGH);
  SPI.begin(kRfidSckPin, kRfidMisoPin, kRfidMosiPin, kRfidSsPin);
  gRfid.PCD_Init();
  digitalWrite(kRfidRstPin, HIGH);
  const byte version = gRfid.PCD_ReadRegister(MFRC522::VersionReg);
  gRfidOk = version != 0x00 && version != 0xFF;
  Serial.printf("rfid %s 0x%02x\n", gRfidOk ? "ok" : "fail", version);
}

static int nearestPole(const int *mv) {
  int best = -1;
  int bestAbs = kDeltaMv - 1;
  for (int i = 0; i < kSensorCount; ++i) {
    const int delta = mv[i] - gIdleMv[i];
    const int magnitude = delta < 0 ? -delta : delta;
    if (magnitude > bestAbs) {
      bestAbs = magnitude;
      best = i;
    }
  }
  if (best < 0) {
    return 0;
  }
  const int delta = mv[best] - gIdleMv[best];
  if (delta <= -kDeltaMv) {
    return 1;
  }
  if (delta >= kDeltaMv) {
    return -1;
  }
  return 0;
}

static void rememberPole(const int *mv) {
  const int pole = nearestPole(mv);
  if (pole == 0) {
    gAcceptPole = true;
    return;
  }
  if (gAcceptPole) {
    gSavedPole = pole;
  }
}

static void pollCard(uint32_t now) {
  if (!gRfidOk) {
    return;
  }
  const bool seen = gRfid.PICC_IsNewCardPresent() && gRfid.PICC_ReadCardSerial();
  if (seen) {
    gRfid.PICC_HaltA();
    gRfid.PCD_StopCrypto1();
    gLastCardMs = now;
    if (gCardHeld) {
      return;
    }
    gCardHeld = true;
    const int pole = gSavedPole;
    gSavedPole = 0;
    gAcceptPole = false;
    if (pole == 0) {
      Serial.println("card no record");
      return;
    }
    gPay = pole;
    gPayUntil = now + kFareMs;
    if (pole > 0) {
      beginClip(kOctopusMp3, kOctopusMp3Len, Clip::Octopus);
      Serial.println("card N +$2.0 reset");
    } else {
      beginClip(kErrorMp3, kErrorMp3Len, Clip::Error);
      Serial.println("card S -$2.0 reset");
    }
    return;
  }
  if (gCardHeld && now - gLastCardMs >= kCardGoneMs) {
    gCardHeld = false;
  }
}

void setup() {
  // GPIO12 (S0) chooses the flash voltage if it is high at boot.
  digitalWrite(kS0Pin, LOW);
  pinMode(kS0Pin, OUTPUT);
  digitalWrite(kS0Pin, LOW);

  digitalWrite(kS1Pin, LOW);
  digitalWrite(kS2Pin, LOW);
  pinMode(kS1Pin, OUTPUT);
  pinMode(kS2Pin, OUTPUT);
  digitalWrite(kS1Pin, LOW);
  digitalWrite(kS2Pin, LOW);

  digitalWrite(kMosPin, LOW);
  pinMode(kMosPin, OUTPUT);
  digitalWrite(kMosPin, LOW);

  analogReadResolution(12);
  analogSetPinAttenuation(kZPin, ADC_11db);

  ledcSetup(kServoChannel, 50, kServoBits);
  ledcAttachPin(kServoPin, kServoChannel);
  writeServo(false);

  Serial.begin(115200);
  initDisplay();
  initRfid();
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
  static uint32_t lastCardMs = 0;
  const uint32_t now = millis();

  if (now - lastCardMs >= kCardPollMs) {
    lastCardMs = now;
    pollCard(now);
    feedAudio();
  }

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
    rememberPole(mv);
    feedAudio();
    serviceSound(gAnySouth);
    showPoles(anyNorth, gAnySouth);

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
    Serial.printf("  -> %s servo%d%s%s%s\n", gLampName, gAnySouth ? 180 : 0,
                  gSoundOn ? " snd" : "",
                  gSavedPole > 0 ? " recN" : (gSavedPole < 0 ? " recS" : ""),
                  gPay > 0 ? " +$2.0" : (gPay < 0 ? " -$2.0" : ""));
  }

  setLamp(gLamp);
  feedAudio();
  serviceSound(gAnySouth);
}
