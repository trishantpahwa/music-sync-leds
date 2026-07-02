/* 
  Written by Trishant Pahwa
  Modified for SparkFun Sound Detector (SEN-12642) + WS2812B
*/

#include <FastLED.h>
FASTLED_USING_NAMESPACE

#define NUM_LEDS         300 // Total Number of LEDs
#define DATA_PIN          7 // Connect your Addressable LED Strip to this Pin.
#define LED_TYPE     WS2812B // WS2812B for addressable LEDs
#define COLOR_ORDER     GRB // Default Color Order
#define ENVELOPE_PIN     A0 // Envelope Pin of the Sparkfun Sound Detector Module (SEN-12642)

#define BRIGHTNESS      255 // Max brightness (0-255)
#define SATURATION      255 // Color saturation (0-255)
#define MIN_VAL           5 // Increased to ignore background signal spikes
#define MAX_VAL          30 // Drastically lowered: signal of 30 will now be full brightness
#define NOISE_FLOOR       4 // Increased to filter out background noise (like fans or distant voices)
#define HUE_INIT         10 // Initial color hue
#define HUE_CHANGE        2 // Hue rotation speed

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

void setup() { 
  Serial.begin(9600);
  pinMode(ENVELOPE_PIN, INPUT);
  
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  // Initialize baseline and smoothed with current room noise
  baseline = analogRead(ENVELOPE_PIN);
  smoothed = baseline;

  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::Black;
  }
  FastLED.show(); 
  Serial.println("Sound Reactivity Initialized...");
}

void loop() {
  int raw = analogRead(ENVELOPE_PIN);

  // Asymmetric attack/decay: faster rise for zero delay
  if (raw > smoothed)
    smoothed = (smoothed + raw) / 2; // Instant attack (was 3:1)
  else
    smoothed = (smoothed * 3 + raw) / 4; // Faster decay (was 7:1)

  // Dynamic baseline tracks ambient noise levels
  if (smoothed < baseline)
    baseline = (baseline * 3 + smoothed) / 4;
  else
    baseline = (baseline * 31 + smoothed) / 32; // Slower upward drift for stability

  int signal = smoothed - baseline;
  
  if (signal <= NOISE_FLOOR) {
    analogVal = 0;
  } else {
    // Map signal to a range we can use for visualization
    analogVal = constrain(signal, MIN_VAL, MAX_VAL);
  }

  // Debug Output (Commented for zero latency)
  Serial.print("Raw: "); Serial.print(raw);
  Serial.print("\tBaseline: "); Serial.print(baseline);
  Serial.print("\tSignal: "); Serial.print(signal);
  Serial.print("\tAnalogVal: "); Serial.println(analogVal);

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
  int dynamicDelay = map(analogVal, MIN_VAL, MAX_VAL, 10, 1); // Reduced max delay to prevent lag

  if (val > 0) {
    for (int i = 0; i < NUM_LEDS-1; i++) {
      leds[i] = leds[i+1];
    }
    leds[NUM_LEDS-1] = CHSV(dynamicHue += HUE_CHANGE, SATURATION, val);
    // Removed blocking delay(max(1, dynamicDelay))
  } else {
    fadeToBlackBy(leds, NUM_LEDS, 20);
  }
}

void LinearReactive() {
  if (analogVal == 0) {
    fadeToBlackBy(leds, NUM_LEDS, 30);
    return;
  }
  val = map(analogVal, 0, MAX_VAL, 0, NUM_LEDS);
  for(int i = 0; i < NUM_LEDS; i++) {
    if (i < val)
      leds[i] = CHSV(HUE_INIT+(HUE_CHANGE*i), SATURATION, BRIGHTNESS);
    else
      leds[i].nscale8(10);
  }
}

void BrightnessReactive() {
  if (analogVal <= NOISE_FLOOR + 1) { // Added threshold for complete silence
    fadeToBlackBy(leds, NUM_LEDS, 50); // Fast fade to black for crisp transitions
    return;
  }
  
  val = map(analogVal, MIN_VAL, MAX_VAL, 0, BRIGHTNESS);
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
  val = map(analogVal, 0, MAX_VAL, 0, NUM_LEDS/2);
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