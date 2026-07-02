# Music-Sync LEDs

Sound-reactive WS2812B LED strip driven by an Arduino and a SparkFun Sound
Detector. Silence keeps the strip dark; the louder the music, the brighter
the LEDs — with beat-accurate response (~20 ms sound-to-light latency) and
five visualization styles.

## Hardware

| Part | Notes |
|------|-------|
| Arduino Uno (or compatible AVR board) | Sketches use ~21% flash, ~67% RAM at 300 LEDs |
| SparkFun Sound Detector (SEN-12642) | Only the **ENVELOPE** output is used |
| WS2812B LED strip | 300 LEDs by default (`NUM_LEDS`) |
| 5 V power supply | See [Power](#power) below |

### Wiring

| Sound Detector | Arduino |
|----------------|---------|
| ENVELOPE | A0 |
| VCC | 5V |
| GND | GND |

| LED strip | Connection |
|-----------|------------|
| DIN | Pin 7 (through ~330 Ω resistor recommended) |
| 5V / GND | External 5 V supply (share GND with the Arduino) |

Requires the [FastLED](https://fastled.io/) library (3.10+ supported).

## Which sketch do I use?

| Sketch | What it is |
|--------|-----------|
| **`music-sync-leds-auto/`** | **Recommended.** Self-calibrating — no thresholds to tune, ever. Learns the room's noise floor and the music's loudness range continuously, on-device. |
| `music-sync-leds/` | Manually calibrated version. Fixed `NOISE_FLOOR` / `MIN_VAL` / `MAX_VAL` thresholds plus runtime auto-gain. Use it if you want full manual control; calibrate with the tools below. |
| `calibrate-sound/` | Interactive measurement tool for the manual sketch. Guides you through measuring ambient noise and music levels over serial, then suggests threshold values. |

Both LED sketches share the same five styles, selected by `STYLE`:

| `STYLE` | Effect |
|---------|--------|
| 0 | `LinearFlowing` — colors scroll along the strip, brightness = loudness |
| 1 | `LinearReactive` — VU-meter bar from one end |
| 2 | `BrightnessReactive` *(default)* — whole strip pulses, hue drifts over time |
| 3 | `CentreProgressive` — bar grows outward from the center |
| 4 | `EdgeProgressive` — bars grow inward from both ends |

## The self-calibrating sketch (`music-sync-leds-auto`)

Upload it and you're done. It adapts by itself to the room, the volume, and
the Sound Detector's gain-pot setting using two tiny on-device online
learners (integer fixed-point math, a few bytes of state):

1. **Ambient noise model** — learns the mean and spread of the signal, but
   only during sustained quiet runs (>1 s below an absolute quiet band).
   Music's between-beat gaps are shorter than that, so music can never
   drag the noise gate up. A fan turning on is absorbed within seconds.
   The learned floor is hard-capped at the quiet band, so real music can
   always light the strip no matter what the learner has seen.
2. **Loudness scale** — a stochastic quantile estimator (pinball-loss
   updates, 9:1 up/down ratio) tracks the music's rolling 90th percentile,
   so the loudest ~10% of any song saturates to full brightness at any
   volume.

The integer math was validated in simulation against silence, loud music,
quiet music, volume changes, steady fan noise, fan+music, quiet outros,
pauses between songs, and resets mid-song.

Good to know:

- Booting is seeded from ~0.5 s of ambient sound. Booting mid-song is fine
  (the seed is clamped and self-corrects), but a quiet boot converges fastest.
- A quiet outro that hums along for a few seconds will be re-learned as
  "ambient" and go dark — by design. The next louder sound reacts instantly.
- Set `DEBUG 1` and open Serial Monitor at 115200 to watch the learner:
  `Floor` should sit around 6–15 in silence and never block music.

## The manually calibrated sketch (`music-sync-leds`)

Fixed thresholds, plus runtime auto-gain for the top of the scale. Two ways
to calibrate:

### Automatic (recommended): `tools/calibrate.py`

Pure Python 3, no dependencies for log analysis (`pip3 install pyserial`
for live capture). Guided one-command calibration:

```bash
python3 tools/calibrate.py --port auto
```

It walks you through 15 s of silence and 45 s of music, separates the two
with robust statistics (and 1-D k-means when given a single mixed log),
computes `NOISE_FLOOR` / `MIN_VAL` / `MAX_VAL`, **validates them against
your own data** (false-trigger rate, music coverage, brightness spread),
and writes them into `music-sync-leds.ino` automatically. If the capture
looks broken — contaminated quiet phase, clipping, music indistinguishable
from silence — it refuses to touch the sketch and tells you what to fix.

Other modes:

```bash
python3 tools/calibrate.py capture.log                    # one mixed recording
python3 tools/calibrate.py --quiet q.log --music m.log    # separate recordings
python3 tools/calibrate.py capture.log --dry-run          # analyze only
```

### Manual: `calibrate-sound/`

Upload the sketch, open Serial Monitor at 115200, and type `1` (measure
ambient, room quiet), `2` (measure music), `3` (print suggested `#define`
values). The live stream is Serial-Plotter-friendly (`peak`/`baseline`/
`signal`) for eyeballing the response.

## Power

300 WS2812B LEDs can draw up to ~18 A at full white — far beyond USB or a
small adapter. Power the strip from a proper 5 V supply, share ground with
the Arduino, and consider adding this to `setup()` so FastLED throttles
brightness instead of browning out:

```cpp
FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000); // match your supply
```

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Strip glows/rotates during silence | Auto sketch: give it 1–2 s of quiet to learn. Manual sketch: raise `NOISE_FLOOR` or re-run calibration. |
| Music barely registers | Turn the Sound Detector's gain pot clockwise, or move it closer to the speaker. `tools/calibrate.py` warns when gain is too low or clipping. |
| Everything is full brightness | Manual sketch with stale calibration — re-run `tools/calibrate.py` (the auto sketch is immune; its scale adapts). |
| Feels laggy | Ensure `DEBUG 0`. Debug serial output costs frame time. |
| `FASTLED_USING_NAMESPACE does not name a type` | Old sketch line removed in FastLED 3.10+ — this repo's sketches don't use it; update your copy. |

## Repository layout

```
music-sync-leds-auto/   Self-calibrating sketch (recommended)
music-sync-leds/        Manually calibrated sketch with auto-gain
calibrate-sound/        Interactive serial calibration sketch
tools/calibrate.py      Log analyzer / guided auto-calibration for the manual sketch
```
