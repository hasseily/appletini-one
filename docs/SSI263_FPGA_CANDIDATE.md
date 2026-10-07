# Native SSI-263 FPGA candidate

This implements the portable C++ model in `scripts/ssi263_host/` in synthesizable
SystemVerilog. The C++ code is the canonical higher-level implementation;
controller-state and sample-by-sample PCM checks keep the RTL in step with it.
Both use the committed SSI parameter ROM, native scanner, native voice/noise
source and all five prototype formants. Neither depends on the old sound
generator or coefficient package. Firmware stays `F1.2.5-d1`.

The 2026-10-07 update integrates the user-approved balanced voice/noise levels
and the measured AMP-zero retention correction. The original balanced audio
and its hashes remain the historical listening reference. Earlier qualification
results below describe their dated source revisions, not the updated firmware.

The production `ssi263_bus_wrapper` now selects the native engine for both
SSI263AP sockets. The old SC-01 sound core and formant backend are no longer
in the firmware source list. The standalone checks below remain historical
evidence for the listening checkpoint; full firmware validation is separate.

## Blocks and clock contract

| Module | Role |
| --- | --- |
| `ssi263_native_controller` | Raw ROM scanner, eight DDA slots, held TPARM state, duration, ART and amplitude clocks, filter phases |
| `ssi263_native_pitch` | Retained listening-model inflection policy, isolated from sound generation |
| `ssi263_native_source` | U68 amplitude count, U62 pitch, glottal load/count, CD4006 noise and phase latches |
| `ssi263_native_tract` | Scheduled charge transfers, five formants, output/reconstruction holds |
| `ssi263_native_engine` | Per-socket sequencing, DIV2, 48 kHz sample requests and deadline faults |
| `ssi263_response_timing` | Retained SSI duration/frame response counters for D7 and interrupts |
| `ssi263_stereo_mixer` | Shared SSI gain and independent pan, with smooth control changes |

Every block runs on the fabric clock. `xck_ce` is a pulse in that domain;
it is not a new clock domain. The top defaults to DIV2 enabled for Phasor Q3.
Set `DIV2=0` only when the caller already supplies effective SSI clock pulses.
The two physical sockets require independent instances and state.

The controller accepts writes on any fabric edge. A write coincident with
an effective XCK precedes that tick. The source captures the settled controller
outputs on the following fabric edge, resolves the gates over eight clocks,
then submits a complete filter event. The tract keeps every source, route and
parameter change, including changes during a held filter phase. An unchanged
event still performs the requested output-switch hold operation.

The current maximum is **121 fabric clocks from effective XCK to tick_done**:
one controller-to-source clock, eight source clocks, one source-to-tract clock
and at most 111 tract clocks. At 133 MHz the fastest NTSC Phasor effective XCK
allows at least 130 clocks. FF=255 changes filter phase on every effective XCK;
it neither mutes nor drops events. There is no event queue to overflow during
valid operation. An input arriving before the previous job completes sets a
sticky `fault`. Physical XCK cannot be paused to cover a missed deadline.

`audio_tick` requests one 48 kHz PCM sample and advances the retained pitch
policy. It includes any XCK accepted on that same edge. If an update is still
running, the top waits for it and returns `audio_valid` with the completed
sample. The caller must consume the sample at `audio_valid`, rather than assume
it arrives at `audio_tick`. A second request while one is pending is a fault.
The maximum wait is less than one effective XCK period at the supported clock
rate. Reset clears the complete candidate, pending requests and sticky faults.

## Arithmetic

Charge/state voltages use signed 24-bit values. Each capacitor bank retains
the separate voltage on each disconnected plate. Differences use 25 bits;
raw capacitance products accumulate before one rounding operation. Division
rounds nearest with ties away from zero, then clamps to the signed 24-bit
range. A finite reciprocal table and correction produce the exact integer
quotient; the engine does not use a general combinational divider.

Six shared multiply lanes and registered divider stages process dependent
charge transfers in the host order. The tract exports a sticky
`state_saturated` diagnostic separately from its scheduling/arithmetic `fault`.
Saturation is defined model behavior, not an event drop.

The default voice drive is trimmed to -16384/0; frication is +/-301.
No second voice scaling occurs in the tract. The default PCM gain is 1 followed
by arithmetic division by 2 and signed 16-bit clipping. These remain listening
settings, not measurements of chip gain or potentiometer position. This is the
approved balanced pair: compared with the first firmware's 2048/8 pair, it
preserves nominal voice gain and reduces frication relative to voice by 18 dB.

The song preview uses a separate 1.25 SSI-to-AY mix multiplier (+1.94 dB)
based on listening feedback. The firmware's SSI volume control provides
-5 through +5 dB in 1 dB steps and defaults to +2 dB (about 1.259 times).
SSI0/SSI1 pan use the AY pan scale, 0 through 15, defaulting to left/right.
The mixer combines volume and pan into four gains. One shared multiplier
applies them outside the native engine schedule. It captures each socket on
`audio_valid`, ramps gains at 48 kHz, and keeps the AY/SSI sum wide until the
final clamp. These card mix controls do not change voice/noise balance or
the engine's checked PCM gain. See the [host renderer notes](SSI263_HOST_RENDERER.md)
for the A/B preview.

The frontend stores `phasor.ssi.volume_db`, `phasor.ssi.pan.0` and
`phasor.ssi.pan.1` in global/profile settings. The existing Phasor audio word
uses signed bits 31:27 for SSI dB; the high pan word uses bits 27:24 for SSI0
and bits 31:28 for SSI1. Lower fields keep their existing meanings.

## Prototype differences kept explicit

This ports the audible candidate; it does not replace its unresolved policies
with the known buggy prototype:

- ART uses a fixed reference RATE of 8, keeping it independent of speech RATE.
  The prototype couples those clocks; the absolute SSI articulation time is
  still unmeasured.
- Phone writes restart duration. CTL release restarts the host timing windows.
  CTL also hard-mutes excitation and clears the glottal-load synchronizer;
  the prototype has a separate PD/RST path.
- Selector 4 retains the host duration permit and omits the prototype U166B
  pending-write inhibit. Its setup and transition paths now require host AMP
  to be nonzero, retaining stored amplitude during AMP=0 as the drawing shows.
  This removes the two former AMP-zero conflicts. See the pin-level and
  physical-capture evidence in [the attack audit](SSI263_ATTACK_AUDIT.md).
- Unknown cold routes use the provisional FRIC1 fallback that removed the
  first-H hiss. Held control state still carries its unknown flag; the fallback
  does not assert a known physical U20 reset value.
- Inflection mode 3 retains the previous approximate glide table and 20 kHz
  cadence. The standalone adapter contains only that register/pitch policy,
  with no imported sound tables or old oscillator. It differs from the
  prototype RATE-coupled glide.
- The tract remains an ideal integer charge model. Analog component tolerances,
  amplifier limits, propagation at coincident edges and output reconstruction
  beyond the modeled hold remain unverified against production SSI silicon.

The AMP-zero change adds no attack shaping or new time constant. The balanced
voice/noise settings are a listening choice checked against the two real chips.

## Remaining work after the 2026-10-07 review

This firmware changes the balanced gains and AMP-zero retention only. The
remaining evidence does not yet specify complete fixes for:

- D7 response handling when a new phone arrives before the preceding response.
  The physical log rejects the current unconditional restart in those cases,
  but does not uniquely choose the replacement counter/latch behavior.
- The late S-to-I transition in the song. Retaining AMP does not change that
  passage; the PW0/duration timing remains a separate question.
- The prototype U166B write inhibit, pitch glide and unknown cold-state policy.
- High-frequency output reconstruction and amplitude-dependent tone. A single
  fixed EQ cannot explain the measured amplitude-dependent change.

The Phasor demo issue was resolved by comparing the supplied HDV with all four
archived DSKs: its speech code is original, but its saved FF was 128 instead of
232. The user requested a corrected HDV. No clock or FF remap belongs in the
engine for that saved-setting difference.

## Reproduce the checks

The simulation commands use Verilator on Linux or Ubuntu WSL on Windows:

```powershell
python scripts/test_ssi263_native_controller.py
python scripts/test_ssi263_native_pitch.py
python scripts/test_ssi263_native_source.py
python scripts/test_ssi263_native_tract.py
python scripts/test_ssi263_native_engine.py
python scripts/test_ssi263_native_integration.py
python scripts/test_ssi263_mixer.py
python scripts/test_phasor_ssi_ui.py
python scripts/test_phasor_demo_filter.py
python scripts/test_smartport_data_phase.py
```

`test_ssi263_native_engine.py` replays both sockets through the complete RTL,
compares each PCM sample to the canonical C++ classes, and compares the finished
PCM bytes to a separate run of the public renderer. It also runs a dense fabric
schedule with Q3 every 65 clocks, writes during active work, sample requests
across pipeline phases and deliberate deadline violations. Idle clocks may be
skipped only in the longer trace replay; every clock executes in the dense test.

The main listening result is
`build/test_ssi263_native_engine/listening/rtl.wav`. Reports record hashes,
sample counts and maximum latency. The stress case exercises all 64 phones,
FF extremes, register aliases, CTL, amplitude changes and both pitch modes.

Use `--quick` for a short listening prefix during development, or `--trace`
with an existing host trace JSON. Add `--synth` to synthesize, place and route
only this engine for `xc7z020clg484-2` at a 7.5 ns fabric period. This writes
reports and a standalone checkpoint under `build/test_ssi263_native_engine`;
it does not build or flash Appletini firmware. Isolated timing does not prove
timing or available resources in the complete two-socket board design.

## Integrated host and RTL checks, 2026-10-07

Before the updated full-board build, the following checks passed:

- Controller: 734,176,077 state checks at ART references 0, 8 and 15. These
  include interrupted amplitude transitions and AMP-zero writes coincident
  with selector-4 setup and update edges.
- Source and tract: 266,194,267 source checks and 3,654,267 tract checks.
- Engine: listening, stress and retained-amplitude traces match the public
  C++ renderer sample for sample. Late AMP-zero and CTL intervals have zero AC;
  the synthetic AMP0-to-1 trace has a 15.904 dB early peak above its late level
  on both sockets. This synthetic trace differs from the physical calibration.
- Bus integration: 41,231,842 SSI263P/AP checks and 196,101,634 response checks.
  The unchanged maximum completion times are 122 clocks for an input event
  and 124 clocks for a sample, within the 130-clock effective XCK spacing.
- Full card: 35,926 checks, 6,736 writes and 6,144 frames; all 4,096 matched
  Mockingboard/Phasor PCM comparisons pass. Mixer and UI regressions also pass.
- The full 119.997-second calibration, raw song vocals and unchanged-AY song
  mix from the integrated C++ code match the reviewed retention candidate's
  WAV hashes exactly, without internal saturation or output clipping.

The current model ID is `balanced-amp-zero-hold-2026-10-07`. Its golden hashes
live in `scripts/fixtures/ssi263_host/amp_zero_hold_reference.json`. The original
`balanced_reference.json` is unchanged. Local test reports and frozen test
sources are retained in `build/ssi263_firmware_balanced_20261007/test_evidence.zip`.

## Qualified updated full-board build, 2026-10-07

Vivado 2025.2 completed fresh synthesis and implementation for
`20261006T230942Z-00dbe551-full`, with no incremental reference. The build
contains the balanced gains and AMP-zero retention described above. It uses
the user's positive-slack test policy and remains a development build.

| Final nominal check | Result |
| --- | --- |
| Setup slack | **+0.015 ns**, no failing endpoints |
| Hold slack | **+0.048 ns**, no failing endpoints |
| Pulse-width slack | **+0.265 ns**, no failing endpoints |
| Total negative slack, unconstrained internal endpoints, route errors and missing constraint objects | Zero |
| Board/CDC/DVI bounds and bus skew | PASS; bus-skew slack +5.903 ns |
| Resources | 38,296 LUTs, 29,108 registers, 30 DSPs, 110 block RAM tiles |

Both temporary clock margins are zero at signoff; the original direction
10 ns and PHI0-release 8 ns limits remain in force. This passes the requested
test-build criterion, not the separate known-good reference promotion policy.

The run artifacts are in `.timing_runs/20261006T230942Z-00dbe551-full/`.
The working-tree build records its base commit and dirty flag, with 489
unchanged source hashes and an archived source snapshot under
`build/ssi263_firmware_balanced_20261007/`. The tests and reference fixtures
have a separate archive there. These identify the actual updated sources;
the base commit alone does not include these changes.

The full Vitis rebuild and firmware packaging then passed. The frontend embeds
the fresh CPU1 image; FSBL and both application ELFs are newer than the hardware
export. The XSA's embedded bitstream matches the qualified bitstream. Source
and test-evidence hashes remain unchanged after generated-asset and PS builds.

The resulting `build/ssi263_firmware_balanced_20261007/FIRMWARE.BIN` is
**F1.2.5-d1**, 4,432,172 bytes, SHA-256
`eafeef2c0bbfdc8b41a49836cf29609e00628ef638008ef9d48691f0a190a9dc`.
Its firmware/recovery manifest, CRC and flash-size checks pass. The same folder's
`build_report.json` binds the source, timing, bitstream, XSA and software hashes.
This image awaits the user's hardware test; the workflow did not flash a card.

## Recorded validation

The final source passed on 2026-10-06. Reports live under
`build/test_ssi263_native_{controller,pitch,source,tract,engine}/` and bind the
tested source files by SHA-256.

| Check | Result |
| --- | --- |
| Scanner/controller | 24,295,972 observations; ART references 0, 8 and 15; all register fields and simultaneous-write phases |
| Pitch policy | 1,182,303 observations; all 4,096 words, modes and glide steps |
| Source | 5,100,723 effective XCK ticks; exhaustive 4,096-state gate-settling bound of three passes |
| Tract | 18,749 events; all 61 charge states checked after each event, including deliberate saturation |
| Complete four-hello replay | 205,440 stereo frames; PCM bytes identical to the separate frozen-renderer run |
| Complete stress replay | 4,939 stereo frames; PCM bytes identical; maximum completion latency 121 clocks |
| Continuous fabric schedule | Two sockets, Q3 every 65 clocks, busy-time writes and sample arrivals; 772 sample checks per run; deliberate overruns flag faults |
| Existing control/ROM regressions | 5,354 control checks and 11 data tests passed |

Vivado 2025.2 placed and routed one complete socket engine for
`xc7z020clg484-2` at **7.5 ns (133.333 MHz)**. Setup slack is **+0.454 ns**,
hold slack **+0.098 ns**, with no failing timing endpoints. The result uses
**3,467 LUTs, 2,112 flip-flops, 12 DSPs and no block RAM or latches**.

The out-of-context reports lack board port delays and the final clock-buffer
location. DRC reports DSP pipeline advice and the expected missing PS7 block;
it reports no errors. These results establish internal standalone timing.
The separate full-board qualification below includes both sockets, the
surrounding design and the real board constraints.

## Production bus integration

The wrapper forwards accepted SSI writes to the native engine, including
filter aliases, and retains the established mode, ACK, D7 and IRQ routing.
One fabric register stages the native engine's complete event stream: writes,
Q3, sample ticks, resets and P mode latching. This breaks the long Apple bus
decode-to-engine path while preserving the order of all sound events. The
response counters and bus status retain their original timing. This adds one
7.5 ns fabric clock of sound-engine latency, not a change to the chip model.
Separate SSI counters retain bus response timing while the native controller
drives sound envelopes. Each consumes the raw Q3 enable and divides it by two;
the dividers are parallel, not cascaded. This preserves the existing bus
contract rather than claiming new evidence for internal prototype timing.

AP warm reset retains DUR/INF/RATE/FF, powers down speech and clears requests.
The wrapper publishes valid zero samples during reset so the mixer cannot
hold a stale sample. Source/filter state resets under this retained board
reset policy; that is not a measured production analog charge-retention rule.
The production cold FF register remains 0, while standalone engine tests
default to FF255 to preserve the frozen host checkpoint's reset settings.

The test firmware remains F1.2.5-d1. It uses the build script's
`APPLETINI_POSITIVE_SLACK_ONLY=1` policy at the user's request: setup slack
must be strictly positive; hold, pulse-width, routing and constraint checks
still apply. Audio comparison against the tester's recording remains pending.

Production integration checks passed before the full build:

- SSI263P/AP bus and reset regression: 41,231,842 checks, including 1,540
  exact host PCM comparisons, live RATE, IRQ routing and write/completion races.
  Staged-input tests also cover changing simultaneous write/Q3/sample events,
  consecutive writes and a one-clock AP reset with a stale nonzero sample.
  Native ticks finish within 122 clocks of the original input event and
  wrapper samples within 124, below the 130-clock effective XCK interval.
- Response counters: all DUR/RATE/function combinations, repeated requests,
  live reloads and warm-reset cancellation passed 196,101,634 checks.
- Full card: 6,708 Apple bus writes, 27,705 checks and 6,144 audio frames
  passed at a 130-clock effective XCK interval without engine faults or lost
  sample-valid pulses. FF aliases and socket selection were covered in both
  Mockingboard and Phasor modes, including rejected Echo+ accesses.
  The input-stage version produces byte-identical full-card PCM to the first
  version. Its source hashes and reports are retained under
  `build/ssi263_firmware/input_stage_trial/build/`.
- Mixer: 139,013 checks cover gain, pan, live ramps, signed sums and clipping.
- UI: 5,632 combined settings and 66 compact focus/layout cases passed;
  the rendered 640x400 and 1920x1080 pages were visually checked.

The CTL-high write now wins over a coincident completion response. This fixes
an existing wrapper race that could reassert D7/IRQ immediately after clearing
them; it does not alter the native sound model.

## Qualified full-board PL build

Vivado 2025.2 completed a fresh full build on 2026-10-06, identified by
`20261006T200017Z-41ac9896-full`. It synthesized and routed the complete
`xc7z020clg484-2` design, including both native SSI engines and the volume/pan
mixer. It reused no synthesis or incremental placement checkpoint.

| Final nominal check | Result |
| --- | --- |
| Setup slack (WNS) | **+0.170 ns**; zero failing endpoints |
| Hold slack (WHS) | **+0.048 ns**; zero failing endpoints |
| Pulse-width slack (WPWS) | **+0.265 ns**; zero failing endpoints |
| Total negative setup, hold and pulse-width slack | Zero |
| Unconstrained internal endpoints, route errors and missing constraint objects | Zero |
| Board/CDC/DVI constraint bounds and bus skew | PASS; bus-skew slack +5.950 ns |
| Resources | **38,300 LUTs, 29,093 flip-flops, 30 DSPs, 110 block RAM tiles** |

The final checks use the original nominal clock definitions, with zero extra
setup uncertainty on the fabric and pixel clocks. The 5 ns raw-input bound,
10 ns direction-output bounds and 8 ns PHI0-release bound remain in force.
No timing requirement was relaxed to obtain this result.

The build includes one small SmartPort data-path change in `apple_top.sv`:
SmartPort and overlay consumers use the existing captured data-phase byte,
avoiding the live virtual-bus reply mux. They consume that byte only when
`data_en` is asserted. The consumed bytes and their timing remain identical;
unused byte values between consumption strobes can differ. Physical-bus data,
strobes, resets and slot gates remain unchanged. This adds no clock latency
and changes no SSI model, ROM or prototype policy.

The differential regression passed **1,272,185 checks**, covering 2,356 data
phases, physical and virtual accesses, reset overlap, gated writes and changes
after capture. It compares the frozen and current top-level routing with the
real virtual bus, slot-7 guard, SmartPort and overlay modules. The existing
virtual-bus, linear-text-overlay, SmartPort reset, SmartPort shortcut and slot-7
DEVSEL guard regressions also passed. Functional evidence and hashes are in
`build/ssi263_firmware/smartport_data_trial/validation.json`; that file records
the earlier functional-only stage, while the full-build manifest records the
later PL qualification.

The qualified run directory is
`build/ssi263_firmware/smartport_data_trial/.timing_runs/20261006T200017Z-41ac9896-full/`.
It retains `manifest.txt`, timing/route/bus-skew reports, `candidate.dcp`,
`appletini_yarz_top.bit` and `appletini_yarz_top.xsa`. The adopted 328-source
snapshot and XSA lineage are recorded in `build/ssi263_firmware/sources.json`
and `build/ssi263_firmware/adoption.json`. The approved XSA was copied to
`project/appletini_yarz_top.xsa` for the PS build.

The full Vitis rebuild completed on 2026-10-06, including the platform,
FSBL, bootloader, CPU1 image and frontend. The frontend contains the fresh
CPU1 image. All 328 frozen source hashes still match after asset generation.

`build/ssi263_firmware/FIRMWARE.BIN` is the verified **F1.2.5-d1** image:
4,432,748 bytes, SHA-256
`96fe081406d183d4bfa064949b7fb7f0967e744b98a022859194835d4082d01e`.
It contains the qualified bitstream and fresh software, passes the firmware
role/recovery-capable manifest and size checks, and retains the development
version. `build/ssi263_firmware/build_report.json` records the artifact hashes
and checks; the Vitis and packaging logs are in the same directory.

The default SSI volume is +2 dB, adjustable from -5 to +5 dB; SSI0 defaults
left and SSI1 right, with separate pan controls. The build workflow did not
flash hardware. The user subsequently tested the image and reported correct
mb-audit speech, but low-sounding Phasor.hdv demo speech until its Pitch/FF
setting was raised to 230, with remaining differences. That control used the
legacy filter mapping in the previous firmware. Menu-control coverage is not
yet reported. PL timing success does not establish measured SSI sound accuracy.
