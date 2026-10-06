# SSI-263 physical-chip matching: FPGA handoff

Prepared 2026-10-06 for the local Appletini One instance with Vivado.

## Goal and current status

Make Appletini interpret SSI-263 register writes like a physical Phasor and
produce a measured, close match to its speech. The same register stream should
work on both targets, including PAL and NTSC. Start with the correct clock and
register laws, then calibrate the audio response from isolated chip recordings.

This document is an implementation brief. It does not report a completed FPGA
fix, a new bitstream, or a calibrated analog model. The working correction so
far is in the **song exporter**, which has a separate physical-SSI profile.

The user tested a PAL enhanced Apple //e at normal 1 MHz with a physical Phasor
and two SSI speech chips. After the export correction, the user reported that
the song sounded much better. The new recording supports that observation.

Work in the user's current firmware branch and follow `AGENTS.md`. The sources
below were inspected in local commit `24f13df`. On 2026-10-06, these five files
were also fetched and compared byte for byte against remote main `ab0846b` and
`codex/turbo-paging-dma` at `8fe96ac`: the formant backend, digital core, formant
package, XCK clock-enable helper, and filter-frequency documentation. All five
matched. This does not claim that the full repository trees match.

## Evidence from the two recordings

The user supplied `asong.mp3` before the export correction and `risingsun.mp3`
afterward. Both contain the full music mix. The second file is assumed to play
the physical PAL disk published at software commit `85bd75c`; audio alone does
not prove which build or register values were played.

Measurements used the same 64 sustained-vowel windows, aligned against the
score and Appletini RTL preview. Audio was resampled to 12 kHz; stereo powers
were averaged without summing channel phases. Pitch came from narrow spectral
searches near the expected notes. Short windows, vibrato, two chips, AY sound
and MP3 compression can bias individual measurements.

| Measurement | Original capture | Corrected capture |
| --- | ---: | ---: |
| Median vocal pitch relative to RTL preview | -30.99 cents | +1.62 cents |
| Median vocal pitch relative to authored score | Not used for this comparison | +1.19 cents |
| Median energy ratio, 500–4000 Hz / 70–500 Hz | -14.97 dB | +5.41 dB |
| Median spectral centroid in vocal windows | 270 Hz | 788 Hz |

For the corrected capture, the middle 80% of pitch estimates lie between
-1.19 and +5.57 cents relative to the score. Instrumental alignment remains
stable, with about 69 ms recording lead-in and no substantial accumulated
drift. AY bass tuning agrees within measurement resolution.

Upper speech energy has returned. These spectra compare mixed recordings;
they do **not** identify a pure SSI transfer function or prove exact formant
frequencies. The RTL isolated voice's median centroid is about 621 Hz, so the
new recording also does not establish that the two voices now match.

The song exercises only a narrow physical FF range, 229–231 on PAL including
initialization. It cannot calibrate all FF values, phoneme filters, noise,
amplitude response, transitions, or either board's analog output path.

## Exact register and clock contract

The [SSI-263A datasheet](https://downloads.reactivemicro.com/Electronics/Speech/SSI-263A%20Data%20Sheet%20v2.pdf)
gives these relationships. `XCK_effective` means the clock after any DIV2
selection, not necessarily the frequency at the raw XCK pin.

```text
filter_clock_hz = XCK_effective / (2 * (256 - FF))
fundamental_hz  = XCK_effective / (8 * (4096 - I))

FF: unsigned 8-bit filter-frequency register, 0..255
I:  unsigned 12-bit immediate-inflection word, 0..4095
```

For the physical export, the default effective clocks are 1,015,625 Hz PAL and
1,020,484 Hz NTSC. The original recording's pitch and inflection words imply
about 1,015,529 Hz at the median, supporting the PAL assumption. This inference
does not replace a direct clock measurement or a primary board schematic.
The existing FPGA clock helper documents Q3 feeding both SSI sockets with DIV2
enabled. Use the actual clock-enable path when implementing the register laws.

| FF | PAL filter clock | Relative to nominal 20 kHz |
| ---: | ---: | ---: |
| 0 | 1,983.64 Hz | 0.09918 |
| 128 | 3,967.29 Hz | 0.19836 |
| 230 | 19,531.25 Hz | 0.97656 |
| 231 | 20,312.50 Hz | 1.01563 |
| 232 | 21,158.85 Hz | 1.05794 |
| 255 | 507,812.50 Hz | 25.39063 |

The filter clock is an internal switched-capacitor clock. It is **not** the
voice fundamental, an individual formant frequency, or the DAC sample rate.
The datasheet gives about 20 kHz as typical. The
[Programming Guide](https://downloads.reactivemicro.com/Electronics/Speech/SSI-263A%20Programming%20Guide.pdf),
page 4, uses FF `$E9` (233) in its nominal parameter table; the original Phasor
demo uses 232. A fixed byte is not a clock-independent neutral setting.

Preserve these independent behaviors:

- FF changes the tract response without changing excitation pitch, speech
  duration, articulation cadence, or request/IRQ behavior.
- FF255 is a valid fastest setting. It is not silence and must not be clamped
  to an arbitrary normal-range value.
- Preserve register aliases 5–7 to register 4 and the existing bus selection.
- Preserve CTL, reset and warm-reset register semantics. An FF write must not
  restart the phoneme. Measure transition behavior before claiming physical
  continuity; do not reset filter histories merely to simplify an FF update.
- Direct SC-01/Votrax playback must retain its separate contract.
- Inflection packing is already correct: R1 holds I10..I3; R2's low nibble
  holds I11,I2,I1,I0. Do not introduce an extra shift or divide-by-eight fix.

## Source map and current limitations

Line numbers describe the inspected revision; use symbols after code changes.

| Source | Relevant behavior |
| --- | --- |
| `hdl/apple/ssi263_formant_backend.sv`, around 134–158 | `ssi_filter_step()` returns `128 + FF`; FF128 is the provisional one-pass reference. |
| Same file, around 1060–1076 and 1446 | Fractional scheduler allows zero, one or two tract passes per 48 kHz output sample. Extra passes reuse held excitation. |
| Same file, `voice_source_from_pitch`, presence/attack/output stages | Heuristic excitation, noise gain, presence boost, attack shaping and slew limiting affect the result beyond FF. |
| `scripts/build_ssi263_formant_rom.py`, constants near 27 | Coefficients use a 20 kHz capacitor-clock reference and a distinct 48 kHz DSP sample rate. Filter parameters derive from the SC-01 model. |
| `hdl/apple/sc01a_digital_core.sv`, around 314–329 | `ssi263_pitch_period_limit()` uses `floor((4096-I)*5/32)` on a nominal 20 kHz source cadence. |
| Same file, around 705–787 | Duration follows Q3/DIV2; source pitch still uses the DAC-derived control cadence. |
| `hdl/apple/ssi263_xck_ce.sv` | Existing synchronized Q3 clock-enable source; avoid adding a separate fabric clock domain. |
| `hdl/apple/ssi263_formant_pkg.sv` | Native SSI/SC-02 parameter ROM is already present alongside SC-01 data. |
| `docs/SSI263_SC02_ROM_FORMAT.md`, “Current hybrid behavior” | Upper-nibble targets are used; lower-nibble TPARM paths are not yet executed. Some phones differ in those controls. |
| `hdl/apple/mockingboard.sv` | Mixing, gains and final tone controls also affect a recorded comparison. |

Read [SSI263_FILTER_FREQUENCY.md](SSI263_FILTER_FREQUENCY.md),
[SSI263_SC02_ROM_FORMAT.md](SSI263_SC02_ROM_FORMAT.md), and
[SSI263_SC01_HYBRID_PLAN.md](SSI263_SC01_HYBRID_PLAN.md) before editing.
The current filter document explicitly labels the linear FF mapping as
provisional. Existing tests encode that provisional contract; passing them
unchanged does not prove physical-chip compatibility.

## FPGA implementation plan

### 1. Establish an independent clock/register reference

Build an arithmetic reference from the datasheet equations for all 256 FF
values and both regional clocks. Include FF0, FF128, the normal range and
FF255. Model immediate inflection from its full 12-bit word and actual
effective XCK. Preserve transitioned-inflection mode semantics separately.

Use the physical clock enables already available. Keep source pitch, tract
response, duration and articulation as distinct mechanisms. Correcting the
pitch-clock approximation prevents a residual tuning error from confusing
the acoustic comparison. Keep a fixed register trace as input to both the
reference and RTL; do not retune the trace to hide an implementation error.

### 2. Replace the limited tract-rate scheme

Changing `128 + FF` to a reciprocal lookup is insufficient by itself. Relative
to the current 20 kHz reference, PAL FF255 would require about 25.39 times the
recurrence rate. A simple extension would run 25 or 26 passes per output
sample. At the documented roughly 143 cycles per extra pass, that exceeds
the roughly 2,778 fabric cycles available at 133 MHz / 48 kHz.

This is a budget estimate for the existing architecture, not a new routed
timing result. Check the real clocks and schedule in the current project.
Do not silently clamp the rate or claim high-register fidelity from a
normal-range-only implementation.

Preferred direction: implement an SSI transfer model whose coefficients
depend on the physical filter clock, at a defined internal sample rate, with
proper conversion to the output rate. Compare coefficient banks, computed
coefficients and oversampling against DSP/BRAM use, numerical stability and
worst-case processing time. A redesigned variable-rate engine is also viable
if it has enough throughput and proper interpolation/decimation.

Keep these distinctions explicit:

- A 20 kHz physical filter clock does not mean “run the present 48 kHz
  coefficients 20,000 times per second.” Their sample-rate assumptions differ.
- Repeating or holding output samples without reconstruction filtering can
  introduce images. A correct average rate is not a complete transfer model.
- Stable fixed-point arithmetic, pole placement, gain, clipping and state
  changes matter at extreme FF values as well as in normal speech.
- The SC-01-derived capacitor/filter parameters remain an approximation until
  measured against SSI recordings. The native phoneme ROM alone is not enough.

### 3. Calibrate the audio and complete missing SSI behavior

Use isolated recordings to fit resonance positions, widths, relative gains,
voiced/noise spectra and transitions. Review current presence boosts, attack
boosts, fricative gain and slew limiting against those measurements. Do not
retain or remove them solely because they make one song sound more pleasant.

Track unimplemented native ROM control paths explicitly. Register-law work
can land before all source/envelope details are resolved, but do not call the
whole SSI model complete while known distinctions remain absent. Keep the
direct SC-01 path unchanged and test it separately.

Preserve the native ROM's identity, hash and addressing. Separate new SSI
coefficient generation from shared SC-01 tables so changing the generator
cannot silently change Votrax playback.

### 4. Update the software contract and existing demo assets

Once the physical contract is the firmware default, use the same physical
register stream on Appletini and the real card. Old Appletini exports encoded
FF near 128 under the provisional law; they will sound different under the
physical law. Recompile the song/demo assets and bump their declared firmware
profile, or retain a clearly named legacy profile if compatibility is required.
Do not auto-detect the intended law from an FF byte.

The PHS1 stream format does not carry a profile identifier. The software
report does. Keep reports with streams, update pinned firmware hashes and
preview/fitting profiles, and retain old goldens only for the legacy contract.

## Calibration capture plan

The next physical test should be a small deterministic calibration disk with
an exact register/timestamp manifest. It has not yet been built or recorded.

For an initial capture of a few minutes:

1. Silence AY accompaniment. Exercise SSI0 and SSI1 separately, while recording
   stereo. Keep card and capture gains fixed and leave headroom.
2. Hold five vowels spanning tract settings, such as E, AH, AW, U and ER, at
   fixed low pitch (about 90–120 Hz), amplitude, rate and articulation.
3. Step through FF128, 192, 216, 224, 228, 230, 232, 236 and 240. As a starting
   schedule, allow about 650 ms per sound plus 200 ms separation. Check that
   this leaves a settled interval; increase it where necessary.
4. Repeat a reference segment at the end to check drift. Add a separate AY
   reference tone or sequence to help align captures and estimate common
   recording-path gain. Keep it out of the speech measurement intervals.
5. Save lossless WAV, preferably 48 kHz/24-bit or 96 kHz/24-bit, without AGC,
   EQ, noise removal or normalization. Record the chip/card details and clock.

Then extend the sequence without testing every possible parameter combination:

- All 64 phones at a few representative FF values, including noise phones.
- Selected vowels at a second and third pitch, to separate the excitation's
  harmonics from the filter response.
- Separate amplitude sweeps and live FF steps during a held phoneme.
- Articulation transitions, immediate/transitioned pitch, and CTL/start/stop.
- A broad 0–255 FF sweep for endpoints and stability, with denser acoustic
  fitting near the normal range around 224–240.

Capture the same raw writes on Appletini with bass, warmth and treble neutral
(including warmth 0). RTL renders speed iteration, but an Appletini line-out
capture also tests its mixer and DAC path. Phasor line-out measurements include
that board's analog circuitry; document whether the target is board output or
the chip alone. A chip-only claim needs the output path characterized separately.

Set acoustic tolerances using repeated captures and differences between the
two SSI chips. Compare resonance frequencies, widths, spectra, level and
transition timing. Phase-identical PCM is not an appropriate target for
free-running sources and noise. Reserve some phones, phrases and this song
for validation rather than fitting everything to the evaluation material.

## Simulation, Vivado and board validation

Replace the provisional linear FF oracle with independent datasheet-based
checks. Preserve unrelated bus, CTL, duration, IRQ and SC-01 regressions.
Cover all 256 values, both sockets and clocks, aliases, writes during active
speech, reset/warm reset, state continuity, signed bounds, output cadence and
worst-case throughput. Compare source/timing state before and after an FF
write to ensure that filter changes cannot alter those mechanisms.

Existing focused entry points, run from the firmware repository root with
Vivado simulator tools on PATH:

```sh
python scripts/test_ssi263_filter_frequency.py
python scripts/test_ssi263_filter_neutral.py
python scripts/test_phasor_demo_filter.py
python scripts/test_ssi263_duration_lookup.py
python scripts/test_ssi263_sc01_hybrid.py
python scripts/test_ssi263_hybrid_core.py
python scripts/test_ssi263_filter_finalize.py
python scripts/test_ssi263_history_mask.py
python scripts/test_ssi263_start_timing.py
python scripts/test_ssi263_xck_ce.py
```

Install the tests' Python dependencies, including NumPy. The legacy-neutral
test also reads Git commit `310193469104b5e07b847631129223467b043cfd`; make that
history available if the local checkout is shallow.

The neutral test currently checks old FF128 behavior; revise or split it
deliberately rather than changing a golden waveform without explanation.
Add spectral/clock tests that can fail an incorrect divider or a rate clamp.

After relevant simulations and source checks pass, use the repository build
flow and inspect synthesis, utilization and routed timing:

```sh
vivado -mode batch -source scripts/create_project.tcl
vivado -mode batch -source scripts/build_and_export_xsa.tcl
```

Follow current `AGENTS.md` and build-script requirements. Check constrained
paths, clock-domain crossings, generated clocks and unconstrained paths.
Resolve any constraint queries that match no objects. Record DSP/BRAM/LUT/FF
use, worst slack and worst per-sample processing time for both speech sockets
active. Simulation passing does not establish FPGA timing closure.

Finally, run the calibration sequence and physical-profile song on Appletini.
Compare captures against the real card with one documented alignment and gain
policy; do not tune alignment, pitch or EQ separately for each phoneme to
conceal model errors. Check full-song timing, both SSI outputs, AY/speech
balance, resets, and sustained playback at the normal host clock.

## Existing software and reusable artifacts

The physical export lives in
[`hasseily/appletini-software`, branch `codex/physical-phasor-ssi`, commit `85bd75c`](https://github.com/hasseily/appletini-software/tree/85bd75cf9604ad9ebef688fb42890e21bbfd2478).

- Framework: `music/song_to_phasor/phasor/hardware.py`, `compiler.py`, `cli.py`.
- Profile: `physical-ssi263`; effective-clock override:
  `--ssi-effective-clock-hz`. PAL default is 1,015,625 Hz.
- Normalized score filter `f` requests `20000 * (128 + f) / 256` Hz; the physical
  compiler chooses the closest physical FF. This score authoring convention is
  separate from the raw SSI register law the FPGA must implement.
- Disk builder: `music/house_of_the_rising_sun/player/build_physical.py`.
- Ready test disk: `music/house_of_the_rising_sun/RISING.SUN-Physical-SSI263.zip`.
  The disk defaults to PAL, offers N/P selection, and requires native Phasor
  slot 4 and a 65C02. Both SSI sockets receive the full vocal.
- Initial diagnosis: `music/house_of_the_rising_sun/PHYSICAL_PHASOR.md`.
- Disk SHA-256:
  `a2c937afb905d158fca0abf64cfe4114404f401bcd69946db8f4835ecdca062c`.

The existing physical disk has passed complete assembled-player checks:
9,392 PAL register writes, a busiest polling iteration of 4,879 nominal CPU
cycles out of 10,156 available, and no disk reads during playback. These
checks validate the player and stream, not analog speech fidelity.

The private input MP3s and analysis audio are not committed. The preparation
workspace has measurement notes and scripts under the software repository's
ignored `music/house_of_the_rising_sun/build/physical-followup-analysis/`.
That directory is not part of a Git checkout; the evidence summary above is
included so this handoff remains usable without those local files.

## Completion criteria for the local FPGA work

- Datasheet-based FF and pitch behavior uses the host's effective chip clock.
- All FF bytes have defined, stable behavior, without an arbitrary rate clamp
  or missed output deadline.
- FF writes preserve independent source/timing/bus behavior; direct SC-01
  regressions pass.
- Synthesis and routed timing pass with documented resource and cycle budgets.
- Controlled real-chip and Appletini captures quantify acoustic agreement and
  remaining differences across phones, settings and the two SSI chips.
- Updated software profiles and demo assets can use the same physical SSI
  register stream, with an explicit policy for old FF128-based content.

Report these as separate milestones. Correct register semantics, a working
bitstream and calibrated sound are distinct results; state which have actual
test evidence and which remain pending.
