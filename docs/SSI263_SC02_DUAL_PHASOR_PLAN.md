# Dual SSI-263 / SC-02 Phasor implementation plan

## Goal

Implement two independent SSI-263AP chips from the supplied SSI-263 data,
SC-02 schematic, and ROM. Use the original Applied Engineering Phasor socket,
clock, request, and audio-channel wiring. Do not retain any SC-01 or Votrax
speech code, table, map, timing rule, or compatibility path.

This plan treats sound quality as a test result, not as a design input. A change
to the speech engine must trace to the schematic, ROM, SSI-263 documents, or
original Phasor wiring. Do not add a phone-name rule or tune a capacitor ratio.

## Source order

Use these sources for separate parts of the design:

1. `sc-02_Final_Schematic_V1.00.pdf` defines the internal digital gates,
   counter paths, source routes, and ideal switched-capacitor network.
2. `ssi263a.bin` defines all phone data used by the selector engine.
3. The SSI-263A data sheet and programming guide define the host registers,
   A/R and D7 behavior, public clock laws, and P/AP reset behavior.
4. The original Applied Engineering Phasor manual, board photos, `mb-audit`,
   and the real-card AppleWin decode define card-level socket and request
   wiring.
5. Real SSI-263AP captures will set any term that the chip drawing does not
   define, such as absolute output voltage and nonideal analog response.

The 2020 SC-02 reconstruction also contains board changes used to make a
discrete build work. Keep its external amplifier, speaker, pot, and board-only
reset fixes outside the SSI-263 chip model.

## Supplied ROM identity and map

The source ROM is:

```text
C:\Users\hasse\OneDrive\Documents\Apple2\Appletini\Mockingboard\SC-02\ssi263a.bin
size:    2,048 bytes
CRC32:   CC0A72EE
SHA-256: 9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941
```

Offsets `000-1FF` contain the 512 active bytes. Offsets `200-7FF` are zero.
The lookup is exact:

```text
address = ((phone & 0x3f) << 3) | (selector & 7)
```

The upper nibble supplies the target, except selector 4 uses the host amplitude.
The target slots are:

| Selector | Target |
| ---: | --- |
| 0 | F1 frequency |
| 1 | F2 frequency |
| 2 | F2 resonance |
| 3 | shared F3 and F4 frequency code |
| 4 | filter amplitude, from the host amplitude register |
| 5 | voice amplitude |
| 6 | fricative amplitude |
| 7 | no parameter-state write |

The low ROM bits feed the drawn pulse and route logic. Sheet 5 stores them with
these exact latch polarities:

```text
PW0 = selector-0 TPARM0
PW1 = selector-1 TPARM0
PW2 = selector-2 TPARM2
PW5 = NOT selector-2 TPARM2
PW3 = NOT selector-2 TPARM1
```

U34D pin 11 is the `PW3` output. U180C feeds TPARM1 to U11C, while U30E
inverts it for U11A; the U34C/U34D cross-NAND latch therefore stores the
complement. The low bits must not become an abstract voiced, fricative, stop,
or phone-class decoder.

## Original Phasor wiring

Model the two populated sockets as follows:

- A5 selects the upper, secondary SSI-263 socket and Phasor channel A.
- A6 selects the lower, primary SSI-263 socket and Phasor channel B.
- A5+A6 writes both chips. A native dual-selected read keeps the proved A5
  D7 priority.
- In Mockingboard mode, each A/R output feeds its matching VIA CA1 input.
- In native Phasor mode, enabled A/R outputs feed the direct Apple IRQ route.
- Channel A and channel B remain separate. The FPGA audio convention sends A
  to left and B to right, but the original Phasor labels are A/B, not L/R.
- Echo+ hides the SSI host route without erasing either chip's state.

Both original sockets strap SSI pin 23, DIV2, to the pin 24 +5 V rail. The
implemented clock route uses Apple Q3 at about 2.045 MHz on XCK and asserted
DIV2, for an effective rate near 1.023 MHz. Both chips share that edge source
but share no die state. A Phasor mode change does not alter SSI XCK. The known
native-mode clock doubling applies to the AY chips, not to SSI XCK.

Board photos prove the DIV2 strap. A direct pin-22 continuity check or scope
capture remains useful proof of the Q3 trace and exact part-to-part rate; it is
not a reason to substitute a guessed clock.

## Host and reset contract

Each SSI-263AP implements:

- registers 0-3 and filter aliases 4-7;
- address and data capture at the end of the selected write condition;
- D7-only readback while D0-D6 remain outside the chip drive;
- active-low open-collector A/R;
- DR acknowledgement, repeat, CTL, and request behavior from the SSI data;
- AP `PD/RST` behavior driven by Apple RES;
- register retention where the AP source calls for it.

`card_enabled` is a virtual backplane boundary. It masks host writes, D7,
A/R, and audio at the wrapper. It does not reset either SSI die model. Only the
global FPGA reset initializes emulator state; Apple RES drives SSI `PD/RST`.

Keep these public timing laws:

```text
pitch_hz      = effective_XCK / (8 * (4096 - I))
filter_hz     = effective_XCK / (2 * (256 - FF))
frame_ticks   = 4096 * (16 - R)
phone_ticks   = frame_ticks * (4 - D)
```

## Exact selector and transition work

Follow sheets 3-7 as a clocked circuit:

- U44 divides FASTCLK by four.
- U45 Q1 is an internal phase bit. U45 Q2, Q3, and Q4 drive SEL0, SEL1, and
  SEL2.
- One selector slot lasts two SLOWCLK edges, or eight effective FASTCLK ticks.
- The full eight-slot scan lasts 64 effective FASTCLK ticks.
- The seven parameter stores move by the lower-ROM pulse and gate paths. A
  host write wins a same-edge data-path collision.
- Selector 3 writes the one shared F3/F4 code. Selector 7 writes no parameter
  store.
- Rate, duration, filter divider, inflection, and articulation follow their
  drawn immediate or stepped paths. Do not replace them with a smooth curve.

## Exact excitation work

### U60 glottal counter

U60 `/RST` and CET are tied high. U53C feeds `/VOICED` to CEP. P0 and P1 are
tied high, P2 is grounded, and P3 is visibly open in the supplied drawing.
The digital model keeps that unknown CMOS input as the explicit build-time
assumption `U60_OPEN_P3_LEVEL`; the current low setting makes U34A's active
parallel load jam `0011`, not zero. The exact Phi1 recurrence for that setting
is:

```text
q_next = U34_load ? 3 : (q == 15 ? 15 : q + 1)
VOICED = (q == 15)
```

U60 therefore advances `3..15`, then `/VOICED` lowers CEP and holds 15 until
the next U61/U34 load. The U118A/U201/C205 Phi1 transfer uses the post-clock
state at that same boundary: `14->15` selects the AGND source at once, and a
`15->3` parallel load selects the positive voice-amplitude source at once.
Do not add an asynchronous clear or a free-running wrap.

U61 `/CLR` is the physical `T/PD_/RST` net. Asserting `PD/RST` clears U61 and
the pending U34 load condition. It does not clear U60.

### U75 and U73 noise recurrence

U75 counts on each rising U41C edge. P0 is high, P2/P3 are low, and P1 is
visibly open. The model exposes P1 as `U75_OPEN_P1_LEVEL`; the current low
setting jams `0001` after terminal count, so the steady sequence is `1..15`,
not `0..15`. Use the post-rise U75 state for U42C:

```text
U42C = NOT(U75.Q2 OR U75.Q3)
```

The CD4006 U73 shifts on the following falling U41C edge. Preserve these
section connections:

```text
D1 input = D3+4
D2 input = D4+5
D3 input = D2+5
D4 input = U42C XOR D1+4 XOR D2+5 XOR D4+4 XOR D4+5
```

The source tap is U73 D3+4. The complete drawn fricative gate is:

```text
U104C     = PW3 AND U62./Q
FRICATIVE = NOT(D3+4 OR U104C)
             AND (U62./Q OR VOICE_AMP_ZERO)
```

U41C alone clocks this recurrence. Do not gate the noise register with an
invented `phone_active`, phone class, CTL, or `stop_class` signal.

### U68 and U85 amplitude/reset path

Implement the sheet-6 counter and reset path, not a decoded stop rule:

- U68 is a CD4029 in binary mode: B/D is tied to VCC and its jam inputs are
  grounded.
- U68 Q2-Q4 form `AMPCT1..3`; their NOR is `AMPCT_ZERO`.
- `U104C = PW3 AND U62./Q`; its inverse is the `AMPCT0` direction term.
- The voice- and fricative-amplitude zero tests take part in the shown count
  enable and direction gates.
- The gated SEL2 level clocks U68. Detect its full rising level because a gate
  can open while SEL2 is already high.
- U85C combines U104C with U68 `/CO` and drives the active-high U62 reset.

Test the complete count, direction, terminal, and U62-reset trace. No phone
name or `PW2 && !PW3` shortcut may replace it.

### U20B, U112, and U166 route state

Keep the two fricative switches as separate state:

- the gated WR_SEL2 edge clocks TPARM3 into U20B through the shown PW0, PW1,
  PW2, `AMPCT_ZERO`, and `FRIC_AMP_ZERO` terms;
- U112 is transparent while Phi1_X is low and produces `FRIC1_SW` from U20B;
- U166A samples the complement of U20B on the positive Phi0_X edge and holds
  `FRIC2_SW`.

Do not assign one switch from a ROM bit and the other to its direct inverse.
Their separate clocks and held state are part of the circuit.

## Exact ideal filter work

Keep the serial charge path:

```text
VOICE -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2) -> U146 -> U148
```

Do not reduce a four-switch bank to one code-to-gain value. Keep one retained
source-plate value for every one-switch capacitor. For a final switch mask
`M`, target `t`, stored plate values `h[i]`, and feedback capacitor `Cf`, one
atomic topology event is:

```text
delta_out = -sum(i in M, C[i] * (t - h[i])) / Cf
h[i]      = t for i in M; all open h[i] values hold
```

This rule must cover a selected source change, an open source change, a new
closure, a pure opening, and a simultaneous multi-bit change such as 7 to 8.
Reset phases precharge selected plates to the reset target and send that charge
to AGND. They do not clear open plates.

The one-switch banks are:

| Bank | Capacitors, low bit first (pF) | Fixed or feedback capacitor (pF) |
| --- | --- | ---: |
| VOICE | 220, 430, 870, 1800 | C205=3300 |
| F1 frequency | 160, 330, 660, 1300 | fixed 250; feedback 11500 |
| F2 resonance | 220, 430, 870, 1800 | fixed load 200; feedback 6800 |
| F2 frequency | 280, 560, 1120, 2300 | fixed 500; feedback 6800 |
| F3 frequency | 210, 420, 820, 1640 | fixed 820; feedback 4700 |
| F4 frequency | 200, 400, 820, 1620 | fixed 1670; feedback 4300 |
| FRIC1 | 270, 512, 1068, 2160 | C133=3900 |
| FL amplitude | 76, 150, 300, 600 | C172+C173=2750 |

FRIC2's 270, 530, 1082, and 2160 pF input bank has complementary switches,
so it has no floating per-bit plate. A code change at a steady divider level
causes no U152 impulse; the final mask weights the next real divider edge.

At phase entry, solve the drawn stable equations without rounded gain tables:

```text
p1' = [11500*p1 + 2700*(y1-x) - 2700*x] / 11700
y1' = y1 - [250*p1' + sum(F1 Ci*(p1'-hi))] / 11500

D2  = 6800 + 200 + sum(selected F2 RES Ci)
p2' = [6800*p2 + 4700*(y2-x2) + sum(RES Ci*ri)] / D2
y2' = y2 - [500*p2' + sum(F2 Ci*(p2'-hi))] / 6800
            + 1000*sum(L143_old-L143_new) / 6800

p3' = [4700*p3 + 3900*(y3-x3) + 2000*(y1_old-y1_new)] / 4900
y3' = y3 - [820*p3' + sum(F3 Ci*(p3'-hi))] / 4700

p4' = [4300*p4 + 4700*(y4-x4)] / 4500
y4' = y4 - [1670*p4' + sum(F4 Ci*(p4'-hi))] / 4300

p5' = [3450*p5 + 4700*(y5-x5)
       - 1150*delta_s150 - 3700*delta_s151] / 3730
y5' = y5 - 4700*p5' / 3450
```

F1, F3, and F5 run in Phi1. F2 and F4 run in Phi0. F3 must consume the F2
value just completed in Phi0 and the same-Phi1 F1 change through C127. F5 must
consume the F4 value just completed in Phi0. A live event during an active
phase applies only its charge increment; it must not rerun a stable phase
recurrence.

U152 and U154 form a required two-state loop through C139-C142. With U152
output `s`, U154 output `z`, and a selected-bank divider-edge step `e`:

```text
Phi1->Phi0: ds=12*z/13; s+=ds; z-=19*ds/13
Phi0 edge:  s+=e; z-=19*e/13
Phi0->Phi1: z-=12*s/13
Phi1 edge:  s+=e; z-=31*e/13
```

C150 and selected C151 pass only U152 changes that occur during Phi1. The
Phi1-to-Phi0 U152 change flows into the grounded FRIC2 bus and must not enter
F5. Original U166 timing closes C151 only at that grounded boundary, so it
precharges before the next active phase and has no active reconnect impulse.

U146's Phi1-entry recurrence is:

```text
o' = [2700*o + sum(selected FL Ci*(hi-y5_new))] / 2750
```

Later Phi1 F5 or FL-mask events add only their new input-cap charge; they do
not apply the `2700/2750` entry factor again. Selected FL plates precharge to
the current F5 value in Phi0, while open plates hold. CLOSURE copies the
completed U146 state to U148. A phone boundary clears none of these states.

Use wide signed charge sums and round once after each complete node equation.
The output operation is only a one-bit Q16-to-signed-Q15 number conversion.
There is no `LINE_OUTPUT_SHIFT=3` gain control.

The exact FPGA engine uses six shared 25-by-16 signed product lanes. Every
physical division uses one of the 28 live schematic denominators. A 37-bit
reciprocal product gives `q` or `q-1`; one registered `(q+1)*d` check selects
the exact quotient after signed nearest rounding. Each analog stage takes nine
fabric clocks. The longest Phi0 job takes 110 clocks from dequeue to the next
pop; Phi1 and code events take at most 101 clocks. A 16-entry ordered event
queue covers the proved physical Q3/DIV2 rates. The routed out-of-context audio
engine uses 11 DSP48E1 blocks and has `+0.641 ns` WNS at a 7.500 ns fabric
period. This is an engine check, not final full-card timing.

C381 is outside the present chip output model. The drawing gives its
capacitance but not the load, so it does not define an RC pole. Export the
reconstructed AO/U148 node. Add a board-level C381 term only after the load is
known; do not use the rejected 255/256 digital high-pass estimate.

## Deliberately absent behavior

The implementation must contain none of these prior shortcuts:

- SC-01 ROM, coefficients, phone map, timing, decoder, or audio core;
- Votrax compatibility wiring or control;
- `stop_class` or `PW2 && !PW3` source muting;
- abstract `phone_voiced`, `phone_fricative`, `voiced`, or `fricative` gates;
- `phone_active` as an audio-source or filter-tail gate;
- direct or complementary ROM assignments for both fricative route switches;
- the 255/256 C381 high-pass estimate;
- `LINE_OUTPUT_SHIFT=3` or any phone-specific gain, burst, presence, or pitch
  adjustment.

The configured HDL source closure now contains only the SSI-263 ROM, XCK edge
block, native core, charge/audio core, wrapper, and two Phasor instances for
speech. No SC-01 or Votrax source enters the build.

## Deterministic FPGA power-up and model limits

Some reconstructed counters and shift registers have no useful chip reset.
The FPGA must still start from known bits. Use fixed seeds for repeatable tests:
U60=`F`, U75=`F`, U73 D1=`0001`, D2=`00000`, D3=`0000`, D4=`00000`, U68=`0`,
U20B=`0`, FRIC1_SW=`0`, and FRIC2_SW=`1`. These seeds define emulator power-up;
they do not claim that every SSI-263 die powers up with those analog or digital
states. Tests must prove the recurrence after the seed.

The charge model uses ideal components and rounded fixed-point math. It does
not yet model LF356 bandwidth and slew, CD4016 resistance and charge injection,
stray capacitance, source pulse voltage, temperature, component tolerance, or
chip spread. Saturation protects the digital state rails but is not a claim
about silicon clipping. Set absolute level only from real AO measurements.

## Verification plan

Before the final build, require source and cycle tests for:

- the ROM size, hashes, all 512 active bytes, and all zero padding;
- every phone/selector address and target;
- register write-end capture, aliases, D7, A/R, DR modes, repeat, CTL, and AP
  `PD/RST` collisions;
- U44/U45 eight-tick selector slots and the full transition traces;
- the sheet-5 PW0/PW1/PW2/PW5 latch polarities and inverted U34D `PW3`,
  including row `$01` slot-2 byte `$0E` producing `PW3=0`;
- U61 `PD/RST` clear without U60 clear, U60 load to 3, `3..15`, terminal hold,
  and post-clock Phi1 source selection;
- U75 `1..15`, post-rise U42C, CD4006 falling-edge recurrence, D3+4 source tap,
  and the full FRICATIVE equation;
- U68 count/direction/terminal state and U85C-to-U62 reset behavior;
- U20B/U112/U166 route clocks and held switch state;
- every capacitor value, retained-plate reconnect, simultaneous mask change,
  exact node equation, side path, route, and sign;
- U152/U154 Phi0 and Phi1 recurrences, and proof that C150/C151 accept only
  live Phi1 U152 changes;
- same-event causality from new F2 to F3, new F4 to F5, F1 through C127, and
  new F5 to U146;
- pending-event and engine-overrun checks at the fastest filter setting;
- persistent tract/output state across phone and card-mode boundaries;
- `card_enabled` boundary masking without die reset;
- A5/A6 writes, reads, A/R-to-VIA and native IRQ paths, A/B audio isolation,
  and simultaneous speech;
- absence of SC-01/Votrax files, names, elaborated cells, and synthesis inputs.

Then run all relevant source regressions and focused XSim benches. The final
full Vivado build must start from the final source commit. Run one full build,
not two. Keep it if synthesis, implementation, route, constraints, and the
temporary user-approved timing floor pass.

## Firmware rule

Keep the version at `F0.9.99` for the final SSI-263 fixes.

All firmware made before the current exact schematic corrections is rejected
and must not be offered for testing. Do not retain an old firmware path or hash
as the current artifact.

```text
Final pre-build suite:  passed
Final full build:       pending
Final F0.9.99 firmware: pending
Final routed timing:    pending
Final firmware size:    pending
Final SHA-256:          pending
```

Package only the bitstream from the one named passing timing run. Verify that
packaging did not fall back to a stale root or project bitstream.

## Acceptance gate

The branch is ready for the next hardware listen only when:

- no SC-01 or Votrax implementation exists in the active build or source
  closure;
- both speech sockets use independent SSI-263AP state;
- the ROM, selector, U60, U68/U85, U75/U73, U20B/U112/U166, request, and reset
  traces pass against independent expected vectors;
- the ideal filter uses only the drawn topology, capacitor values, and signs;
- A5/A6, channel A/B, VIA CA1, native IRQ, Q3, and DIV2 match the original
  Phasor route;
- all focused and source tests pass;
- the one final F0.9.99 build passes and yields a named, hashed firmware image.

Hardware listening can find a fault, but it cannot by itself prove an exact
SSI-263 analog match. That claim needs same-vector AO captures from a real
SSI-263AP.

## Logical commit checkpoints

1. Remove SC-01 and Votrax build residue.
2. Correct selector and U60 clock recurrences.
3. Implement exact U75/U73 excitation gates.
4. Implement U68/U85 and U20B/U112/U166 state.
5. Remove unsupported output and phone-class rules.
6. Replace mirrored tests with exact circuit traces.
7. Lock original Phasor wiring and two-chip isolation.
8. Update the implementation report and source audit.
9. Run one final full build and package F0.9.99.
