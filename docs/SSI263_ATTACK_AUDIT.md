# SSI-263 host attack audit

The user approved the balanced version as our listening reference on
2026-10-07. The [host reference preset](SSI263_HOST_RENDERER.md#accepted-balanced-reference-2026-10-07)
pins its parameters and accepted audio hashes. Approval selects a reference;
it does not resolve the measured differences or update the built firmware.

## Physical capture follow-up, 2026-10-06

The user hears smoother attacks on the real Phasor than in our House of the
Rising Sun render. That remains an open sound-quality issue. A volume-rise
measurement alone does not measure perceptual smoothness: a short noise burst,
spectral change, output step or resonant overshoot can sound harsh even when
the RMS envelope rises slowly.

The first [physical capture review](SSI263_CAPTURE_COMPARISON.md) provides
useful constraints. In conditioned AH starts, the real chips rise in roughly
35–40 ms and our native candidate in 60–70 ms using the same 20 ms RMS measure.
This does not disprove the listening report and does not justify making our
amplitude ramp still slower.

The stronger measured difference is frication relative to the vowel. In the
matched AH–HF–AH probe, physical HF is about 23 dB below AH, versus 8 dB in
native. For S the figures are about 19 dB and 2 dB. Both chips show this, and
the ±25 ms global timing uncertainty changes the contrasts by less than
0.5 dB. Voiced TH also has a large excess of high-frequency power in native.
These sustained differences can affect perceived attacks as well as the body
of each phoneme. They identify a source/filter/output-path investigation,
not a measured source-gain correction: the physical card and recorder are
absent from the model.

Check the onset spectrum, frication-to-voicing balance, discontinuities and
filter overshoot against this capture and any available original song audio
before adding a generic fade. A new fade would depart from the prototype's
short U68/U206 envelope described below. The source's fixed voice/noise gains
and ideal output path remain assumptions; evaluating them does not establish
that the prototype's digital envelope is wrong. The cold first-H rule also
remains unverified by these conditioned probes.

No attack or source parameter changed in this review. The following sections
record the earlier model-only audit and its prototype evidence.

The current song render's PAL command stream is byte-identical to `SUN.PAL`
in the checked physical-song disk. The later mixed preview raises only the
SSI mix gain by 1.25 (+1.94 dB); it keeps the vocal waveform and writes.
Six song onset contexts are saved in
`build/ssi263_host/hardware_capture_20261006/song_attack_register_contexts.csv`.
The user subsequently supplied the corrected physical `risingsun.mp3`.
Its SHA256 is `095690407f6c125c8b7bc2960e7cf116e488b58df510ef4a919acc14dfd93eec`.
The original stays unchanged; the analysis decodes it to float WAV at its
original 44.1 kHz, with no resampling, gain or EQ. Eight instrumental windows
give `recorded_seconds = 0.0709137384 + 0.9999809219 * score_seconds`.
Three unused instrumental checks agree within 0.14 ms, but attack comparisons
retain a conservative ±20 ms allowance for windowing, MP3 and system effects.
The recording covers the full song and has no decoded clipping. Its stereo
socket mapping is unresolved; do not assign those channels to chips by guess.

`build/ssi263_host/hardware_capture_20261006/attack_spectrum.png` compares
quiet–AH, AH–HF and AH–S envelopes and 4–20 kHz band levels. The companion
`attack_spectrum_plot.py` and JSON retain the method and curves. Each source's
conditioned AH AC RMS supplies a declared plotting reference; this changes no
audio. The graph uses the single global marker map and marks its ±25 ms
uncertainty. The native high-band excess persists after the transition, so
attack duration alone cannot explain it.

### Source-balance experiment

The archived change `2c528cca72939a658aee1faf101f21e3c0e171f3` reduced the
voice-only drive from 65536 to 2048 (−30.10 dB), kept noise at ±301 and added
a common output multiplier. Its comment calls the voice trim provisional.
The prototype drawing has an adjustable POT3 voice source without a specified
wiper setting. The ±301 noise drive comes from the drawn 390 kΩ/1.8 kΩ divider
ratio in normalized Q16 units. The current host inherited voice trim 2048 and
uses common gain 8 for listening headroom. Neither value was measured on SSI.

A separate host-only trial uses voice trim **16384** and common gain **1**.
The ideal voiced product stays `2048*8 == 16384*1`, while frication relative
to voicing falls by 18.06 dB. ART, ROM, register trace, source gates, filters,
pitch and envelope timing stay fixed. The song retains the existing 1.25 SSI
mix multiplier and byte-identical AY backing. No production source or firmware
default changed.

The full calibration and song trials have zero internal state saturation and
zero output clipping. Calibration counters and event totals match the frozen
render. Fixed-point rounding prevents bit-identical voiced PCM, but AH level
changes by only +0.0018/−0.0054 dB in the guarded measurement windows.

| Guarded phone measurement | Physical chips | Current model | Balance trial |
| --- | --- | --- | --- |
| THV power in 4–20 kHz, as fraction of 20 Hz–20 kHz | 7.39 / 5.84% | 80.20 / 79.08% | 8.09 / 7.47% |
| S level relative to AH | −19.20 / −19.23 dB | −1.67 / −1.77 dB | −19.73 / −19.83 dB |
| HF level relative to AH | −19.74 / −20.16 dB | −4.46 / −4.45 dB | −22.53 / −22.51 dB |

These figures use the phone-survey's 200–350 ms tails, not the conditioned
AH–HF–AH probe above. The trial removes much of the gross noise excess and
the Z-like voiced-TH balance. It makes HF about 2.4–2.8 dB too quiet; other
fricatives also retain smaller level errors. Pure-S spectral shape stays too
bright because a relative gain cannot change a noise-only phone's spectrum.
This is a candidate for listening, not a final fit or proof that every attack
is correct.

**Prototype status:** changing an unspecified POT3-like setting and unmeasured
common gain does not contradict a known setting in the drawing. The trial
does not change a drawn capacitor, noise divider or route. It does not settle
the existing U166B, CTL/source, cold-state or duration policy departures.

The full song and eight matched excerpts are at
`build/ssi263_host/song_hardware_20261006/listen.html`, with real hardware,
current model and balance trial. Clips keep original levels and sample rates;
they do not fit gain or align individual attacks. The real recording includes
AY, so the page also offers the original model's separate SSI and AY stems.
`alignment.json`, `balance_trial_metrics.json`, `source_gain_audit.json` and
`listen_manifest.json` preserve timing, exact render commands, source/input/
output hashes, assumptions and independently checked measurements. The original
MP3 is lossy; avoid interpreting very short transients as direct chip voltages.

### Song checks of the balance trial

The trial parameters came from the calibration capture, not a fit to individual
song words. The song provides a separate check. Near the S at score 21.75 s,
2–10 kHz RMS relative to a selected AE vowel window is about −18.0 dB on
hardware, −2.7 dB in the current mix and −20.7 dB in the trial. Moving the S
window ±20 ms gives roughly −19.5 to −17.7 dB on hardware. This supports the
balance correction while suggesting that the full 18 dB reduction can be too
large. The window approaches the following B/percussion event, and MP3 can
spread transient energy; this is not an isolated physical noise-source voltage.

The S-to-I transition at score 83.03 s also shows a remaining difference. In a
40–80 ms window, 2–10 kHz power as a share of 70 Hz–20 kHz power is about 6.6%
on hardware (2–21% under the timing check), 81.6% in the current mix and 24.7%
in the trial. A 200–1500 Hz vowel-band proxy reaches within 3 dB of its later
level at roughly 70 ms on hardware, versus 150 ms in both model versions.
The hardware estimate spans roughly 55–90 ms under the time-map allowance.
These are mixed-output measures, not direct internal gate or envelope timing.
They call for a separate transition/state investigation; source balance alone
does not resolve this handoff. The checked crest factors do not show a general
excess of model peak overshoot.

`attack_metrics.json`, `attack_holdout_metrics.json` and their analysis scripts
retain the song measurements. `song_attack_brightness.png` plots three full-mix
transitions, with a common 70 Hz–20 kHz denominator to exclude sub-70 Hz envelope
and window leakage. The model's AY stem helps choose windows but is never
subtracted from the physical recording or presented as a separated real SSI.

## Earlier model-only audit

The native candidate already has the prototype's fast U68 envelope and
U206 amplitude mask. This audit found no missing soft ramp to add. Its
remaining selector-4 amplitude approximation differs from the prototype,
but the tested attacks start after that parameter has reached its target.
There is no evidence yet that changing selector 4 would correct the attack
the user hears.

No sound code, gain, filter, ROM, waveform or listening fixture changed.
The audit starts from checkpoint `41ac989`. The physical SSI capture is
still needed to establish production-chip attack behavior.

## Reproduce the audit

```powershell
python scripts/audit_ssi263_attack.py
python scripts/test_ssi263_attack_audit.py
```

The script traces A, B and E from the mb-audit listening fixture at RATE=$B
and $C, plus the four-hello demo at FF=231. It uses the same register bytes
and nominal request times as the listening page. It does not add a reset,
warm-up phrase or hidden register write. The mb-audit timing still omits
6502 IRQ latency.

Results go under `build/ssi263_host/attack_audit/`:

- `manifest.json`: input provenance, source hashes, measured onsets and limits.
- Each clip's `states.csv`: bus writes and changes in PW0/PW1/PW2/PW3/PW5,
  U20, selector-4/5/6 DDA state, AMP/VA/FA, U68 and U206. Each row also gives
  the scanner phase, source drives and tract output at that instant.
- Each clip's `diagnostic.pcm`: every 48 kHz mono output sample.
- Each clip's `reference/prototype.wav`: an ordinary stereo host render.
- Each clip's `summary.json`: phone boundaries, source onset measurements,
  U68 rises and output levels.
- `prototype_evidence/`: exact archived netlist and drawn SVG sheets, their
  hashes, and 299 relevant pin/net memberships.

The diagnostic replays socket 0. Every sample must equal socket 0 of the
ordinary renderer, or the audit fails. All seven clips passed this check.
State CSVs record changes, not every XCK tick; their occasional output
columns are not a replacement for the full PCM stream.

## What the traces show

All tested source onsets have host AMP=12 before excitation starts. The
first H begins with AMP=9 at its phone write, but AMP reaches 12 before FA
becomes nonzero. The provisional host-amplitude ramp is therefore no
longer rising during that H's audible attack.

U68 then makes a short binary count rise. U70 masks individual amplitude
bits; it does not multiply the audio by a smooth gain. With host AMP=12,
the first U206 rise in every clip is `0 -> 4 -> 8 -> 12`.

| Clip | First U68 count rise to 15, ms | First increment to U206=12, ms | First nonzero PCM after first non-PA phone write, ms |
| --- | ---: | ---: | ---: |
| A, RATE=$B | 1.710 | 1.336 | 34.006 |
| B, RATE=$B | 1.733 | 1.359 | 35.319 |
| E, RATE=$B | 1.764 | 1.394 | 91.298 |
| A, RATE=$C | 1.704 | 1.336 | 27.317 |
| B, RATE=$C | 1.727 | 1.359 | 34.338 |
| E, RATE=$C | 1.764 | 1.390 | 73.150 |
| Four-hello, first H | 1.723 | 1.380 | 50.750 |

The count-rise column measures the first increment through the final
increment, not the complete wait from source enable. A 0-to-15 rise has
15 increments but 14 intervals between them. In E, source enable precedes
the first increment; enable-to-15 takes 1.786 ms. The summary records both
times. Gate opening while SEL2 is already high can place the first count
between regular scanner boundaries.

E's first non-PA phone is $27. Its fricative amplitude becomes nonzero,
but PW3 remains set, U68 stays at zero, and its PCM remains silent. The
count rises during the following $20 phone. Thus E's longer interval in
the last column does not describe a slower U68 ramp. The audit reports
this gate sequence; it does not claim it matches a production SSI.

The four H sounds all use FRIC1. Their first positive FA occurs 50.336,
48.664, 49.009 and 49.365 ms after their respective phone writes. The first
U68 rise starts at count 0; later H rises start at count 1, which the
prototype's upper-bit zero detector also treats as zero. These residual
state and scanner-phase differences do not justify forcing repeated
phonemes to produce identical PCM.

All seven clips reported zero output clips and zero internal state
saturations. Phone-level peak/RMS measurements include any preceding
phone's retained filter tail. A nonzero sample just after a phone write
must not automatically be called that phone's new attack.

## Prototype evidence for the existing envelope

The source is the seven-sheet prototype archive at
`502ae04f68f04e23ce04abaaf0d44990c71e763a`, especially
`schematics/sc02_prototype/sc02_schematic.json`,
`connectivity/sc02_pin_net_memberships.csv`, and drawn sheets
`source_svg/06_digital_4.svg` and `07_digital_5.svg`.
Their pin memberships come from the original smart PDF's component/net
records. The archived `ssi263_sc02_core.sv` agrees with the findings below,
but was only a cross-check, not the primary evidence.

- Sheet 6 U68 is a CD4029. B/D pin 9 connects to VCC, so it counts in binary.
  PSE pin 1 and CI pin 5 connect to ground. Q1, pin 6, has no consumer;
  Q2/pin 11, Q3/pin 14 and Q4/pin 2 supply AMPCT1, AMPCT2 and AMPCT3.
- U69B NORs those upper three bits. Counts 0 and 1 both assert AMPCT_ZERO.
- U71B drives U68 clock pin 15 from gated SEL2. This is the scanner clock,
  not ART or RATE. The terminal gates stop the up-count at 15 and stop
  the down-count when the upper bits reach zero.
- Sheet 7 U70A-D are four CD4081 AND gates. They combine U111's stored host
  amplitude bits with AMPCT0-3. AMPCT0 comes from /U104C, not U68's unused
  Q1. The resulting amplitude is a bitmask.
- U206 is a CD40174 with clock pin 9 on Phi0 and clear pin 1 held at VCC.
  It samples those four masked bits on the positive Phi0 edge.

These connections support the short stepped envelope already implemented.
With one regular eligible count per 128 XCK, the 14 intervals from count
1 through count 15 take 1.7646 ms at 1,015,625 Hz. Adding a much slower
generic fade would conflict with this prototype circuit unless later
production-chip evidence supports it.

The tests check every observed U68 count change against the drawn binary
direction gates and every U206 output change against the four AND gates
and Phi0 sampling edge. They establish internal gate consistency, not the
timing of real gate propagation or the correctness of prototype-to-SSI
assumptions.

## Selector-4 differences confirmed from the drawing

The prototype's settled selector-4 permit at U96 pin 1 is:

```text
permit4 =
    (sampled_DURCLK_rise OR (RATE == 15 AND P2_pin2))
    AND U166B./Q
    AND (host_AMP != 0)
```

U169 samples DURCLK on SEL2 rising. Its Q3 and inverted Q4 reach U102A,
which detects the sampled rise. U177A/B require all four RATE bits high
and `NetP2_2`; U208C adds this bypass to the duration pulse. The drawing
says P2 was grounded on the prototype, with R301 pulling the input down.
The production chip's corresponding choice is unknown.

U166B is a CD4013. D9 is high, clock11 is `/WR_3`, and set8 is low. A
completed control write sets Q13 and lowers /Q12. The following control
setup pulse from U169, through U167A, resets it through pin10. U178D ANDs
/Q with `/AMPZERO`; U167C adds the duration/bypass term. U161A/B make
`/AMPZERO` high only when the host AMP nibble is nonzero.

The control-write setup gates U162B, U102B and U168C/A give:

```text
A_CLR_control =
    control_setup
    AND (state_is_5_or_6 OR (STATE4 AND host_AMP != 0))
```

The drawing names the combined state-5/state-6 signal `STATE5&6`.

The current host deliberately differs in three ways:

1. It permits selector 4 from a software duration window and has no
   separate U166B write-pending inhibit.
2. It permits selector-4 transitions toward AMP=0. The drawn prototype
   blocks these transitions while the host nibble is zero.
3. It retargets selector 4 on AMP=0 control writes. The drawn setup gate
   excludes selector 4 in that case.

The omitted RATE=$F/P2 bypass has no effect on these RATE=$8/$B/$C traces.
The existing setup window prevents a normal transition step during setup,
which covers part of U166B's job, but that is not proof of equivalent
behavior for closely timed writes.

These are concrete prototype conflicts and should remain explicit. A
future U166B implementation should be tested with control writes around
SEL2 and duration boundaries, including zero-AMP writes, before choosing
which behavior belongs in the production SSI model. This audit does not
silently replace the existing policy with the possibly buggy prototype.

## What remains unknown

The measured attack still passes through several assumptions: fixed ART
reference RATE=8, full duration restart on phone writes, the CTL hard-mute
policy, cold state, and ideal analog charge transfer. Simultaneous gate
and Phi0 events use host ordering rather than measured propagation.
The source's FRIC1 cold fallback also remains a listening-based choice,
not a known prototype reset rule.

Output is sampled directly at 48 kHz; real Phasor mixing, reconstruction
and op-amp response are absent. This audit made no change to that path and
does not identify it as the attack's cause. The current evidence supports
preserving the candidate audio while these limits and the physical WAV
are checked.
