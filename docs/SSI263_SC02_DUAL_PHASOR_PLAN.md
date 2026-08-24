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

Final card configuration:

- synchronize the physical Apple Q3 pin into the fabric domain and create one
  XCK enable for each observed rising edge;
- keep SSI DIV2 high, which gives an effective `1,022,727.14 Hz` time base;
- keep the effective clock constant when Phasor mode changes.

This choice follows the reconstruction rather than changing it. Its local
crystal path divides `3.579545 MHz` by four to `894,886.25 Hz` FASTCLK. P4
also provides an external TCLKIN path, and U62B divides that input by two
before FASTCLK. Apple Q3 through that path gives `1,022,727.14 Hz`. The RTL's
XCK input and asserted DIV2 model the same two stages. The internal
FASTCLK-to-SLOWCLK divider, pitch counter, and U62 voice divider stay intact.
The two-flop input synchronizer can move an edge by one fabric clock, but it
tracks the card's real Q3 rate and creates no generated FPGA clock.

The standard Mockingboard instead feeds about 1 MHz PHI2 to XCK with DIV2 low;
the effective rate is the same. The prior `894,886.25 Hz` default made `I=$A80`
produce `79.4466 Hz`. The final profile produces `90.7961 Hz`, exactly `8/7`
higher. This fixes the measured fundamental without a phone, ROM, or
internal-divider adjustment. It does not validate a tract model: the rejected
Q3/all-pole firmware still sounded low, monotonous, and muffled. The exact
original Phasor strap still merits a continuity or scope check. Do not change
SSI XCK when Phasor mode changes; the proved native-mode clock doubling applies
to the AY chips.

The generated Zynq fabric clock resolves to 133,333,344 Hz and samples Q3 more
than 65 times per nominal cycle. It does not synthesize or replace the card
clock.

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

The current source planned for F0.9.99 uses this exact section graph:

```text
VOICE -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2) -> U146
```

It also uses the unipolar U60 voice pulse, the inverted HCC D4+5 noise tap,
the PW3/U62 polarity gate, a 48 kHz output hold, and C381-style DC blocking.
`PW2 && !PW3` marks exactly B, D, P, T, and K. Keep those held sources silent
and let a following live phone drive the transitioning tract. Do not add a
guessed stop burst. Treat U68/U85C and the T-to-HF FRIC2 handoff as open until
a corrected net list or chip trace settles them.

The ideal filter model must use the charge state of the two switched
integrators, not guessed pole radii:

```text
p[n] = alpha*p[n-1] + a*(y[n-1] - u[n])
       + g*(v[n-1] - v[n])
y[n] = y[n-1] - b*p[n]
```

Derive `alpha`, `a`, `b`, and `g` from the drawn capacitor ratios. F1 uses its
prior voice sample for `v`; F3 uses the direct C127 F1 path. F2 resonance loads
the first integrator, so both its `alpha` and `a` share the loaded denominator.
Do not change a ratio to tune a phone.

Keep the two fricative amplifiers distinct. U157 resets in Phi1 and must
regenerate `-(Cselected/3900)*HCC` on every Phi0, including repeated high HCC
samples. Its switched 1000 pF C143 path enters F2's second integrator as
`-(1000/6800)*U157`; it has no cross-pair route history. U152 does not reset.
It keeps the edge recurrence
`U152'=U152-(Cselected/3900)*(HCC-HCC_previous)`. Its 1150 pF C150 path always
enters F5's first integrator, while the switched 3700 pF C151 path keeps a
separate history while open.

Model U146 with the drawn output capacitors:

```text
out[n] = (2700/2750)*out[n-1]
         + (Cselected/2750)*(F5[n-1] - F5[n])
```

CLOSURE copies the completed U146 hold to C100/U148 and otherwise holds it.
Phone end must not mute the post-U148 path. Snapshot all formant, resonance,
route, source, and FL_AMP controls at Phi0 so one transfer uses one phone
state.

Completed filter work:

1. derive double-precision state equations from each capacitor and switch
   section;
2. check all capacitor values and ratios from source in an independent test;
3. quantize coefficients and state widths;
4. use shared DSP multipliers without sharing chip state;
5. filter and resample the phase-domain output to 48 kHz;
6. guard the harmonic response, all 64 phones, route isolation, and PCM rails.

The final isolated out-of-context route used a 7.500 ns clock, mapped one audio
instance to five DSP48E1 blocks, and met setup with WNS `+0.113 ns`. This route
includes the final U157, U146, output-tail, Phi0 control snapshots, and the
registered F5 output-delta stage. The isolated check has no board input/output
delays or `HD.CLK_SRC`; use it only to screen internal audio paths. The one full
card route remains the timing signoff. The engine takes 34 clocks, well below
the 133-clock minimum phase gap.

Remaining analog work:

1. add LF356 bandwidth, CD4016 on-resistance, and stray capacitance to a direct
   circuit model;
2. compare the model with AO and post-C381 real-chip captures;
3. set final level from measured voltage, not phone-specific boosts.

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

## Firmware checkpoint status

The Q3/all-pole firmware from commit `cdb43efd` is rejected. Hardware speech
sounded worse: low, monotonous, and muffled. It must not be named or used as
the current test image.

The version remains `F0.9.99`. Commit
`9297eb3dfddbd5e5b8a8d0689680f27005571dd4` supplied the final charge engine to
one clean, non-incremental full build, `20260824T112723Z-9297eb3d-full`. The
normal `+0.300 ns` release gate rejected the first post-route result at
`+0.012 ns`. The same routed checkpoint then received one physical
optimization pass under temporary setup uncertainty. The script restored
uncertainty to `0.000 ns` before the final reports and bitstream. No second
synthesis, placement, route, or full build ran.

The final manual test checkpoint has setup WNS `+0.100 ns`, hold WHS
`+0.047 ns`, pulse-width WPWS `+0.265 ns`, no unconstrained internal endpoints,
no route errors, and a passing bus-skew report. This meets the temporary
`+0.100 ns` test floor. It is not a normal `+0.300 ns` timing promotion.

```text
Final F0.9.99 firmware path: .timing_runs\20260824T112723Z-9297eb3d-full\FIRMWARE_F0.9.99_SSI263_SCHEMATIC_WNS0p100.BIN
Final routed WNS: +0.100 ns
Final firmware size: 3,752,332 bytes
Final firmware SHA-256: EE8FE36167953B08F7E944574645CB66C9FC79DEE8829B2EB411670E250A41B7
```

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
11. `Implement schematic SSI-263 charge filters`
12. `Package final F0.9.99 SSI-263 firmware`
