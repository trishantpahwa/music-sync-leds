/*
  calibrate-sound.ino
  Calibration tool for the SparkFun Sound Detector (SEN-12642) envelope output.
  Companion to music-sync-leds.ino — run this first to find good values for
  NOISE_FLOOR, MIN_VAL and MAX_VAL, then copy them into the main sketch.

  Usage — open Serial Monitor at 115200 baud (line ending: any) and type:
    1  ->  measure AMBIENT noise for 10 s   (keep the room QUIET)
    2  ->  measure MUSIC for 15 s           (play music at your usual volume,
                                             include a loud section / drop!)
    3  ->  print suggested #define values based on steps 1 and 2
    r  ->  reset all measurements
    p  ->  pause / resume the live stream

  The live stream is formatted for the Arduino Serial Plotter
  (Tools > Serial Plotter) so you can watch peak / baseline / signal
  respond to your music in real time.
*/

#define ENVELOPE_PIN   A0
#define WINDOW_MS      20   // one sample = the peak envelope value in this window
#define AMBIENT_SECS   10
#define MUSIC_SECS     15

struct Stats {
  bool valid;
  int minPeak;
  int maxPeak;
  long sum;
  int count;
  int p90;      // 90th percentile of window peaks
};

Stats ambient = {false, 0, 0, 0, 0, 0};
Stats music   = {false, 0, 0, 0, 0, 0};
uint16_t hist[128];        // 8-wide buckets covering ADC range 0..1023
bool streaming = true;

// Mirrors the smoothing/baseline logic in music-sync-leds.ino so the
// plotted "signal" matches what the main sketch will actually see.
int smoothed = 0;
int baseline = 0;

int avgOf(const Stats &s) {
  return s.count ? (int)(s.sum / s.count) : 0;
}

// Sample the envelope as fast as possible for WINDOW_MS, return the peak.
int readWindowPeak() {
  int peak = 0;
  unsigned long start = millis();
  while (millis() - start < WINDOW_MS) {
    int v = analogRead(ENVELOPE_PIN);
    if (v > peak) peak = v;
  }
  return peak;
}

void printStats(const __FlashStringHelper *label, const Stats &s) {
  Serial.println();
  Serial.print(F("--- ")); Serial.print(label); Serial.println(F(" results ---"));
  Serial.print(F("  min peak : ")); Serial.println(s.minPeak);
  Serial.print(F("  avg peak : ")); Serial.println(avgOf(s));
  Serial.print(F("  90th pct : ")); Serial.println(s.p90);
  Serial.print(F("  max peak : ")); Serial.println(s.maxPeak);
  Serial.println();
}

void measure(Stats &s, int seconds, const __FlashStringHelper *label) {
  Serial.println();
  Serial.print(F(">>> Measuring ")); Serial.print(label);
  Serial.print(F(" for ")); Serial.print(seconds);
  Serial.println(F(" s... (one dot per second)"));

  memset(hist, 0, sizeof(hist));
  s.minPeak = 1023;
  s.maxPeak = 0;
  s.sum = 0;
  s.count = 0;

  unsigned long start = millis();
  unsigned long nextDot = start + 1000UL;
  while (millis() - start < (unsigned long)seconds * 1000UL) {
    int peak = readWindowPeak();
    if (peak < s.minPeak) s.minPeak = peak;
    if (peak > s.maxPeak) s.maxPeak = peak;
    s.sum += peak;
    s.count++;
    hist[peak >> 3]++;
    if (millis() >= nextDot) {
      Serial.print('.');
      nextDot += 1000UL;
    }
  }

  // 90th percentile: walk the histogram until we pass 90% of samples
  long threshold = (long)s.count * 9 / 10;
  long cumulative = 0;
  s.p90 = s.maxPeak;
  for (int b = 0; b < 128; b++) {
    cumulative += hist[b];
    if (cumulative >= threshold) {
      s.p90 = b * 8 + 4;  // bucket centre
      break;
    }
  }

  s.valid = true;
  printStats(label, s);
}

void printSuggestions() {
  if (!ambient.valid || !music.valid) {
    Serial.println(F("Run measurement 1 (ambient) and 2 (music) first."));
    return;
  }

  int base = avgOf(ambient);
  int ambientSpike = ambient.maxPeak - base;      // worst background spike
  int musicTypical = music.p90 - base;            // "loud part" level
  int musicMax     = music.maxPeak - base;        // absolute loudest hit

  int noiseFloor = ambientSpike + 2;              // +2 margin over worst spike
  int minVal     = noiseFloor + 1;
  int maxVal     = musicTypical;                  // p90 so drops saturate to full

  Serial.println();
  Serial.println(F("=== Suggested values for music-sync-leds.ino ==="));
  Serial.print(F("  ambient baseline (raw) : ")); Serial.println(base);
  Serial.print(F("  music typical signal   : ")); Serial.println(musicTypical);
  Serial.print(F("  music max signal       : ")); Serial.println(musicMax);
  Serial.println();
  Serial.print(F("#define MIN_VAL      ")); Serial.println(minVal);
  Serial.print(F("#define MAX_VAL      ")); Serial.println(maxVal);
  Serial.print(F("#define NOISE_FLOOR  ")); Serial.println(noiseFloor);
  Serial.println();

  // Sanity checks on the detector's hardware gain
  if (musicMax < 15) {
    Serial.println(F("WARNING: music barely rises above ambient (< 15 counts)."));
    Serial.println(F("  -> Turn the gain pot on the Sound Detector clockwise,"));
    Serial.println(F("     or move it closer to the speaker, then re-run 1 and 2."));
  } else if (music.maxPeak > 1000) {
    Serial.println(F("WARNING: envelope is clipping at the top of the ADC range."));
    Serial.println(F("  -> Turn the gain pot down a little, then re-run 1 and 2."));
  }
  if (maxVal <= minVal) {
    Serial.println(F("WARNING: MAX_VAL <= MIN_VAL — not enough separation between"));
    Serial.println(F("  music and background noise. Fix gain/placement and re-run."));
  }
}

void printHelp() {
  Serial.println(F("Commands: 1=measure ambient  2=measure music  3=suggestions"));
  Serial.println(F("          r=reset  p=pause/resume live stream"));
}

void setup() {
  Serial.begin(115200);
  pinMode(ENVELOPE_PIN, INPUT);
  smoothed = analogRead(ENVELOPE_PIN);
  baseline = smoothed;
  Serial.println(F("Sound Detector Calibration Tool"));
  printHelp();
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case '1': measure(ambient, AMBIENT_SECS, F("AMBIENT")); break;
      case '2': measure(music, MUSIC_SECS, F("MUSIC")); break;
      case '3': printSuggestions(); break;
      case 'r':
        ambient.valid = false;
        music.valid = false;
        Serial.println(F("Measurements reset."));
        break;
      case 'p':
        streaming = !streaming;
        if (!streaming) Serial.println(F("Stream paused."));
        break;
      case '\n':
      case '\r':
        break;
      default:
        printHelp();
    }
  }

  int peak = readWindowPeak();

  // Same asymmetric attack/decay + dynamic baseline as the main sketch
  if (peak > smoothed)
    smoothed = peak;
  else
    smoothed = (smoothed * 3 + peak) / 4;

  if (smoothed < baseline)
    baseline = (baseline * 3 + smoothed) / 4;
  else
    baseline = (baseline * 31 + smoothed) / 32;

  int signal = smoothed - baseline;

  if (streaming) {
    // Arduino Serial Plotter format: label:value pairs
    Serial.print(F("peak:"));     Serial.print(peak);
    Serial.print(F("\tbaseline:")); Serial.print(baseline);
    Serial.print(F("\tsignal:"));   Serial.println(signal);
  }
}
