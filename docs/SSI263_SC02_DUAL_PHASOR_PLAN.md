# Dual SSI-263 / SC-02 Phasor implementation plan

## Goal

Replace the SC-01-derived speech code with two independent SSI-263AP / SC-02
cores.  Model the SSI-263 register, request, timing, ROM, transition, excitation,
and filter behavior from the manufacturer data and the supplied SC-02
reconstruction.  Keep the Phasor bus behavior that real-card tests prove.

The final virtual card represents a fully populated Phasor:

- channel B / primary speech socket: SSI-263AP;
- channel A / second speech socket: SSI-263AP;
- no SC-01 or Votrax speech engine;
- separate state, timing, A/R request, and audio for each chip;
- speech routed to the matching Phasor output channel.

## Source material and confidence

Use these sources for different parts of the design:

1. The SSI-263A data sheet and programming guide define the package, register
   interface, public timing laws, and control meanings.
2. `mb-audit` and AppleWin define Phasor-visible address, request, reset, and
   mode behavior that tests on an original card have checked.
3. `sc-02_Final_Schematic_V1.00.pdf` and `ssi263a.bin` define the native ROM,
   selector engine, digital circuit, excitation path, and switched-capacitor
   circuit.
4. Real SSI-263 and Phasor captures must settle clock wiring, P/AP reset detail,
   analog level, and part tolerance.

The SC-02 PDF is a 2020 discrete reconstruction.  It calls out changes made for
an external clock, replacement state RAMs, reset fixes, board cuts, and output
amplifiers.  Keep those board changes out of the chip core unless a real-chip
test proves the same behavior.

### Supplied ROM

- source: `C:\Users\hasse\OneDrive\Documents\Apple2\Appletini\Mockingboard\SC-02\ssi263a.bin`
- size: 2,048 bytes;
- CRC32: `CC0A72EE`;
- SHA-256: `9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941`;
- active range: offsets `000-1FF`;
- unused range: offsets `200-7FF`, all zero.

The exact lookup is:

```text
rom_address = ((phoneme & 0x3f) << 3) | (selector & 7)
```

The upper nibble supplies a target.  The lower nibble controls transition,
delay, closure, and timing logic.  The selector targets are:

| Selector | Target |
|---:|---|
| 0 | F1 frequency |
| 1 | F2 frequency |
| 2 | F2 resonance |
| 3 | F3 and F4 frequency |
| 4 | filter amplitude, with the host amplitude used as the target |
| 5 | voiced amplitude |
| 6 | fricative amplitude |
| 7 | idle selector slot; no parameter latch on sheet 7 |

## Clock decision

The Phasor native mode runs its AY chips at twice the Mockingboard AY rate.  That
does not, by itself, prove the SSI-263 XCK pin rate.

The new core will model both the external XCK and the SSI DIV2 input.  It will
not fold them into a guessed 20 kHz update clock.

Initial card configuration:

- use a fabric-clock rational accumulator to create evenly spaced XCK edge
  enables at 3,579,545 / 2 Hz;
- set SSI DIV2 high, which gives an effective 894,886.25 Hz time base;
- keep a raw two-times-Apple-bus XCK profile with DIV2 high for direct Phasor
  comparison;
- keep a 1 MHz effective profile for the data-sheet nominal case;
- keep a 3,579,545 / 4 Hz raw profile with DIV2 high, which gives the supplied
  reconstruction's literal 447,443.125 Hz internal FASTCLK.

The default follows the data sheet's suggested colorburst-derived source and
its desired 800 to 1000 kHz effective time base.  A raw clock near twice the
Apple bus rate with DIV2 high remains plausible and gives a similar effective
rate, but no physical Phasor XCK or DIV2 trace proves it yet.  The reconstruction
divides its crystal down one stage further and conflicts with the published
pitch and time-base range, so it remains a comparison profile instead of the
firmware default.  Do not change SSI XCK when Phasor mode changes; the proved
native-mode clock doubling applies to the AY chips.

The generated Zynq fabric clock resolves to 133,333,344 Hz.  The XCK accumulator
uses that generated value rather than the rounded 133 MHz label used in
comments elsewhere in the design.

The core must use these laws:

```text
pitch_hz      = XCK_effective / (8 * (4096 - I))
filter_hz     = XCK_effective / (2 * (256 - FF))
frame_ticks   = 4096 * (16 - R)
phoneme_ticks = frame_ticks * (4 - D)
```

## Target source layout

```text
hdl/apple/ssi263_sc02_pkg.sv
    Native ROM and common fixed-point helpers.

hdl/apple/ssi263_sc02_core.sv
    Registers, write-end capture, XCK/DIV2, selector state, transition RAM,
    duration, rate, inflection, repeat, power-down, A/R, and D7.

hdl/apple/ssi263_sc02_audio.sv
    Voice and fricative sources, closure control, switched-capacitor formants,
    phase clock, output state, and 48 kHz sample conversion.

hdl/apple/ssi263_voice.sv
    One SSI-263 chip wrapper.  No SC-01 parameters or inputs.

hdl/apple/mockingboard.sv
    Two SSI-263AP instances, A5/A6 socket select, VIA/native IRQ routing,
    Phasor modes, and channel A/B audio routing.

scripts/ssi263_sc02_reference.py
    Cycle reference for ROM, register, timer, selector, transition, request,
    excitation, and filter-control traces.

hdl/sim/tb_ssi263_sc02_*.sv
    Focused RTL benches driven from the reference vectors.
```

## Chip interface contract

Each core must implement:

- register 0: `DR1 DR0 P5 P4 P3 P2 P1 P0`;
- register 1: `I10 I9 I8 I7 I6 I5 I4 I3`;
- register 2: `R3 R2 R1 R0 I11 I2 I1 I0`;
- register 3: `CTL T2 T1 T0 A3 A2 A1 A0`;
- registers 4-7: `F7 F6 F5 F4 F3 F2 F1 F0`;
- data and address capture at the end of the active write condition;
- D7-only readback for every selected read address;
- high-impedance D0-D6 at the chip boundary;
- active-low, open-collector A/R request;
- `PD/RST` power-down without register loss;
- separate SSI-263P and SSI-263AP card-reset behavior where required.

Acknowledgment rules:

- writes to registers 0, 1, and 2 clear A/R;
- register 3 clears A/R only when CTL is one;
- registers 4-7 do not clear A/R;
- reads never clear A/R;
- DR=00 disables the external A/R drive but retains the prior timing mode;
- completion still sets the internal D7 status while DR=00 blocks the external
  interrupt request;
- a phone repeats while A/R remains pending;
- after an acknowledgment, the next repeat boundary asserts A/R again.

Event priority must have tests for collisions.  Use this order:

1. FPGA/card removal reset;
2. chip `PD/RST` assertion;
3. active write ending;
4. phone start or CTL wake;
5. timer boundary and A/R assertion;
6. background selector, transition, pitch, and filter work.

A write that acknowledges or powers down on the same fabric edge as a timer
boundary must win.  A later statement in an `always_ff` block must not restore
the request.

## Native transition engine

The implementation will follow the circuit, not the old interpolation rule:

- an eight-state selector addresses `ROM[(phone << 3) | selector]`;
- each state holds for the circuit-defined slow-clock phases;
- the upper ROM nibble or host amplitude is the target;
- state memories hold the active four-bit values;
- add, subtract, carry, equality, and write gates move one step at the selected
  pulse class;
- the lower ROM nibble selects pulse, closure, and timing actions;
- phone, amplitude, and transitioned inflection are targets;
- rate, filter clock, duration, articulation, and immediate inflection act at
  once;
- filter and excitation state persists across phone writes.

First build a gate-level software trace from sheets 3-7.  Do not replace an
unread gate path with a guessed curve.  Mark any unproved net and cover it with
two candidate vectors until hardware chooses one.

## Native audio engine

The audio engine will use the SC-02 topology and controls:

- native voice-clock counter from the 12-bit inflection setting;
- the shown glottal phase and voiced gating;
- the HCC4006/CD4030 fricative shift-register feedback;
- separate voiced and fricative amplitude paths;
- closure plus `FRIC1_SW` and `FRIC2_SW` control;
- F1, F2, F2 resonance, F3, and F4 parameter states;
- the eight-bit `256-FF` reload divider;
- non-overlapping `Phi0` and `Phi1` switched-capacitor phases;
- persistent filter state;
- one signed output sample per existing audio tick.

The F0.9.99 hardware correction uses this exact section graph while the full
nodal work remains open:

```text
VOICE -> F1 -> F2 -> (+FRIC1) -> F3 -> F4 -> (+FRIC2) -> F5
```

It also uses the unipolar U60 voice pulse, the inverted HCC D4+5 noise tap,
the PW3/U62 polarity gate, a 48 kHz output hold, and C381-style DC blocking.
`PW2 && !PW3` marks exactly B, D, P, T, and K. Keep those held sources silent
and let a following live phone drive the transitioning tract. Do not add a
guessed stop burst. Treat U68/U85C and the T-to-HF FRIC2 handoff as open until
a corrected net list or chip trace settles them.

Development order for the filter:

1. derive double-precision state equations from each capacitor and switch
   section;
2. compare phase-by-phase results with a direct circuit reference;
3. quantize coefficients and state widths;
4. use shared DSP multipliers without sharing chip state;
5. filter and resample the phase-domain output to 48 kHz;
6. set final level from real-chip captures, not phone-specific boosts.

Do not include the reconstruction board's AD817, LM386, pots, speaker, or line
drivers in the chip model.

## Dual-chip Phasor integration

- A6 selects the primary/channel-B SSI-263.
- A5 selects the secondary/channel-A SSI-263.
- both bits may select both chips for a write;
- native reads expose the selected D7 status while D0-D6 retain the Apple bus;
- a dual A5+A6 native read returns the secondary/A5 D7, matching the current
  AppleWin priority; the RTL must assert its read-drive enable for this case;
- route each A/R level to its matching VIA CA1 in Mockingboard mode;
- route enabled A/R levels to direct Apple IRQ in native mode;
- keep request-pending/D7 state separate from the enabled A/R output level;
- retain a pending request across Phasor mode changes;
- Echo+ hides the SSI bus route but does not erase chip state;
- route channel-A speech left and channel-B speech right, subject to final
  Phasor output trace;
- offer mono speech only as a post-chip mix option if wanted later.

AppleWin comments say A/R may reach both CPU IRQ and VIA CA1 in native mode,
while current `mb-audit`-matched behavior routes it by card mode.  Keep the
mode-routed behavior first.  Add a native-mode VIA-IFR scope test before claiming
that the FPGA matches the physical wire on this point.

## SC-01 removal

After both SSI cores pass their tests, remove:

- `sc01a_digital_core.sv`;
- the SC-01 ROM, decoder, coefficients, and generator;
- SSI-to-SC01 and Votrax-to-SSI maps;
- `HAS_SC01`, SC-01 phone, and Votrax ports;
- VIA ORB/PCR SC-01 start and CB1 completion logic;
- SC-01 audio fixes and phone-specific tuning;
- SC-01 simulations, comparison scripts, help, and credits;
- all source-list and build entries for those files.

Do not retain a hidden SC-01-to-SSI compatibility map.  Software that only
targets a Speech-I / SC-01 interface will no longer produce speech.

## Test plan

### ROM and reference tests

- verify source size and hashes;
- verify all 512 active bytes;
- verify all unused bytes are zero;
- verify every one of 64 phones and eight selectors;
- verify selector 4 host-amplitude substitution;
- verify selector 3 updates both F3 and F4;
- verify selector 7 performs no parameter-latch write;
- verify the known set of nonzero fricative targets.

### Register and request tests

- all register and filter aliases;
- write-end capture;
- D7 at every read alias and D0-D6 pass-through;
- every valid and invalid acknowledgment;
- all four DR modes;
- CTL wake and power-down;
- continuous repeat with A/R held and then acknowledged;
- simultaneous write, completion, reset, and mode-change pairs;
- P and AP reset cases.

### Timing and control tests

- rate 0-F exact frame tick counts;
- duration 0-3 exact phoneme tick counts;
- inflection endpoints and normal speech points;
- one-times and two-times XCK plus DIV2 combinations;
- all eight articulation settings;
- all ROM lower-nibble pulse classes;
- all selector state values and transition traces;
- filter divider values 00, 01, 7F, E9, FE, and FF;
- no filter-history reset on phone boundaries.

### Audio tests

- deterministic voiced source periods;
- exact fricative shift-register sequence;
- closure and start/stop gating;
- impulse tests for each formant setting;
- full filter-frequency sweep;
- phone-to-phone transition continuity;
- all 64 phones at standard settings;
- manufacturer `Hello` and compound-phone vectors;
- dual-chip simultaneous speech and channel isolation;
- spectra, envelope, and level against real SSI-263 captures.

### Card and build tests

- `mb-audit` SSI-263 behavior for both sockets;
- AppleWin-compatible address and mode tests;
- overlapping VIA/SSI false reads and writes;
- both VIA CA1 paths and native direct IRQ;
- no SC-01 source or build dependency;
- HDL compile and focused XSim benches;
- full source regressions;
- fresh Vivado synthesis and implementation;
- routed timing with no unbound constraint warnings;
- one clean full build from the final source commit before firmware packaging;
  keep that route if it passes instead of running a duplicate full build.

## Acceptance gate

The work is complete only when:

- the SC-02 core contains no SC-01 ROM, mapping, timing, coefficient, or phone
  rule;
- both speech sockets use independent SSI-263AP state;
- all native ROM and digital reference traces pass;
- XCK, DIV2, timing, A/R, D7, repeat, and reset tests pass;
- filter frequency changes the full filter clock and FF is not a digital mute;
- filter state persists at phone boundaries;
- channel routing and simultaneous speech pass;
- all SC-01/Votrax firmware code and tests are gone;
- focused and full source regressions pass;
- routed timing passes;
- the F0.9.99 test image passes the source and route gates, with any remaining
  real-chip analog and clock choices stated as hardware follow-up work.

The user's attached Appletini listen test is the F0.9.99 hardware gate. It can
confirm that a fault is fixed, but it cannot prove exact SSI-263 analog match
without a same-vector capture from a real SSI-263AP.

## Commit checkpoints

1. `Document dual SSI-263 implementation plan`
2. `Add native SC-02 ROM and reference model`
3. `Implement SSI-263 digital control core`
4. `Implement SC-02 excitation and filters`
5. `Use two SSI-263 chips in Phasor`
6. `Remove SC-01 speech support`
7. `Complete dual SSI-263 verification`
8. `Correct SSI-263 tract and source response`
9. `Retiming SSI-263 input and bus selects`
10. `Package F0.9.99 SSI-263 test firmware`
