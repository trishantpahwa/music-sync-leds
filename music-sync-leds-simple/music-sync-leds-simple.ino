/* 
  Written by Trishant Pahwa (https://trishantpahwa.me)
  Inspired by Rupak Poddar's Music Reactive LED Strip (https://github.com/Rupakpoddar/Sound-reactive-LED-strip) @Rupakpoddar
  Enhanced for Audio Beat Reactivity & Decay Physics
  Last Updated: July 2026
*/

#include <FastLED.h>        // https://github.com/FastLED/FastLED

#define NUM_LEDS        300 // Total Number of LEDs
#define DATA_PIN          7 // Connect your Addressable LED Strip to this Pin.
#define LED_TYPE    WS2812B // WS2801, WS2811, WS2812B, LPD8806, TM1809, etc...
#define COLOR_ORDER     GRB // Default Color Order
#define ENVELOPE_PIN     A0 // Envelope Pin of the Sparkfun Sound Detector Module

#define BRIGHTNESS      255 // Min: 0, Max: 255
#define SATURATION      200 // Punchier color saturation for ambient bounce
#define HUE_INIT         10 // Starting color hue
#define HUE_CHANGE        1 // Slower color progression for smoother ambient gradients

// --- Dynamic Calibration & Tuning Settings ---
float runningMin = 40.0;        // Baseline audio noise floor guess
float runningMax = 400.0;       // Initial peak audio limit guess
const float decayRate = 0.0003; // Calibration drift rate (how fast it adjusts to volume changes)

float smoothedVal = 0;          // Blended value used for controlling movement speed
unsigned long lastShiftTime = 0; // Tracks non-blocking execution intervals

// --- Physics-Based Beat Decay Variables ---
float brightnessTracker = 0;     // Simulated physical decay curve variable
const float beatDecay = 0.88;    // Lower = faster drop to black, Higher = longer color trails
int lastAnalogVal = 0;          // Used to calculate frame-by-frame sound differentials

/*============= SELECT STYLE =============*/
/* */
/* 0   -->   LinearFlowing (BEST)      */
/* 1   -->   LinearReactive            */
/* 2   -->   BrightnessReactive        */
/* 3   -->   CentreProgressive         */
/* 4   -->   EdgeProgressive           */
/* */                                        
/* */          int STYLE = 0;          /* */
/* */
/*========================================*/

CRGB leds[NUM_LEDS];
byte dynamicHue = HUE_INIT;
int val = 0;

void setup() { 
  pinMode(ENVELOPE_PIN, INPUT);
  
  // Initialize FastLED library configuration
  FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(BRIGHTNESS);

  // Clear strip on boot up
  FastLED.clear();
  FastLED.show(); 
}

void loop() {
  int analogVal = analogRead(ENVELOPE_PIN);

  // --- 1. DYNAMIC AUTO-CALIBRATION ---
  // Slowly drift calibration bounds toward the ambient baseline
  runningMax = (runningMax * (1.0 - decayRate)) + ((float)analogVal * decayRate);
  runningMin = (runningMin * (1.0 - decayRate)) + ((float)analogVal * decayRate);

  // Expand bounds instantly on true audio peaks or drops
  if (analogVal > runningMax) runningMax = analogVal;
  if (analogVal < runningMin) runningMin = analogVal;

  int currentMin = (int)runningMin;
  int currentMax = max((int)runningMax, currentMin + 40); // Lock a safety buffer
  analogVal = constrain(analogVal, currentMin, currentMax);

  // Blend audio signals cleanly to drive movement timing shifts
  smoothedVal = (smoothedVal * 0.85) + ((float)analogVal * 0.15);

  // --- 2. PHYSICS-BASED BEAT DECAY ENGINE ---
  int delta = analogVal - lastAnalogVal;
  int currentTargetBrightness = map(analogVal, currentMin, currentMax, 0, BRIGHTNESS);

  // Detect a sudden audio jump (e.g., bass drum transients)
  if (delta > 15 && currentTargetBrightness > brightnessTracker) {
    brightnessTracker = currentTargetBrightness; // Snap to full impact power
  } else {
    brightnessTracker *= beatDecay;              // Otherwise, simulate a natural exponential drop-off
  }
  
  // Establish a faint ambient threshold so things don't flicker aggressively into pure darkness
  if (brightnessTracker < 15) brightnessTracker = 15; 
  
  lastAnalogVal = analogVal;

  // --- 3. ANIMATION ROUTINE CONTROLLER ---
  switch (STYLE) {
    case 1:
      LinearReactive(currentMin, currentMax);
      break;
    case 2:
      BrightnessReactive(currentMin, currentMax);
      break;
    case 3:
      CentreProgressive(currentMin, currentMax);
      break;
    case 4:
      EdgeProgressive(currentMin, currentMax);
      break;
    default:
      LinearFlowing(currentMin, currentMax);
      break;
  }
  
  // Render calculated frame elements to the strip
  FastLED.show();
}

// ============================= ANIMATION STYLES =============================

void LinearFlowing(int currentMin, int currentMax) {
  // Map shifting interval: higher volume drops interval, making it move faster
  unsigned long shiftInterval = map((int)smoothedVal, currentMin, currentMax, 22, 4); 

  if (millis() - lastShiftTime >= shiftInterval) {
    lastShiftTime = millis();
    
    // Shift pixels downwards along the length of the string array
    for (int i = 0; i < NUM_LEDS - 1; i++) {
      leds[i] = leds[i + 1];
    }
    
    // Inject the falling tracking brightness into the active dynamic pipeline
    leds[NUM_LEDS - 1] = CHSV(dynamicHue += HUE_CHANGE, SATURATION, (int)brightnessTracker);
  }
}

void LinearReactive(int currentMin, int currentMax) {
  val = map((int)brightnessTracker, 0, BRIGHTNESS, 0, NUM_LEDS);

  for(int i = 0; i < NUM_LEDS; i++) {
    if (i <= val)
      leds[i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
    else
      leds[i].nscale8(200); // Smooth trailing fade decay
  }
}

void BrightnessReactive(int currentMin, int currentMax) {
  for(int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, (int)brightnessTracker);
  }
}

void CentreProgressive(int currentMin, int currentMax) {
  val = map((int)brightnessTracker, 0, BRIGHTNESS, 0, NUM_LEDS / 2);

  for(int i = 0; i < NUM_LEDS / 2; i++) {
    if (i <= val) {
      leds[(NUM_LEDS / 2) + i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
      leds[(NUM_LEDS / 2) - 1 - i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS); 
    } else {
      leds[(NUM_LEDS / 2) + i].nscale8(200);
      leds[(NUM_LEDS / 2) - 1 - i].nscale8(200);
    }
  }
}

void EdgeProgressive(int currentMin, int currentMax) {
  val = map((int)brightnessTracker, 0, BRIGHTNESS, 0, NUM_LEDS / 2);

  for(int i = 0; i < NUM_LEDS / 2; i++) {
    if (i <= val) {
      leds[i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS);
      leds[NUM_LEDS - 1 - i] = CHSV(HUE_INIT + (HUE_CHANGE * i), SATURATION, BRIGHTNESS); 
    } else {
      leds[i].nscale8(200);
      leds[NUM_LEDS - 1 - i].nscale8(200);
    }
  }
}