# Fast SSI-263 listening tests

`scripts/render_ssi263.py` builds a small C++17 program and renders stereo
48 kHz WAVs. It does not run Vivado, build firmware, or need an Apple II.
The firmware target remains `F1.2.5-d1`.

## Run it

From the repository root:

```powershell
python scripts/render_ssi263.py --demo hello --engine all --compare-ff 231 --bundle
python scripts/render_ssi263.py --demo transitions --engine all --output build/ssi263_host/transitions
python scripts/render_ssi263.py --demo phone_survey --engine all --output build/ssi263_host/phones
```

Open `build/ssi263_host/listen/listen.html` for the first comparison. Each
folder contains the WAVs, the register trace, and a JSON report for each
model. Left is socket 0; right is socket 1. The short `hello` sequence is a
hand-written phone sequence, not a recording or a text-to-speech system.

`--bundle` adds `SSI263-HOST-LISTEN.zip` with the executable, inputs, WAVs
and model notes. Extract it and open `listen.html`. On Windows, `render.cmd`
creates fresh `rerender-*.wav` files without Python or a compiler.

Python needs NumPy. A C++17 compiler is needed when the C++ source changes.
The script checks `CXX`, `g++`, `clang++`, then this machine's standalone
MinGW compiler. Using that compiler does not start Vivado. The Windows
executable links its C++ runtime statically and runs without the AMD tools.
Source and executable hashes control the build cache.

## mb-audit listening phrases

```powershell
python scripts/render_ssi263_mb_audit.py
```

Open `build/ssi263_host/mb_audit/listen.html`. It includes the familiar A-G
phrases from Tom Charlesworth's related SSI player and the original
mb-audit "Classic Adventure" test. Each phrase has a current-engine baseline
and a native prototype candidate. A-G have RATE $A (the player default),
$B and $C (the rates in its published examples), with $B selected first.
The combined WAVs join the seven separately rendered phrases with short gaps.

The pinned `scripts/fixtures/ssi263_host/mb_audit_phrases.json` records the
native SSI bytes sent by the player, after its phrase lookup, plus source
commits, file hashes and attribution. These renders use the SSI engine;
they do not synthesize SC-01 phonemes or regenerate sound tables. A-G keep
the player's FF=$E9, ART=5, AMP=12 and pitch. Classic Adventure keeps its
own five-register rows, FF=$E6 and RATE=$B. Both models receive the same
register trace; their different filter-frequency laws remain visible.

The replay uses PAL effective XCK 1,015,625 Hz and nominal SSI duration
requests. It omits 6502 interrupt latency and competing timer interrupts.
Classic Adventure's initial silent kick uses the host's reset RATE=0;
mb-audit inherits that register from prior activity. Each clip starts from
a fresh model state. The listening ZIP includes WAVs, traces and reports.

## Listening stages

| Profile | What it changes |
| --- | --- |
| `baseline` | Software port of the current engine's sample arithmetic, pulse, noise, coefficient mapping, interpolation and sound shaping. |
| `pitch` | Baseline with exact SSI pitch periods driving the existing pulse shape. An isolated timing experiment. |
| `transitions` | Native eight-slot scanner, raw SSI ROM flags and linear transition arithmetic driving the existing sound generator and filter coefficients, with fractional filter state retained. |
| `prototype` | Native scanner plus envelope, pulse, CD4006 noise, held routes, separate F3/F4 codes and five ideal switched-capacitor formants from the recovered prototype work. |

The profiles allow comparison after each change. They are not four claims
of physical-chip accuracy. No profile changes the running FPGA engine.
The baseline retains the sound the user reports as close to the real SSI;
the prototype candidate has its own sound and unresolved timing assumptions.

## First listening findings

The initial FF=128 demo muffled the prototype candidate. Its exact divider
gives a filter clock of 3,967.285 Hz at PAL XCK. The old models treat FF=128
as their nominal coefficient setting, so equal FF bytes do not imply equal
filter responses. The SSI datasheet suggests about 20 kHz; FF=231 gives
20,312.5 Hz here. The source's U60 pulse also changes from 1.008 ms to
0.197 ms because it spans four filter cycles. Checks of delivered phase
edges and pulse lengths found no extra clock division in the prototype port.

`--compare-ff 231` adds another group to the same listening page. It changes
only the demo's FF writes and applies the resulting trace to every model.
The first group keeps FF=128 for comparison. This option applies only to
built-in demos; it does not rewrite hardware calibration traces. No EQ,
model coefficient changes or level fitting accompany this comparison.

The faint tone during native-transition pauses came from fixed-point
rounding in the retained digital filters. Their state kept oscillating after
VA and FA reached zero. The old closure shaping had hidden that residual.
The host transition adapter now retains fractional filter and output-shaping
state until PCM conversion. It preserves the natural fade and filter decay;
it does not gate silence or reset filters when the sources reach zero.
The baseline and pitch profiles retain their original integer arithmetic.
This is a host numerical fix, not a change to the prototype circuit.

The pitch-only change is small for this phrase: the baseline runs at about
111.11 Hz, and the exact PAL divider gives 110.01 Hz, a difference of
17.2 cents (about 1%). The pulse shape and filters stay the same, so this
short phrase is not a strong listening test for the clock correction.

The prototype path processes source and filter events at effective XCK,
then samples the U148 output hold at 48 kHz. It retains each switched
capacitor's charge across code and phone changes. The baseline retains the
current engine's per-phone filter history masks.

The source uses U60/U62 glottal state, U68 envelope state and U75/CD4006
noise. Native envelope state feeds the scanner's AMPCT_ZERO input. Immediate
pitch words reach the next divider reload. Transitioned pitch still uses
the current engine's glide policy, observed at audio sample boundaries.

## Listening checkpoint pending the Phasor WAV

After the FF comparison and numerical-noise fix, the user reports that the
pitch and transition samples sound indistinguishable from the baseline.
The FF=231 prototype sounds much better, but its first H in `hello` sounds
more like "sss", while the second sounds more like "ha".

The `hello_four` demo repeats the same phrase four times in one continuous
run, with no reset between phrases. The user confirms that only the first H
differs. All four H intervals have the same formant codes: F1=7, F2=9,
F2Q=0 and F3=F4=12. The difference came from the noise route: the first H
used the unverified cold seed FRIC1=0/FRIC2=1 while U20 was still unknown.
The first EH1 initializes U20; the next three H sounds retain FRIC1=1/FRIC2=0.
PA and HF cannot initialize this latch from the preceding PW2=0 state.

A diagnostic copy changed only the route used while U20 was unknown. It
moved the first H's noise spectrum toward the later H sounds, leaving those
later sounds unchanged. The user reports never hearing this first-H hiss
on physical SSIs and directs us to treat it as incorrect. The host source
now starts with FRIC1=1/FRIC2=0 while the held route is unknown. NativeControl
keeps U20 unknown and retains the prototype's gate and phase-latch rules;
known route values still replace the fallback normally. No attack tuning
or demo preconditioning accompanies this change.

This differs from the archived model's cold FRIC2 seed. It is a provisional
startup choice based on listening evidence, not a verified reset rule for
the SC-02 prototype or production SSI. The real Phasor recording is still
needed to check the full attack. The source regression replays all four H
intervals and checks their active noise routes, including the unknown first
U20 state and the established route in the later three.

```powershell
python scripts/render_ssi263.py --demo hello_four --engine prototype --ff 231 --output build/ssi263_host/hello_four
```

The four-phrase WAV is `build/ssi263_host/hello_four/prototype.wav`; state and
route-only diagnostic results are in `build/ssi263_host/attack_trace/`.

The user reports that the native mb-audit phrases are already close to a
physical SSI, with attacks still a little aggressive. This is the listening
checkpoint for the next attack-timing audit and capture-comparison work.
Keep its source, filter, gain and timing settings reproducible; listening
memory alone does not establish which part of the remaining attack differs.

## Explicit assumptions and prototype conflicts

- **ART clock conflict:** the prototype couples articulation to live RATE.
  The production datasheet says they are independent. The host model freezes
  that factor at reference RATE 8. `--articulation-reference-rate 0..15`
  makes the assumption explicit. At ART 5 and PAL XCK this gives about
  96.8 ms for sixteen transition opportunities. This is not a measured SSI
  transition time.
- **Duration conflict:** phone writes and CTL release restart a full duration
  interval. The recovered prototype has free-running CTL counter behavior.
  The host keeps the current engine's compatibility policy pending capture.
- **CTL/source conflict:** CTL high clears pending source synchronization and
  forces both excitation drives to zero. The archive treats CTL and external
  PD/RST separately and retains source charge. This hard mute is a host
  policy, not a verified prototype or production-chip rule. Filter charge
  remains stored and can still produce a decay after the source stops.
- **Amplitude timing approximation:** selector 4 gets one transition
  opportunity per duration phase. This does not yet reproduce the full
  prototype U166B permit logic. The prototype profile does apply U68/U206
  amplitude masking; the `transitions` bridge still mutes host AMP=0 at once.
- **Pitch glide approximation:** transitioned-I timing is inherited from the
  current engine, not verified against prototype gates or production SSI.
- **Analog assumptions:** the prototype equations use ideal charge transfer.
  Op-amp response, physical tolerances, potentiometer positions, Phasor mixing
  and reconstruction/anti-alias filters are not modeled. High FF output can
  therefore alias in the WAV.
- **Startup assumptions:** cold counter/noise seeds are deterministic, not
  measured. While its held route is unknown, the source now uses FRIC1 in
  place of the archived model's FRIC2 seed to avoid an incorrect first-H
  hiss. This choice has no proven prototype or production reset rule; the
  control model retains its unknown flags and gate behavior.

The prototype defaults to fixed output gain 8 and voice drive 2048 in Q16.
The archived branch used output gain 32, which clips parts of these renders.
Gain 8 leaves headroom in the full calibration trace; it is not a fit to
physical output level. Use `--prototype-gain` and `--voice-trim` for explicit
experiments. No render normalizes its output or applies per-phone gain/EQ.
Reports include rail counts, internal saturation counts and the settings.

The `baseline` and `transitions` paths retain current coefficient mappings,
the old filter-frequency approximation and output shaping. These differ
from the prototype circuit by design and remain comparison models.

## Replay the tester's complete stimulus

```powershell
python scripts/render_ssi263.py --calibration build/ssi263_calibration/SSI263-CAL-PAL-01.zip --engine all --output build/ssi263_host/calibration
```

The loader checks the package and all 1,752 ordered writes. It replays the
1,654 SSI writes and accounts for the 98 AY writes in metadata. WAVs contain
raw SSI model output; they omit the AY markers and analog card mix.
The stream lasts 119.997 seconds at PAL effective XCK of 1,015,625 Hz.

Measured on this workstation, the full stream renders in roughly 0.7 seconds
for baseline/pitch, 1.6 seconds for transitions and 7.4 seconds for prototype.
Compilation, table checks and WAV/report writing add some overhead.

Use `--start-seconds 20 --end-seconds 25` to hear a section. The program
replays all prior state before trimming, so the samples match that section
of a full render. It does not start from a guessed register snapshot.

The tester WAV can later be checked with `scripts/analyze_ssi263_capture.py`.
This host tool does not fit its models automatically to that recording.

## Supply a register trace

```json
{
  "effective_clock_hz": 1015625,
  "duration_ticks": 1015625,
  "events": [
    {"tick": -120, "socket": 0, "register": 3, "value": 128},
    {"tick": -100, "socket": 0, "register": 0, "value": 192},
    {"tick": -80, "socket": 0, "register": 1, "value": 96},
    {"tick": -60, "socket": 0, "register": 2, "value": 128},
    {"tick": -40, "socket": 0, "register": 4, "value": 128},
    {"tick": -20, "socket": 0, "register": 3, "value": 95},
    {"tick": 0, "socket": 0, "register": 0, "value": 194}
  ]
}
```

```powershell
python scripts/render_ssi263.py --trace my_trace.json --engine all
```

Events must stay ordered; equal timestamps retain input order. Register
aliases 4..7 address FF. A trace may include up to one second of negative
preroll. Baseline audio applies writes at the first 48 kHz sample at or after
the event; native state uses XCK event times. Bus synchronizers, D7/IRQ and
fabric pipeline delays are outside this listening model.

For an already-built executable, the generated `tables.txt` and `events.txt`
are enough. No Python or compiler is needed for this command:

```powershell
build/ssi263_host/ssi263_host.exe build/ssi263_host/listen/tables.txt build/ssi263_host/listen/events.txt example.wav prototype 8 8 2048
```

The three numbers are articulation reference RATE, prototype output gain,
and voice trim. The executable writes WAV directly for a `.wav` output name;
other names receive raw signed little-endian stereo PCM.

## Source and checks

The renderer reads existing committed tables and the native SSI ROM. It
never calls or modifies the old SC-01 table generator. The prototype circuit
port comes from `origin/codex/dual-ssi263-sc02` commit
`502ae04f68f04e23ce04abaaf0d44990c71e763a`, including its archived schematic
and `ssi263_sc02_audio.sv`. See [native work notes](SSI263_NATIVE_IMPLEMENTATION.md)
for the archive path and schematic hash.

```powershell
python scripts/test_ssi263_host_data.py
python scripts/test_ssi263_host_baseline.py
python scripts/test_ssi263_host_control.py
python scripts/test_ssi263_host_source.py
python scripts/test_ssi263_host_tract.py
python scripts/test_ssi263_host_filter_clock.py
python scripts/test_ssi263_host_audio_precision.py
python scripts/test_ssi263_host_listening.py
python scripts/test_ssi263_host_mb_audit.py
python scripts/test_ssi263_host_render.py --package build/ssi263_calibration/SSI263-CAL-PAL-01.zip
```

These tests use no Vivado. The baseline test matches all 5,760 stereo frames
in a saved RTL fixture at that fixture's sample positions, including its
one-sample output pipeline. This establishes the tested arithmetic port,
not universal cycle equivalence. The other checks cover DDA ramps, interrupted
phones, held parameters, source counters/noise, charge conservation, direct
WAV output, deterministic replay, stereo independence and exact range trims.
They do not establish a match to physical SSI hardware.
