# Native SSI-263 work

The production path and current checks are in [the native engine guide](SSI263_NATIVE.md).
The control-only modules and tests listed below are earlier prototype work;
Vivado no longer includes them in `hdl/hdl_sources.txt`.

Started 2026-10-06 on `codex/ssi263-physical-match-handoff`.
Firmware stays `F1.2.5-d1` until the user approves `F1.2.5`.

The listening checkpoint now has a complete FPGA audio implementation:
scanner, transitions, source, five formants and a per-socket scheduler. The
production wrapper selects it for both sockets, with SSI volume and pan. See
[FPGA candidate notes](SSI263_FPGA_CANDIDATE.md) for exact host/RTL replay,
arithmetic, clock limits and the production bus integration.

## First implementation step

The new work targets SSI-263 behavior. It does not regenerate the old SC-01
package or treat that model as the desired SSI sound.

The following pieces are now available:

- `hdl/apple/ssi263_clock_core.sv`: an independent SSI divider core.
- `hdl/apple/ssi263_source_control.sv`: native TPARM latches, source terms
  and the two held fricative-route switches.
- `hdl/apple/ssi263_control_core.sv` and `ssi263_parameter_rom.sv`: a separate
  SSI control path with its own raw phone, ROM, source state and exact clocks.
- `scripts/sim_ssi263_calibration.py`: both SSI register interfaces driven by
  the checked CPU-write trace from the physical calibration disk.
- `scripts/analyze_ssi263_capture.py`: checks and measurements for the returned
  stereo WAV, using the same disk manifest.
- `scripts/render_ssi263.py` and `scripts/ssi263_host/`: fast host listening
  models, including the baseline, scanner/transitions and an optional native
  source plus prototype filter candidate. See [host renderer notes](SSI263_HOST_RENDERER.md)
  for commands, model assumptions and known prototype conflicts.

These earlier control-only modules established the ROM and timing rules.
The later `ssi263_native_engine` now supplies production audio. Historical
baseline replays still measure the old engine; they do not validate the new
path. The native engine has separate replay and integration checks. Physical
audio agreement still awaits the tester's recording.

## Recovered prototype source

The complete prototype archive is in local Git history on
`origin/codex/dual-ssi263-sc02`, at commit
`502ae04f68f04e23ce04abaaf0d44990c71e763a`. It is absent from this branch's
checkout, not missing from the repository. The earlier search missed that
ref.

The source is `schematics/sc02_prototype/sc-02_Final_Schematic_V1.00.pdf`:
seven sheets, 5,080,426 bytes, SHA-256
`d0e05ea01fc5e823571e140fce5ce9f12a48f1993484d683f075151382adba35`.
The same archive contains extracted pin/net JSON, CSV, source SVGs and KiCad
files. Its README records the extraction limits and unresolved differences
between the discrete prototype and production chip.

That ref also contains `ssi263_sc02_core.sv`, `ssi263_sc02_audio.sv`, reference
tests and `docs/SSI263_SC02_IMPLEMENTATION_REPORT.md`. They cover the selector,
transition, source, envelope, noise and switched-capacitor model. Their prior
implementation and reported checks need review before reuse; finding them
does not establish a production-chip sound match.

The user reports that the current hybrid sounds extremely close to the real
SSI. Keep it as the listening and measurement baseline. Existing arithmetic,
waveform and filter code can support an incremental replacement, with sound
model assumptions stated explicitly. Similar sound does not establish equal
internal circuits. Flag retained or proposed departures from the prototype
before implementing them; use the recovered source for details it resolves.

## Clock contract and explicit limits

The [SSI-263A datasheet](https://downloads.reactivemicro.com/Electronics/Speech/SSI-263A%20Data%20Sheet%20v2.pdf),
pages 2 and 4, defines the steady-state laws and DIV2 polarity:

```text
effective_XCK = raw_XCK / (DIV2 ? 2 : 1)
pitch_hz     = effective_XCK / (8 * (4096 - I))
filter_hz    = effective_XCK / (2 * (256 - FF))
```

The new core counts effective XCK enables in the fabric clock domain. It does
not derive pitch or filter timing from the 48 kHz output cadence. Its full
period ranges are 8..32768 effective edges for pitch and 2..512 for the filter.
I4095 and FF255 remain valid. The outputs mark complete cycles; they do not
claim to reproduce the prototype's internal Phi0/Phi1 edges.

Run and reload inputs are separate for the two dividers. A live target affects
the next cycle reload. Explicit reload wins over a coincident XCK enable.
These are caller policies, not findings about silicon register-write phase.
The caller must preload the dividers before running them. CTL, warm-reset,
power-up phase, and transitioned-inflection behavior remain outside this
module. No known SC-02 prototype fact conflicts with the steady-state laws.

Do not wire a new pitch-cycle pulse to phone-start reset without evidence.
The existing phone-start path preserves pitch phase. Also, the existing
`pitch` value indexes a pulse waveform and triggers filter commits; it cannot
be replaced with a raw XCK count while keeping those consumers unchanged.

## Native source controls, routes and separation

The source-control block now implements the recorded prototype equations:

- PW0/PW1 set only on their selector write at duration phase 2 or 6, chosen
  by that selector's TPARM0. A phone write clears only these two latches.
- Selector 2 loads PW2/PW5 with opposite polarities. PW3 loads only when
  PW1 permits it, from held CTRL or inverted TPARM1.
- U104C, AMPCT0, FRICATIVE and U32B follow the recorded Boolean terms.
- The U20 route gate uses old PW2 or current TPARM2, plus the documented
  amplitude and PW conditions. U112 passes U20 throughout open Phi1;
  U166 samples its complement on the separate Phi0 edge. The routes can
  briefly both be on or both be off. No live-complement shortcut is used.

HF and HFC now produce different PW3/source-control terms in native-control
tests with the same lead-in and CTRL inputs. This verifies their documented
control distinction; it is not yet an audible HF/HFC result in firmware.

The separate controller reads all 512 canonical ROM bytes directly through
`ssi263_parameter_rom`, without importing the old table package, translating
phone numbers, or converting native target codes. Selector 4 exposes host
amplitude as its target; selector 7 has no parameter target. It owns its phone
and source latches and instantiates the exact SSI dividers. There is no
output-sample clock or inherited pulse-table counter in this control path.

Documented transition permits are exposed for F1, F2 and shared F3/F4 through
U32B, and for voice/fricative amplitude through PW0/PW1 and supplied rate edges.
These are combinational qualifications of the supplied selector edge, not
registered completion events. F2Q and host-amplitude transition timing remains
unspecified: their target values are available, but the controller marks their
transition permission unknown and does not issue a write.

Each held bit has a separate known flag. Cold FPGA values are bounded seeds,
not asserted physical reset states. Only events or controlling Boolean values
establish knowledge. Before the first phone write the target is invalid and
U32B cannot claim knowledge from a seeded TPHO5. Unknown transition permission
never produces a write. Phone writes retain PW2/PW3/PW5/U20 and both routes.
Neither block equates CTL or an Apple warm reset with a cold-state clear.

All event inputs must already belong to the fabric clock domain. The caller
must keep each Phi1 level across a fabric edge. Phone and selector writes must
be serialized, as must Phi0 edges and selector-2 writes that could change U20;
the recorded equations do not settle their simultaneous physical order.
Simulation rejects those ambiguous event combinations. These exclusions are
interface policies, not additional claims about the prototype.

For this earlier control-only module, the caller must supply native selector
events, U37 phase, latched CTRL, U62 /Q, D3+4, U68 zero and current native
amplitude-zero flags. Latched CTRL is not a guessed decode of a host CTL write.
Neither the old pulse counter nor its mapped amplitude state is a valid
replacement for these signals. The later `ssi263_native_engine` now implements
the scanner, U68 envelope, CD4006 noise and analog tract together. It uses the
frozen host event order and explicitly documented SSI policies. It now runs
behind the production wrapper; see [the candidate](SSI263_FPGA_CANDIDATE.md).

## Departures removed from the old audio path

The replaced SC-01-based SSI path had these departures or missing paths:

- A nine-value SC-01 glottal waveform and SC-01 noise recurrence instead of
  the native source/envelope and U75/CD4006 noise logic.
- A heuristic half-period noise gate.
- Closure and delay traits inherited from mapped SC-01 phones.
- SC-01-derived filter coefficients, target conversion and transition cadence.
- Missing low-nibble TPARM controls, including the HF/HFC PW3 distinction.
- Missing separate held FRIC1 and FRIC2 routes.

The native engine implements the TPARM controls, held routes, source,
sequencer and filters. Its remaining prototype conflicts are listed in the
[FPGA notes](SSI263_FPGA_CANDIDATE.md), including retained timing/reset policies
and unmeasured analog assumptions. The recorded
prototype route is voice -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2).
Selector 3 supplies the shared F3/F4 target. Route switches hold state on
different phases; they are not a live bit and its complement. See
[the ROM/control evidence](SSI263_SC02_ROM_FORMAT.md).

Flag any proposed conflict with that evidence before implementing it. Do not
infer production source shape, noise level or hidden latch phase from a
better-sounding result alone.

## Native filter direction

The standalone candidate implements distinct F1..F5 charge states and the
documented held noise routes, driven by effective XCK and FF. It ports the
checked host model's circuit and capacitor ratios. It does not insert native
capacitor banks into SC-01 equations.

The recorded switched banks alone do not define the full transfer function.
The recovered schematic and earlier model supply the fixed capacitors,
topology, phase ordering and output circuit used by the candidate. Acoustic
measurements and differences between the prototype and production chip remain
separate checks.

At the assumed NTSC effective clock of 1,020,484 Hz, FF255 gives 510,242
complete filter cycles per second per chip. At 133 MHz:

| Processing arrangement | Average fabric cycles per job |
| --- | ---: |
| One engine per chip, one job per complete filter cycle | 260.7 |
| One engine shared by both chips, one job per complete cycle | 130.3 |
| One engine per chip, separate jobs for both half-phases | 130.3 |
| One shared engine, separate jobs for both half-phases | 65.2 |

The candidate uses one engine per socket and separate half-phase events.
Its measured worst complete update takes 121 fabric clocks, within the
130-clock minimum effective-XCK interval. The old backend's 48 kHz coefficient
recurrence does not set this budget. High FF also
needs anti-alias filtering before 48 kHz output.

## Replaying the physical disk

The replay reads `SSI263-CAL-PAL-01.zip` without extracting or executing it.
It checks hashes, every manifest/register/trace tuple and strict write order.
It preserves the pre-timer writes, within-tick CPU offsets and all preceding
state when a user requests a shorter WAV range. Both SSI263AP bus wrappers
receive their own writes. Raw Q3 passes through the production synchronizer;
each voice applies DIV2. D7 and IRQ changes are logged.

```powershell
python scripts/sim_ssi263_calibration.py --prepare-only
python scripts/test_ssi263_calibration_replay.py --rtl --package build/ssi263_calibration/SSI263-CAL-PAL-01.zip
```

`--start-seconds` trims recorded output only. It does not jump over earlier
events or load a guessed state snapshot. With no end time the tool simulates
the complete run, about 15.96 billion fabric cycles at 133 MHz. Short smoke
runs validate the harness; a complete audio render has not been run yet.
The observed smoke-test rate projects roughly 15 hours for the complete run;
host load and the active audio workload can change that estimate.

The generated `ssi_raw.wav` contains only the SSI engine outputs. AY markers,
the card mixer and the analog path are absent. `replay.json` records source
hashes, sample alignment, socket order and limits. CPU timestamps are nominal
emulated instruction-end offsets, not measured physical pin edges. The bench
uses logical cycle ratios; it is not a routed timing simulation.

## Receiving the WAV

```powershell
python scripts/analyze_ssi263_capture.py tester.wav --manifest build/ssi263_calibration/SSI263-CAL-PAL-01.zip --out build/ssi263_capture/report.json
```

NumPy is required. The reader accepts stereo PCM 8/16/24/32-bit, float 32/64-bit,
and the corresponding extensible WAV formats. It preserves input scale and
channel order. Five AY marker starts establish one offset and time scale for
the entire recording. Missing or ambiguous bookends require analyst input;
the tool does not choose a convenient match. An inspected capture can use
`--offset-seconds` and `--time-scale` explicitly.

The report includes digital rail counts, completeness, quiet levels, candidate
socket assignment, repeated-reference levels, and per-window levels and band
powers. Socket assignment uses AC level above the initial quiet interval,
not DC or a silence ratio. Reported time scale combines host and recorder
timing; it is not a direct chip-clock measurement. The conservative marker
edge uncertainty is 25 ms, enough to overlap adjacent 40 ms FF sweep steps.

All fitting windows stay marked unverified. Inspect settling and clipping
before fitting. The short FF sweep remains a gross-behavior test. There is no
per-phone gain, EQ or alignment adjustment, and no automatic analog fit yet.

## Validation

```powershell
python scripts/test_ssi263_clock_core.py --synth
python scripts/test_ssi263_control_reference.py
python scripts/test_ssi263_source_control.py --synth
python scripts/test_ssi263_control_core.py --synth
python scripts/test_ssi263_calibration_replay.py --rtl --package build/ssi263_calibration/SSI263-CAL-PAL-01.zip
python scripts/test_ssi263_capture_analysis.py
```

The divider simulation passed 4,621 cases: all 4,096 immediate words, all 256
FF values in both DIV2 modes, pitch endpoint/neighbor cases with DIV2,
PAL/NTSC event cadence, missing XCK edges, and independent run/reload behavior.
Standalone Zynq-7020 synthesis used 50 LUTs, 28 registers and 4 CARRY4 blocks,
with no DSP or BRAM. This is not whole-design timing closure.

The independent event reference passes 15 tests. Source-control RTL passes
6,724 reference vectors, an extra pre-edge Phi1 transparency check and three
invalid-event checks. It synthesizes to 35 LUTs and 16 registers, with no DSP,
RAM or inferred latches.

The combined native controller passes 5,354 checks over all 512 ROM bytes and
two independent sockets, plus two interface rejection checks. Coverage
includes HF/HFC, retained state, AH/S/AH route changes through both-off and
both-on intermediate states, native transition permits, unknown startup and
uninterrupted dividers across phone writes. Standalone synthesis uses 147
LUTs, 51 registers and no DSP or block RAM. These figures include the native
ROM, divider and source-control blocks. Its unconstrained ports and estimated
clock skew do not establish timing closure in the full design.

Replay validation passed seven Python tests and checked all 1,752 packaged
writes (1,654 SSI and 98 AY) across 422 segments. The real disk's first 20 ms
replayed 18 writes and emitted 960 stereo frames. A separate 120 ms RTL test
checked 20 writes, register aliases, distinct nonzero socket outputs, a D7/IRQ
response and all twelve status snapshots. Its 4,800 trimmed frames exactly
matched the corresponding part of its 5,760-frame full render.

Eight WAV tests pass using independent synthetic tones, both channel orders, reversed
polarity, clock drift, duplicate runs, missing endings, clipping, silence,
DC offsets, noise, and integer/float file formats. These check the analysis
tool; they do not establish a physical-chip match. A complete synthetic
48 kHz/24-bit WAV also passed the command-line path with the shipped manifest,
producing measurements for its 416 candidate windows.
