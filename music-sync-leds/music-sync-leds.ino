/* 
  Written by Trishant Pahwa
  Modified for SparkFun Sound Detector (SEN-12642) + WS2812B
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
#define MIN_VAL          13 // Just above idle signal wobble (0-9 in serial log)
#define MAX_VAL          38 // Starting scale; auto-gain adapts upward from here
#define NOISE_FLOOR      12 // Idle signal reaches 9 at current gain, +3 margin
#define HUE_INIT         10 // Initial color hue
#define HUE_CHANGE        2 // Hue rotation speed
#define SAMPLE_WINDOW     5 // ms per frame spent peak-sampling the envelope

/* 
  0   -->   LinearFlowing (Dynamic)   
  1   -->   LinearReactive            
  2   -->   BrightnessReactive        
  3   -->   CentreProgressive         
  4   -->   EdgeProgressive           
*/                                        
int STYLE = 2; // Switched to 2 for instant all-LED updates
/*========================================*/

CRGB leds[NUM_LEDS];
byte dynamicHue = HUE_INIT;
int analogVal = 0;
int val = 0;
int smoothed = 0;
int baseline = 0;
unsigned long lastBaselineRise = 0;
int peakLevel = MAX_VAL; // Auto-gain: loudest recent signal, decays over ~10 s
unsigned long lastPeakDecay = 0;

void setup() { 
  Serial.begin(115200);
  pinMode(ENVELOPE_PIN, INPUT);
  
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  // Initialize baseline with the ambient PEAK over ~0.5s, not a single read —
  // a lucky-low single sample would make quiet rooms register as sound.
  baseline = 0;
  for (int i = 0; i < 50; i++) {
    int v = readEnvelopePeak();
    if (v > baseline) baseline = v;
  }
  smoothed = baseline;

  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
  }
  FastLED.show(); 
  Serial.println("Sound Reactivity Initialized...");
}

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

void loop() {
  int raw = readEnvelopePeak();

  // Asymmetric attack/decay. Kept light: the Sound Detector's envelope
  // output is already hardware-smoothed, so heavy software smoothing here
  // only adds visible lag between the music and the strip.
  if (raw > smoothed)
    smoothed = raw; // Truly instant attack — beats register the same frame
  else
    smoothed = (smoothed + raw) / 2; // Fast decay so beats stay punchy

  // Dynamic baseline: follows quiet moments down quickly, creeps up slowly.
  // Note: a (baseline*31+smoothed)/32 filter can never rise here — integer
  // division needs smoothed >= baseline+32 to move — so ambient drift above
  // the boot-time reading would read as permanent "sound". Time-based +1
  // instead: ambient drift is absorbed within seconds, while music still
  // pulls the baseline back down in every beat gap before it can be eaten.
  if (smoothed < baseline) {
    baseline = (baseline * 3 + smoothed) / 4;
  } else if (smoothed > baseline && millis() - lastBaselineRise >= 250) {
    baseline++;
    lastBaselineRise = millis();
  }

  int signal = smoothed - baseline;

  // Auto-gain: track the loudest recent signal and scale against it, so the
  // full brightness range is used at any volume or gain-pot setting. Rises
  // instantly on a new peak, decays ~6% per 250 ms, and never drops below
  // MAX_VAL so silence-level noise is never amplified up to full scale.
  if (signal > peakLevel) {
    peakLevel = signal;
  } else if (peakLevel > MAX_VAL && millis() - lastPeakDecay >= 250) {
    peakLevel -= max(1, peakLevel / 16);
    if (peakLevel < MAX_VAL) peakLevel = MAX_VAL;
    lastPeakDecay = millis();
  }

  if (signal <= NOISE_FLOOR) {
    analogVal = 0;
  } else {
    // Scale so the recent loudest sound maps to MAX_VAL (full effect)
    analogVal = constrain((int)((long)signal * MAX_VAL / peakLevel), MIN_VAL, MAX_VAL);
  }

#if DEBUG
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug >= 100) { // Throttled so serial never stalls the frame
    lastDebug = millis();
    Serial.print("Raw: "); Serial.print(raw);
    Serial.print("\tBaseline: "); Serial.print(baseline);
    Serial.print("\tSignal: "); Serial.print(signal);
    Serial.print("\tPeak: "); Serial.print(peakLevel);
    Serial.print("\tAnalogVal: "); Serial.println(analogVal);
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
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, BRIGHTNESS);

  if (val > 0) {
    for (int i = 0; i < NUM_LEDS-1; i++) {
      leds[i] = leds[i+1];
    }
    leds[NUM_LEDS-1] = CHSV(dynamicHue += HUE_CHANGE, SATURATION, val);
  } else {
    fadeToBlackBy(leds, NUM_LEDS, 20);
  }
}

void LinearReactive() {
  if (analogVal == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, NUM_LEDS);
  for(int i = 0; i < NUM_LEDS; i++) {
    if (i < val)
      leds[i] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
    else
      leds[i].nscale8(10);
  }
}

void BrightnessReactive() {
  if (analogVal == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 50); // Fast fade to black for crisp transitions
    return;
  }
  
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, BRIGHTNESS);
  // Gamma correction: perceived LED brightness is nonlinear, so square the
  // value to make quiet passages visibly dim and loud hits visibly bright
  val = ((long)val * val) / 255;
  // Rotate the global hue slightly each frame to shift colors over time
  if (val > 0) dynamicHue += HUE_CHANGE;
  
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(dynamicHue, SATURATION, val);
  }
}

void CentreProgressive() {
  if (analogVal == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, NUM_LEDS/2);
  for(int i = 0; i < NUM_LEDS/2; i++) {
    int pos1 = (NUM_LEDS/2) + i;
    int pos2 = (NUM_LEDS/2) - 1 - i;
    if (i <= val) {
      if (pos1 < NUM_LEDS) leds[pos1] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
      if (pos2 >= 0) leds[pos2] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
    } else {
      if (pos1 < NUM_LEDS) leds[pos1].nscale8(10);
      if (pos2 >= 0) leds[pos2].nscale8(10);
    }
  }
}

void EdgeProgressive() {
  if (analogVal == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, NUM_LEDS/2);
  for(int i = 0; i < NUM_LEDS/2; i++) {
    int pos1 = i;
    int pos2 = NUM_LEDS - 1 - i;
    if (i <= val) {
      leds[pos1] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
      if (pos2 >= 0) leds[pos2] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
    } else {
      leds[pos1].nscale8(10);
      if (pos2 >= 0) leds[pos2].nscale8(10);
    }
  }
}