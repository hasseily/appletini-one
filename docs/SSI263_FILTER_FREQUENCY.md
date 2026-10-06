# SSI263 filter-frequency control

## F1.2.5-d1 native engine

The native engine now uses the SSI divider:
`filter_clock = effective_XCK / (2 * (256 - FF))`. At PAL effective XCK,
FF128 gives about 3.97 kHz, FF230 about 19.53 kHz and FF232 about 21.16 kHz.
FF128 is no longer a normal-sounding reference setting. These are internal
filter-clock frequencies, not the voice fundamental.

The original Phasor demo labels R4/FF **Pitch**. Its checked Ver 1.1.0 image
defaults to 232. The earlier advice to select 128 applied only to the legacy
filter approximation below; do not carry that adjustment into the native
engine. The tested mb-audit phrase streams use FF230 or FF233. Comparing
different applications does not isolate a Mockingboard/Phasor mode difference.
Both modes feed the SSI engines from the same Q3/DIV2 clock path.

The real-card regression now replays reset-identical SSI writes and Q3/sample
timing in both modes at FF128 and FF232. All 4,096 raw socket PCM comparisons
are byte-identical, with one valid sample per request. This checks the current
core's mode invariance for those traces; it does not make different programs
write the same parameters.

On 2026-10-06 the user reported that mb-audit speech sounds correct on the
new firmware, while the Phasor.hdv demo sounds too low until its Pitch control
is raised to 230, with remaining audible differences. The checked demo image
establishes the control's meaning, but is not yet verified as that exact HDV.
The returned physical capture supports the native steady pitch and identifies
noise/filter balance differences; see the [capture review](SSI263_CAPTURE_COMPARISON.md).

## Legacy F1.2.4 implementation

The sections below retain the earlier implementation and test history. Their
linear FF mapping and 128 reference apply to that backend only.

Register 4 now changes the speech tract instead of being stored without an
audio effect. Addresses 5–7 remain aliases of register 4. This applies to SSI263
speech; the SC-01/Votrax path ignores this register as before.

The [SSI263A data sheet](https://downloads.reactivemicro.com/Electronics/Speech/SSI-263A%20Data%20Sheet%20v2.pdf)
defines the switched-capacitor filter clock as `XCK / (2 * (256 - FF))`.
Increasing FF raises the tract frequencies. It does not raise the excitation
pitch, change phoneme duration or articulation, or request another phoneme.
`$FF` is the fastest setting, not a mute command. CTL and amplitude retain their
existing roles.

## Provisional range

The current backend uses SC-01-derived filter coefficients. Pending recordings
from a real SSI263, the control uses the linear rate `(128 + FF) / 256`.
All 256 byte values select distinct, evenly spaced rates. `$80` (128) selects
exactly one filter pass per output sample and preserves the pre-FF response;
it does not bypass the existing speech filters.

| FF | Relative tract rate |
| --- | ---: |
| `$00` (0) | 0.5× |
| `$40` (64) | 0.75× |
| `$80` (128) | 1×, unchanged response |
| `$C0` (192) | 1.25× |
| `$FF` (255) | 383/256×, about 1.496× |

This is a provisional digital control range, not the chip's reciprocal divider
law or a measured analog response. It removes the broad flat regions of the
first F1.2.4 implementation. Power-on register 4 remains `$00` (0.5×) and CTL
remains asserted, so reset is silent. Software must write `$80` for the legacy
response; an unwritten FF register does not select neutral. Warm reset retains
register 4. There is no new user setting.

## Implementation and limits

A fractional accumulator schedules zero, one or two passes through the existing
F1, F2, noise-shaping, F2-noise, F3, F4 and final tract filters per 48 kHz sample.
It holds the excitation and gains for any second pass. A skipped pass holds the
last tract output. The source and noise generators, articulation, duration,
request/IRQ logic, amplitude/output shaping, presence and slew limiting still
advance at their existing cadence. Filter histories persist across an FF write;
the new rate applies to the next sample and cannot alter a pass in progress.
Existing phone-start history handling remains unchanged.

The control adds a registered nine-bit rate, an eight-bit phase and one repeat
bit per backend. It reuses the MAC and coefficient tables and adds no DSP,
coefficient bank or RAM. The corrected mapping measures 151 fabric cycles at
neutral and a maximum of 294 for two passes, versus roughly 2,778 available at
the 133 MHz fabric clock. FPGA timing remains a separate build gate.

The fractional schedule repeats or holds samples without an interpolation
filter. Its average rate shifts the whole tract but can introduce resampling
images and does not reproduce every analog bandwidth or gain change of an
SSI263. The coefficients, output shaping and the range still need real-chip
calibration. Do not describe this as a calibrated SSI263 analog model.

## Duration reload timing

The first routed F1.2.4 build exposed a long path from live bus data through
the existing duration reload arithmetic. The new FF scheduler was not on that
path. The duration core now uses an exact 64-entry lookup for the six control
bits `D[1:0]` and `R[3:0]`. It returns `(4-D)*(16-R)*256-1`, with the low eight
bits fixed at `$FF`, without a multiply/subtract carry chain in the source.

The lookup adds no register or clock delay. An accepted RATE write still takes
effect when a duration slot reloads on that same fabric edge; the slot already
in progress keeps its count. Phone starts, duration/DONE, response/IRQ and
SC-01 behavior remain unchanged. Fresh synthesis and routed timing must check
the resulting circuit; simulation alone does not establish timing closure.

## Validation

Run with the Vivado simulator tools on `PATH`:

```text
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

The corrected mapping passes all 256 distinct rates, writes during active
speech, all seven tract pass counts, source/timing state, reset, warm reset,
card disable, one output commit per sample, signed bounds and unchanged SC-01
output. A signed F1 recurrence checks 3,072 real RTL samples. Impulse peaks
measure about 258 Hz at FF0, 516 Hz at FF128 and 775 Hz at FF255.

An independent test extracts the actual F1.2.2 backend from commit `3101934`,
changes only its module name, and compares it with the new backend at FF128.
It passes 9,465,035 cycle comparisons and 24,584 exact PCM samples, covering
12 phonemes, a long stream with changing voice controls, resets, mute and
SC-01 playback. The unchanged coefficient package is pinned. Both sides use
the current duration core, whose lookup has its own exhaustive arithmetic
check. FF127 fails the same neutral check as a negative control.

The real-card test instantiates `mockingboard` and both production SSI263AP
voices. It passes 6,705 bus writes, all 256 FF values through aliases 4-7 on
both sockets in Mockingboard and native modes, and rejection of Echo+ and
unselected/read cycles. Its 3,072 audio frames confirm neutral equality and
nonzero, changed output at both endpoints without changes to source timing.

The original [Phasor demo disk](https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/Phasor/Software/PHASOR1.DSK),
Ver 1.1.0, sends its Pitch byte at RAM `$770F` to SSI register 4 (`$C444` in
slot 4). Its default is 232; set **128** for this firmware's legacy response.
Inflection is a separate control. Executing both TTS copy routines for all
256 Pitch values confirms literal byte writes. The card test replays that
initialization. This does not identify the user's exact disk revision.

The first F1.2.4 build incorrectly clamped values 0-221 to one rate. Its tests
checked that clamped specification and the former neutral at `$E6`, so they
did not catch the missing full-range control or the requested 128 neutral.

At the new `$80` neutral, the pre-FF reference waveform retains SHA-256
`cfbf54f71df24b36d399ac90c5efb56daa9ce1a99d4751e895b9b896fcb5aea7`.
The focused test saves logs, samples and source hashes under
`build/ssi263_filter_frequency_sim/`.

The duration lookup test calls the production RTL function for all 65,536
duration/rate byte pairs, covering all 64 control combinations and all ignored
phone/inflection bits. Its independent integer-arithmetic oracle also rejects
an isolated copy with one incorrect entry. It saves results and logs under
`build/ssi263_duration_lookup_sim/`; the hybrid core regression checks duration
reloads and RATE changes, including coincident boundaries.

For a board test, play a sustained vowel followed by a phrase at `$00`, `$40`,
`$80`, `$C0` and `$FF`, holding pitch, rate, articulation and amplitude constant.
Compare `$80` directly with the pre-FF firmware, then sweep 0–255 during speech.
The tract should progress from darker to brighter while source pitch and
request cadence stay fixed. Check both SSI sockets, power/warm reset, and
unchanged SC-01 playback. The corrected mapping's hardware test and real-chip
calibration remain pending.
