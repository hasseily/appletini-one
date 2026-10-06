# SSI capture comparison

## First physical capture, 2026-10-06

The PAL-01 return is usable. The tester's README identifies a **Phasor Rev E**
with two **SSI263AP** chips and confirms **DONE**. Isolation and player-address
checks give this mapping:

| Trace target | Address / socket | Chip marking | Recorded channel |
| --- | --- | --- | --- |
| 4 | `$C420`, top | P8433 | Right / 2 |
| 5 | `$C440`, bottom | P8431 | Left / 1 |

The recording is 125.684 seconds, stereo 48 kHz, 16-bit PCM. All five markers
are present, with no digital-rail samples or capture-quality flags. Isolation
is about 36/33 dB, including the inactive channel's noise. Repeated reference
levels change by less than 0.02 dB. These checks do not rule out distortion in
the analog chain.

The returned disk contains the exact checked player and calibration streams.
`CAL.LOG` reports completion at tick 12000, 269 change records, no playback
error and no overflow. It samples raw D7 once per roughly 10 ms tick; it does
not measure exact response edges or the SSI clock.

The WAV SHA256 is
`d3d0c94cc23fd0887ca15b4293b1c6b49e2da9cbedea880a4df04cdaf89c56b7`.
The disk SHA256 is
`90e58f02c082d35504b4b043433207221ac3a7525a9edc48c08cddc1a4bb2c3a`.
The checked package SHA256 is
`81e7aaaa9f0d2745cf40417089e9410da68bae53c4178aa26a78061138781438`.

### First findings

No sound parameters or RTL changed during this analysis. The comparison uses
the existing continuous baseline and native candidate, including all preceding
writes. The page calls the native candidate **Prototype**; that label does not
claim exact agreement with the prototype die.

- **Steady pitch is close.** All eight long probes, about 65–280 Hz, agree
  with native within 0.02 Hz in full-window estimates. Shorter-window variation
  reaches about 0.04 Hz, so the extra decimal places are not a clock calibration.
  The old baseline runs about 0.57–5.47 Hz higher. Native also follows the
  measured loudness changes with pitch well for the tested AH/FF setting.
- **Voiced TH is much too rich in high-frequency energy in native.** In guarded
  THV (`$35`) tails, the real chips put about 6–7% of their 20 Hz–20 kHz power
  above 4 kHz, versus about 79–80% in native. Shifting the global recording map
  by ±25 ms gives hardware ranges of 6.9–7.4% and 5.6–6.7%. Tail levels are
  stable, and the large gap appears on both chips. This supports the reported
  Z-like TH, but does not by itself choose between source balance, filtering
  and the omitted Phasor/recorder response.
- **Frication is too strong relative to vowels in the full comparison.** In
  matched AH–HF–AH probes, physical HF is about 23 dB below AH; native is about
  8 dB below. Physical S is about 19 dB below AH; native is about 2 dB below.
  The window-shift check changes these contrasts by less than 0.5 dB. These
  are within-stream level differences, not fitted gains. Analog frequency
  response still matters, so they are not a prescription to reduce source
  gain by a fixed number.
- **HFC suppression and external CTL mute/resume look right in native.** HFC
  falls near the physical noise floor, unlike the old baseline's sustained
  frication. CTL high attenuates speech by about 35 dB; release without a new
  phone restores the held vowel. This supports the external behavior, not the
  candidate's exact internal reset or filter-charge policy.
- **The measured attack does not call for a slower native ramp.** Conditioned
  AH rises take roughly 35–40 ms on hardware and 60–70 ms in native using the
  same 20 ms RMS measure. Smoothing and the analog path limit precision. The
  cold first-H case remains untested; perceived aggression need not mean an
  amplitude ramp that is too fast.
- **Amplitude affects physical tone as well as level.** AMP 4 to 15 lowers
  the physical centroid from about 892 Hz to 726/754 Hz, while native stays
  near 982 Hz. Both chips show the trend; ±25 ms shifts change those hardware
  centroids by at most 1.7 Hz. Constant recorder gain cannot explain it, but
  SSI nonlinearity and the analog output chain remain possible causes.
- **The unconditional R0 response restart conflicts with real D7 observations.**
  PA at tick 840 clears D7; the next R0 at 850 arrives while it remains low.
  Physical D7 rises at 853/854. Current code restarts a 131072-clock interval
  at tick 850, predicting a first high sample near 863. The pattern repeats
  at 900, 950 and 1000. The roughly 90–100 ms error exceeds sampling uncertainty.
  It supports retaining an in-progress response or different pending-request
  handling; it does not yet identify a unique counter/latch implementation.

### Prototype conflicts and limits

The response-restart policy was already a documented departure from the SC-02
prototype's retained timing. **The log now argues against that departure for
these mid-pending writes.** Keep the distinction between an audio phone update,
duration state and response-latch handling when fixing it; changing all three
on this evidence would go beyond what the log establishes.

HF/HFC behavior remains compatible with the known selector difference. CTL
mute/resume does not establish an all-latch reset or glottal-synchronizer clear.
Cold FRIC fallback remains unproven. Fast/slow ART behavior is clear, but the
pair holds RATE at 8 and cannot establish ART/RATE independence. The analog
measurements do not yet justify new departures from the prototype circuit.

Use the long phone/probe windows first. The five-marker affine map retains
±25 ms edge uncertainty; 40 ms FF windows are only broad behavior checks.
Guarded phone-tail measurements use 200–350 ms after the write, so all ±25 ms
selections stay inside the 400 ms phone. Their spectra use a common
20 Hz–20 kHz range. No per-phone alignment, gain fitting, EQ or audio changes
were applied.

### Local evidence and next work

The full listening/plot page is
`build/ssi263_host/hardware_comparison_20261006/index.html` (416 windows).
Search `phone-35` for voiced TH, then select target 4 or 5. Its original
candidate window differs from the guarded analysis window above. The page
keeps original playback levels; model and capture gains are not calibrated.

Reproducible audits are under
`build/ssi263_host/hardware_capture_20261006/`: `quality.json`,
`audit_steady_measurements.py`, `transition_analysis.py`,
`transition_summarize.py`, `phoneme_audit.py`, and `extract_cal_log.py`.
Their JSON/text outputs retain the measurements and uncertainty checks.
`cal_log_timing_review.txt` gives exact write histories;
`cal_log_model_timing_review.txt` gives source hashes and the restart prediction.

Next, turn the observed D7 histories into response regressions, then inspect
the source/filter balance against both recorded chips before any sound tuning.
Use the amplitude spectrum trend to test circuit/output-path explanations.
The completed F1.2.5-d1 firmware remains the pre-analysis listening checkpoint;
it does not include fixes derived from this recording.

## Comparison tool

`scripts/compare_ssi263_capture.py` makes a local HTML page for the returned
PAL-01 stereo WAV. It checks the tester ZIP, renders fresh continuous baseline
and prototype audio from its exact SSI write trace, and compares the capture
with both. It needs Python, NumPy and the same C++17 compiler as the host
renderer. It does not run Vivado or change firmware.

For the real recording:

```powershell
python scripts/compare_ssi263_capture.py --wav C:/captures/tester.wav --package build/ssi263_calibration/SSI263-CAL-PAL-01.zip --output build/ssi263_host/hardware_comparison
```

Open `build/ssi263_host/hardware_comparison/index.html`. The page and its local
assets work without a server or Internet connection. Keep the whole output
folder when moving it. Plot data loads only for the selected window; audio
loads on the first Play click. If a browser cannot
decode the recording's WAV format, use the original WAV link; the numerical
analysis still uses the supported PCM/float samples read by Python.

For a preview before the tester returns:

```powershell
python scripts/compare_ssi263_capture.py --demo --package build/ssi263_calibration/SSI263-CAL-PAL-01.zip --output build/ssi263_host/comparison_preview
```

The demo uses the freshly rendered baseline, adds ideal AY markers, swaps the
channels, and applies a known 0.73-second offset and 600 ppm synthetic recorder
drift. Its page says **SYNTHETIC TOOLING DEMO**. It tests the comparison tools;
it provides no evidence about physical SSI accuracy.

## What the page shows

- A segment/window selector for all 416 candidate windows, a search box and
  separate target-4/target-5 selection.
- Recording and model context clips that include the entire scheduled segment
  plus 80 ms on each side. Candidate-window clips use the same global map.
- RMS envelopes with the candidate window shaded, so attacks and transitions
  remain visible instead of showing only a later steady window.
- Hann FFT amplitude spectra and raw RMS, AC RMS, peak, DC, clipping and
  spectral-centroid measurements from the candidate interval.
- Marker residuals, timing drift, channel mapping, missing coverage and clipping
  flags. The input and model provenance remain linked from the page.

The same recording-wide affine fit from the five AY markers selects every
interval. The isolation segments select a capture channel only when both
sockets have a clear, distinct mapping. Missing markers or unresolved mapping
leave the matched recording controls disabled. Partial candidate windows have
no substitute measurement; their missing data stays explicit.

The marker edges have about **25 ms uncertainty**. This can cross adjacent
40 ms FF settings. Those windows remain marked as gross behavior only. All
candidate settling remains unverified even when the capture passes its quality
checks.

The onset plot also shades a ±25 ms band around nominal segment start. Its
2 ms RMS bins do not imply 2 ms hardware/model alignment accuracy.

## Levels and timing

The tool does not align individual phones, normalize clips, fit gain, apply EQ,
or alter the recording's sample-rate frequency axis. Each playback clip keeps
its own recorded duration and pitch. Only its time labels use the global map.
Browser/device playback may resample to the output device as ordinary playback
does; plots and metrics use the untouched original input samples.

Playback gain starts at unity. Float samples above digital full scale remain
visible in raw metrics. The page keeps the original WAV bytes, so it does not
silently convert an overloaded float recording into clipped PCM. A source
whose output already clips remains flagged; no normalization hides it.

The spectrum removes DC and applies a Hann window. It reports one-sided peak
amplitude in dBFS, not power density. For a compact plot, each group of adjacent
FFT bins contributes its largest bin. Its listed FFT resolution is the actual
sample rate divided by the candidate's sample count. Raw level metrics retain
DC and use every sample.

Model gain is fixed for the whole run. The prototype defaults remain output
gain 8, voice trim 2048 in Q16 and articulation reference RATE 8. The CLI exposes
`--prototype-gain`, `--voice-trim` and `--articulation-reference-rate`; their
values appear in the saved provenance. The baseline uses its existing fixed
gain. No setting is fitted to the returned WAV.

## Saved evidence

The output includes `recording.wav`, `capture_analysis.json`, `comparison.json`,
both full model WAVs and model reports, the checked write stream and extracted
tables. Reports record the recording/package/trace hashes, executable and model
source hashes, table hashes and comparison/analyzer hashes. A render fails if
the host sources change while it runs. Both models replay the full prefix and
retain state across every phone before the page selects any window.

The models omit AY audio, the Phasor analog mixer and the recorder response.
The global drift estimate combines host and recorder timing; it does not prove
the chip clock. A small plotted difference is not a calibration result.

Run the focused tests with:

```powershell
python scripts/test_ssi263_capture_comparison.py
```

These use independent tones and envelopes to check absolute levels, spectrum
frequency, time mapping, channel swapping, missing markers, duplicate channels,
partial coverage, float overrange and safe inline metadata.
