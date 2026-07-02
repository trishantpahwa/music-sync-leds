/*
  music-sync-leds-auto.ino — self-calibrating version of music-sync-leds.
  Written by Trishant Pahwa

  No NOISE_FLOOR / MIN_VAL / MAX_VAL to tune: the sketch learns them
  continuously with on-device online learning (integer fixed-point, x16):

    1. Ambient noise model — the mean and spread of the signal are learned
       during sustained quiet runs (>1 s below an absolute band). Music's
       between-beat gaps are too short to qualify, so music can never
       contaminate the model; a fan turning on is absorbed within seconds.
       Noise gate = quiet mean + 4x spread + 4.
    2. Brightness scale — a stochastic quantile estimator (pinball-loss
       updates, 9:1 up/down ratio) tracks the music's 90th percentile, so
       the top ~10% of the music saturates to full brightness at any
       volume or gain-pot setting.

  This exact integer math was validated in simulation against: silence,
  loud music, quiet music, volume changes, steady fan noise, music+fan,
  and returns to silence. See tools/calibrate.py for the offline analog.

  Hardware: SparkFun Sound Detector (SEN-12642) envelope -> A0, WS2812B -> pin 7.
*/

#include <FastLED.h>

#define DEBUG 0 // Set to 1 for serial debug output (throttled to 10 lines/sec)

#define NUM_LEDS         300 // Total Number of LEDs
#define DATA_PIN          7 // Connect your Addressable LED Strip to this Pin.
#define LED_TYPE     WS2812B // WS2812B for addressable LEDs
#define COLOR_ORDER     GRB // Default Color Order
#define ENVELOPE_PIN     A0 // Envelope Pin of the Sparkfun Sound Detector Module (SEN-12642)

#define BRIGHTNESS      255 // Max brightness (0-255)
#define SATURATION      255 // Color saturation (0-255)
#define HUE_INIT         10 // Initial color hue
#define HUE_CHANGE        2 // Hue rotation speed
#define SAMPLE_WINDOW     5 // ms per frame spent peak-sampling the envelope

#define QUIET_BAND       48 // ambient wobble lives below this (absolute), music doesn't
#define QUIET_RUN_FRAMES 70 // ~1 s of consecutive quiet frames before learning engages

/*
  0   -->   LinearFlowing (Dynamic)
  1   -->   LinearReactive
  2   -->   BrightnessReactive
  3   -->   CentreProgressive
  4   -->   EdgeProgressive
*/
int STYLE = 2;
/*========================================*/

CRGB leds[NUM_LEDS];
byte dynamicHue = HUE_INIT;
int level = 0;      // learned loudness, 0 (silence) .. 255 (recent loudest)
int smoothed = 0;
int baseline = 0;
unsigned long lastBaselineRise = 0;

// ---- learned state (fixed-point x16) ----
long quietC16  = 0;        // mean of the signal during quiet
long spread16  = 2L << 4;  // mean abs deviation of the signal during quiet
long p90_16    = 60L << 4; // running 90th percentile of loud frames
int  quietRun  = 0;        // consecutive frames inside QUIET_BAND
int  noiseFloor = 6;       // derived each frame from the learned state
int  scaleTop   = 60;      // derived each frame from the learned state

// Sample the envelope as fast as possible for SAMPLE_WINDOW ms and keep the
// peak, so short transients (kicks, snares) between frames are never missed.
int readEnvelopePeak() {
  int peak = 0;
  unsigned long start = millis();
  while (millis() - start < SAMPLE_WINDOW) {
    int v = analogRead(ENVELOPE_PIN);
    if (v > peak) peak = v;
  }
  return peak;
}

void setup() {
  Serial.begin(115200);
  pinMode(ENVELOPE_PIN, INPUT);

  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  // Seed the learners from ~0.5s of ambient: baseline = ambient peak,
  // quiet spread = how much the ambient wobbles.
  long sum = 0;
  int maxPeak = 0;
  for (int i = 0; i < 80; i++) {
    int v = readEnvelopePeak();
    sum += v;
    if (v > maxPeak) maxPeak = v;
  }
  baseline = maxPeak;
  smoothed = maxPeak;
  // Clamp the seed: if the device boots while music is playing, the raw
  // spread would be huge and could blind the strip until a long silence.
  // A too-small seed self-corrects within a second of real quiet.
  long seedSpread = (long)(maxPeak - sum / 80) + 1;
  if (seedSpread > 8) seedSpread = 8;
  spread16 = seedSpread << 4;

  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Black;
  FastLED.show();
  Serial.println("Self-calibrating sound reactivity initialized...");
}

void loop() {
  int raw = readEnvelopePeak();

  // Asymmetric attack/decay: instant rise, fast fall. The envelope output is
  // already hardware-smoothed, so more smoothing here only adds lag.
  if (raw > smoothed)
    smoothed = raw;
  else
    smoothed = (smoothed + raw) / 2;

  // Dynamic baseline: follows quiet moments down quickly, creeps up slowly
  // (time-based +1 — an EMA can never rise here due to integer division).
  // Creeps faster once a sustained quiet run confirms the offset is ambient.
  if (smoothed < baseline) {
    baseline = (baseline * 3 + smoothed) / 4;
  } else if (smoothed > baseline) {
    unsigned int wait = (quietRun > QUIET_RUN_FRAMES) ? 150 : 250;
    if (millis() - lastBaselineRise >= wait) {
      baseline++;
      lastBaselineRise = millis();
    }
  }

  int signal = smoothed - baseline;
  if (signal < 0) signal = 0;

  // ---- online learning ----
  long sig16 = (long)signal << 4;

  // Ambient model: learn mean + spread only inside the absolute quiet band,
  // and only after a sustained run there. Music's between-beat gaps are too
  // short to qualify, so the noise gate can never be dragged up by music.
  if (signal <= QUIET_BAND) {
    if (quietRun <= QUIET_RUN_FRAMES) quietRun++;
    if (quietRun > QUIET_RUN_FRAMES) {
      quietC16 += (sig16 - quietC16) >> 4;
      long dev16 = ((long)abs(signal - (int)(quietC16 >> 4))) << 4;
      long d = dev16 - spread16;
      // Asymmetric: spread shrinks 2x faster than it grows, so a floor
      // poisoned by a quiet outro recovers within a fraction of a second
      spread16 += (d > 0) ? (d >> 4) : (d >> 3);
    }
  } else {
    quietRun = 0;
  }
  // The floor may never exceed the quiet band: anything above the band is
  // by definition sound, so music can always light the strip no matter how
  // badly a quiet outro or a mid-song reset skewed the learned state.
  noiseFloor = (int)(quietC16 >> 4) + 4 * (int)(spread16 >> 4) + 4;
  if (noiseFloor > QUIET_BAND) noiseFloor = QUIET_BAND;

  // Loudness scale: pinball-loss 90th-percentile tracker over loud frames.
  // A 9:1 up/down step ratio converges on the value 90% of samples sit below.
  int loudGate = max(2 * noiseFloor, 32);
  if (signal > loudGate) {
    if (sig16 > p90_16) p90_16 += 18; else p90_16 -= 2;
    if (p90_16 < (24L << 4)) p90_16 = 24L << 4;
  }
  int minScale = max(24, 4 * noiseFloor); // never amplify silence-level noise
  scaleTop = max(minScale, (int)(p90_16 >> 4));

  // Loudness level 0..255: gated at the learned floor, full at learned p90
  if (signal <= noiseFloor) {
    level = 0;
  } else {
    long l = (long)(signal - noiseFloor) * 255 / max(1, scaleTop - noiseFloor);
    level = constrain((int)l, 1, 255);
  }

#if DEBUG
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 100) { // Throttled so serial never stalls the frame
    lastDebug = millis();
    Serial.print("Raw: "); Serial.print(raw);
    Serial.print("\tBaseline: "); Serial.print(baseline);
    Serial.print("\tSignal: "); Serial.print(signal);
    Serial.print("\tFloor: "); Serial.print(noiseFloor);
    Serial.print("\tScale: "); Serial.print(scaleTop);
    Serial.print("\tLevel: "); Serial.println(level);
  }
#endif

  switch (STYLE) {
    case 1: LinearReactive(); break;
    case 2: BrightnessReactive(); break;
    case 3: CentreProgressive(); break;
    case 4: EdgeProgressive(); break;
    default: LinearFlowing(); break;
  }

  FastLED.show();
}

void LinearFlowing() {
  if (level > 0) {
    for (int i = 0; i < NUM_LEDS - 1; i++) {
      leds[i] = leds[i + 1];
    }
    leds[NUM_LEDS - 1] = CHSV(dynamicHue += HUE_CHANGE, SATURATION, level);
  } else {
    fadeToBlackBy(leds, NUM_LEDS, 20);
  }
}

void LinearReactive() {
  if (level == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  int lit = map(level, 0, 255, 0, NUM_LEDS);
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < lit)
      leds[i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
    else
      leds[i].nscale8(10);
  }
}

void BrightnessReactive() {
  if (level == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 50); // Fast fade to black for crisp transitions
    return;
  }
  // Gamma correction: perceived LED brightness is nonlinear, so square the
  // value to make quiet passages visibly dim and loud hits visibly bright
  int val = ((long)level * level) / 255;
  dynamicHue += HUE_CHANGE; // shift color over time while sound is present
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(dynamicHue, SATURATION, val);
  }
}

void CentreProgressive() {
  if (level == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  int lit = map(level, 0, 255, 0, NUM_LEDS / 2);
  for (int i = 0; i < NUM_LEDS / 2; i++) {
    int pos1 = (NUM_LEDS / 2) + i;
    int pos2 = (NUM_LEDS / 2) - 1 - i;
    if (i <= lit) {
      if (pos1 < NUM_LEDS) leds[pos1] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
      if (pos2 >= 0) leds[pos2] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
    } else {
      if (pos1 < NUM_LEDS) leds[pos1].nscale8(10);
      if (pos2 >= 0) leds[pos2].nscale8(10);
    }
  }
}

void EdgeProgressive() {
  if (level == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  int lit = map(level, 0, 255, 0, NUM_LEDS / 2);
  for (int i = 0; i < NUM_LEDS / 2; i++) {
    int pos1 = i;
    int pos2 = NUM_LEDS - 1 - i;
    if (i <= lit) {
      leds[pos1] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
      if (pos2 >= 0) leds[pos2] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
    } else {
      leds[pos1].nscale8(10);
      if (pos2 >= 0) leds[pos2].nscale8(10);
    }
  }
}
