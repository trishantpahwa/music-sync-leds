#!/usr/bin/env python3
"""
Sound Detector calibration analyzer for music-sync-leds.

Reads envelope logs of any length and computes NOISE_FLOOR / MIN_VAL / MAX_VAL
for music-sync-leds.ino using robust statistics plus 1-D k-means clustering to
separate silence from music. It then validates the suggested values against
the same data (false-trigger rate, music coverage, brightness spread) so you
know how they will behave before uploading.

Accepted log line formats (timestamp prefixes like "23:05:06.126 -> " are ok):
    calibrate-sound.ino stream : "peak:123  baseline:80  signal:43"
    music-sync-leds.ino DEBUG  : "Raw: 123  Baseline: 80  Signal: 43 ..."

Usage:
  # One mixed recording containing BOTH silence and music (auto-clustered):
  python3 tools/calibrate.py capture.log

  # Separate labeled recordings (most accurate):
  python3 tools/calibrate.py --quiet quiet.log --music music.log

  # Live guided capture from the Arduino (needs: pip3 install pyserial):
  python3 tools/calibrate.py --port auto
  python3 tools/calibrate.py --port /dev/cu.usbmodem1101

Capturing a log without pyserial, straight from the terminal:
  stty -f /dev/cu.usbmodem1101 115200 && cat /dev/cu.usbmodem1101 > capture.log
(Ctrl-C to stop. Or use Arduino IDE Serial Monitor: select all, copy to a file.)
"""

import argparse
import datetime
import glob
import os
import re
import sys
import time

# Matches both sketch formats; we only need the raw envelope peak per frame.
# Case-sensitive on purpose: the main sketch's DEBUG line also prints
# "Peak: N" (the auto-gain level, not a reading) which must NOT match.
VALUE_RE = re.compile(r"(?:^|[\s>])(?:peak:|Raw:\s*)(\d+)")

ADC_MAX = 1023
CLIP_LEVEL = 1015          # treat readings above this as ADC clipping
MIN_SAMPLES = 50           # refuse to calibrate on less data than this
GAMMA = True               # mirror the sketch's val^2/255 gamma in previews


# ---------------------------------------------------------------- statistics

def percentile(sorted_vals, p):
    """Nearest-rank percentile on a pre-sorted list."""
    if not sorted_vals:
        return 0
    k = max(0, min(len(sorted_vals) - 1, round(p / 100 * (len(sorted_vals) - 1))))
    return sorted_vals[k]


def kmeans_two(values, iterations=100):
    """1-D k-means with k=2. Returns (low_cluster, high_cluster)."""
    s = sorted(values)
    lo_c, hi_c = float(percentile(s, 10)), float(percentile(s, 90))
    if hi_c - lo_c < 1:  # degenerate: all values basically identical
        return list(values), []
    for _ in range(iterations):
        boundary = (lo_c + hi_c) / 2
        lo = [v for v in values if v <= boundary]
        hi = [v for v in values if v > boundary]
        if not lo or not hi:
            return list(values), []
        new_lo, new_hi = sum(lo) / len(lo), sum(hi) / len(hi)
        if abs(new_lo - lo_c) < 0.01 and abs(new_hi - hi_c) < 0.01:
            break
        lo_c, hi_c = new_lo, new_hi
    return lo, hi


# ------------------------------------------------------------------- parsing

def parse_values(text):
    return [int(m.group(1)) for m in VALUE_RE.finditer(text)]


def read_log(path):
    with open(path, "r", errors="replace") as f:
        vals = parse_values(f.read())
    if not vals:
        sys.exit(f"error: no envelope values found in {path} — expected lines "
                 f"containing 'peak:N' or 'Raw: N'")
    return vals


# ------------------------------------------------------------------ analysis

def ascii_histogram(quiet, music, width=52, buckets=24):
    all_vals = quiet + music
    lo, hi = min(all_vals), max(all_vals)
    span = max(1, hi - lo)
    step = span / buckets
    lines = []
    for b in range(buckets):
        b_lo = lo + b * step
        b_hi = b_lo + step
        q = sum(1 for v in quiet if b_lo <= v < b_hi or (b == buckets - 1 and v == hi))
        m = sum(1 for v in music if b_lo <= v < b_hi or (b == buckets - 1 and v == hi))
        lines.append((int(b_lo), q, m))
    peak = max(q + m for _, q, m in lines) or 1
    out = ["  value | . = silence frames, # = music frames"]
    for b_lo, q, m in lines:
        qw = round(q / peak * width)
        mw = round(m / peak * width)
        if q + m == 0:
            continue
        out.append(f"  {b_lo:5d} | {'.' * qw}{'#' * mw}")
    return "\n".join(out)


def brightness_for(sig, min_val, max_val):
    """Mirror BrightnessReactive(): map + gamma."""
    if max_val <= min_val:
        return 0
    if sig < min_val:
        sig = min_val
    if sig > max_val:
        sig = max_val
    val = (sig - min_val) * 255 // (max_val - min_val)
    if GAMMA:
        val = val * val // 255
    return val


def default_ino_path():
    return os.path.normpath(os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "music-sync-leds", "music-sync-leds.ino"))


def apply_to_ino(path, values):
    """Rewrite the calibration #defines in the sketch. Returns True on success."""
    try:
        with open(path, "r") as f:
            src = f.read()
    except OSError as e:
        print(f"  ! could not open sketch to update it: {e}")
        return False
    stamp = datetime.date.today().isoformat()
    changed = []
    for name, value in values.items():
        pattern = re.compile(rf"^(#define\s+{name}\s+)(\d+)([^\n]*)$", re.MULTILINE)
        m = pattern.search(src)
        if not m:
            print(f"  ! #define {name} not found in {path} — not updated")
            return False
        old = int(m.group(2))
        src = pattern.sub(
            rf"\g<1>{value} // Auto-calibrated {stamp} by tools/calibrate.py",
            src, count=1)
        changed.append(f"{name} {old} -> {value}")
    with open(path, "w") as f:
        f.write(src)
    print(f"\nUpdated {path}:")
    for c in changed:
        print(f"  {c}")
    print("Re-upload the sketch to apply.")
    return True


def analyze(quiet, music, clustered):
    sq, sm = sorted(quiet), sorted(music)
    warnings = []

    # Trim contamination out of the quiet phase: frames far above the ambient
    # core (median + 8 sigma via MAD) are loud sounds that leaked into the
    # capture, not ambient noise, and would wreck NOISE_FLOOR.
    base = percentile(sq, 50)                       # median ambient level
    mad = percentile(sorted(abs(v - base) for v in sq), 50)
    cutoff = base + 8 * max(0.5, 1.4826 * mad)
    core = [v for v in sq if v <= cutoff]
    dropped = len(sq) - len(core)
    if dropped:
        frac = dropped / len(sq)
        print(f"\n  note: ignored {dropped} loud frames ({frac:.1%}) in the "
              f"quiet phase (above {int(cutoff)})")
        if frac > 0.02:
            warnings.append(f"the quiet phase contained {frac:.1%} loud frames "
                            "— was something making noise? Re-capture with the "
                            "room truly silent for trustworthy values.")
        sq = sorted(core)

    ambient_spike = percentile(sq, 99.9) - base     # worst spike, glitch-robust
    noise_floor = max(3, ambient_spike + 2)
    min_val = noise_floor + 1
    music_sig = sorted(v - base for v in sm)
    max_val = percentile(music_sig, 90)

    print()
    print(f"  Samples: {len(quiet)} silence, {len(music)} music"
          + ("  (separated automatically by clustering)" if clustered else ""))
    print(f"  Ambient: median {base}, p99.9 {base + ambient_spike}, max {sq[-1]}")
    print(f"  Music  : median {percentile(sm, 50)}, p90 {percentile(sm, 90)}, "
          f"max {sm[-1]}")
    print()
    print(ascii_histogram(quiet, music))

    # ---- suggested values
    print()
    print("=== Paste into music-sync-leds.ino ===")
    print(f"#define MIN_VAL      {min_val}")
    print(f"#define MAX_VAL      {max_val}")
    print(f"#define NOISE_FLOOR  {noise_floor}")

    # ---- validation against the actual data
    false_on = sum(1 for v in quiet if v - base > noise_floor) / len(quiet)
    lit = sum(1 for v in music if v - base > noise_floor) / len(music)
    saturated = sum(1 for v in music_sig if v >= max_val) / len(music_sig)
    print()
    print("=== Validation (how these values behave on this data) ===")
    print(f"  Silence frames that would light up : {false_on:6.2%}  (want ~0%)")
    print(f"  Music frames that light up         : {lit:6.2%}  (quiet passages stay dark)")
    print(f"  Music frames at full brightness    : {saturated:6.2%}  (~10% is ideal)")
    print("  Brightness preview across the music (0-255, after gamma):")
    for p in (25, 50, 75, 90, 99):
        sig = percentile(music_sig, p)
        print(f"    p{p:<2} of music -> brightness {brightness_for(sig, min_val, max_val):3d}")

    # ---- consistency checks: catch a broken capture before it ships
    if max_val <= min_val:
        warnings.append("MAX_VAL <= MIN_VAL — the music does not rise above "
                        "the noise floor. Capture is unusable: check that music "
                        "was actually playing near the detector and re-capture.")
    if lit < 0.90:
        warnings.append(f"only {lit:.0%} of music frames would light up (want "
                        ">90%) — these values would leave the strip mostly dark.")
    if abs(percentile(sm, 50) - base) < 10:
        warnings.append("quiet and music phases look almost identical — check "
                        "wiring/gain and that music was playing, then re-capture.")

    # ---- hardware diagnostics
    clip = sum(1 for v in sm if v >= CLIP_LEVEL) / len(sm)
    if clip > 0.005:
        warnings.append(f"{clip:.1%} of music frames clip the ADC — turn the "
                        f"gain pot DOWN a little and re-capture.")
    if max_val < 15:
        warnings.append("music barely rises above ambient (< 15 counts) — turn "
                        "the gain pot UP or move the detector closer, re-capture.")
    if false_on > 0.02:
        warnings.append("ambient spikes overlap the music range — raise "
                        "NOISE_FLOOR manually or reduce background noise.")
    if len(quiet) < MIN_SAMPLES or len(music) < MIN_SAMPLES:
        warnings.append("very few samples in one class — capture a longer log "
                        "for trustworthy numbers.")
    if warnings:
        print()
        print("=== Warnings ===")
        for w in warnings:
            print(f"  ! {w}")
    print()
    return {"MIN_VAL": min_val, "MAX_VAL": max_val,
            "NOISE_FLOOR": noise_floor}, warnings


def split_mixed(values):
    """Cluster a mixed log into (quiet, music); die with advice if impossible."""
    quiet, music = kmeans_two(values)
    if not music or (sum(music) / len(music)) - (sum(quiet) / len(quiet)) < 12:
        sys.exit("error: could not find two distinct loudness levels in this "
                 "log.\nIt probably contains only silence or only music. "
                 "Capture a log that includes both,\nor pass separate files: "
                 "--quiet quiet.log --music music.log")
    # Refinement: k-means drops music's between-beat lows into the quiet
    # cluster, inflating the ambient ceiling. Trim the quiet cluster to its
    # statistical core (median + 6 sigma, sigma from MAD) and reassign the
    # rest to music.
    sq = sorted(quiet)
    base = percentile(sq, 50)
    mad = percentile(sorted(abs(v - base) for v in sq), 50)
    cutoff = base + 6 * max(0.5, 1.4826 * mad)
    refined_quiet = [v for v in quiet if v <= cutoff]
    music += [v for v in quiet if v > cutoff]
    if len(refined_quiet) >= MIN_SAMPLES:
        quiet = refined_quiet
    return quiet, music


# -------------------------------------------------------------- live capture

def find_port():
    candidates = (glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/cu.usbserial*")
                  + glob.glob("/dev/cu.wchusbserial*") + glob.glob("/dev/ttyUSB*")
                  + glob.glob("/dev/ttyACM*"))
    if not candidates:
        sys.exit("error: no Arduino serial port found — pass --port explicitly")
    return candidates[0]


def capture_phase(ser, seconds, label):
    input(f"\n>>> {label}\n    Press Enter to start the {seconds}s capture... ")
    ser.reset_input_buffer()
    vals, deadline, last_dot = [], time.time() + seconds, 0
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace")
        vals.extend(parse_values(line))
        remaining = int(deadline - time.time())
        if remaining != last_dot:
            print(f"\r    {remaining:3d}s left, {len(vals)} samples ",
                  end="", flush=True)
            last_dot = remaining
    print(f"\r    done: {len(vals)} samples          ")
    if len(vals) < MIN_SAMPLES:
        sys.exit("error: too few samples — is the right sketch running and "
                 "printing at 115200 baud? (calibrate-sound.ino, or "
                 "music-sync-leds.ino with DEBUG 1)")
    return vals


def live_capture(port, baud, quiet_secs, music_secs):
    try:
        import serial
    except ImportError:
        sys.exit("error: live capture needs pyserial — run: pip3 install pyserial\n"
                 "(or capture a log file instead; see --help)")
    if port == "auto":
        port = find_port()
    print(f"Opening {port} @ {baud}...")
    with serial.Serial(port, baud, timeout=1) as ser:
        time.sleep(2)  # UNO resets on port open; let the sketch boot
        quiet = capture_phase(ser, quiet_secs,
                              "PHASE 1 — keep the room QUIET (no music, no talking)")
        music = capture_phase(ser, music_secs,
                              "PHASE 2 — play MUSIC at your normal volume "
                              "(include a loud section / drop)")
    return quiet, music


# ---------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="mixed log file (silence + music)")
    ap.add_argument("--quiet", help="log file recorded in silence")
    ap.add_argument("--music", help="log file recorded with music playing")
    ap.add_argument("--port", help="serial port for live capture, or 'auto'")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--quiet-secs", type=int, default=15)
    ap.add_argument("--music-secs", type=int, default=45)
    ap.add_argument("--ino", default=default_ino_path(),
                    help="sketch to update (default: %(default)s)")
    ap.add_argument("--dry-run", action="store_true",
                    help="analyze only, do not modify the sketch")
    ap.add_argument("--force", action="store_true",
                    help="update the sketch even if there are warnings")
    args = ap.parse_args()

    clustered = False
    if args.port:
        quiet, music = live_capture(args.port, args.baud,
                                    args.quiet_secs, args.music_secs)
    elif args.quiet and args.music:
        quiet, music = read_log(args.quiet), read_log(args.music)
    elif args.log:
        values = read_log(args.log)
        if len(values) < MIN_SAMPLES * 2:
            sys.exit(f"error: only {len(values)} samples — capture a longer log")
        quiet, music = split_mixed(values)
        clustered = True
    else:
        ap.print_help()
        sys.exit(1)

    values, warnings = analyze(quiet, music, clustered)

    if args.dry_run:
        print("(dry run — sketch not modified)")
    elif warnings and not args.force:
        print("Sketch NOT updated because of the warnings above. Fix and "
              "re-capture, or re-run with --force to apply anyway.")
    else:
        apply_to_ino(args.ino, values)


if __name__ == "__main__":
    main()
