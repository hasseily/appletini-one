# Dual SSI-263 / SC-02 Phasor implementation report

## Result and scope

The branch now uses two independent SSI-263AP models for Phasor speech. The
active source closure contains no SC-01 core, ROM, coefficient set, phone map,
timing rule, decoder, Votrax interface, or compatibility path. Both speech
sockets instantiate `ssi263_voice`, which joins the native SC-02 digital core
to the ideal schematic charge model.

The sheet-5 control path now follows U37/U38 and the three timed cross-NAND
latches. PW0/PW1 are set-only states, while PW3 loads the CTRL/TPARM1 result
only at the comparator-qualified slot. The source also removes the prior
sound-driven shortcuts: it has no invented stop mask, abstract phone-class
source gate, direct ROM/inverse fricative-switch pair, guessed C381 high-pass,
or three-bit output-level shift. Source and simulation closure are complete.
The one full build and matching firmware package are complete. The routed test
candidate has positive setup, hold, and pulse-width slack. Hardware listening
is the next checkpoint.

The target version remains `F0.9.99`.

## Source basis

The implementation separates chip and card proof:

- the supplied SC-02 schematic defines the selector, transition, excitation,
  route, and ideal switched-capacitor paths;
- the supplied SSI-263 ROM defines the 64 by 8 phone table;
- the SSI-263A data sheet and programming guide define registers, public timing,
  D7, A/R, DR modes, and P/AP reset behavior;
- the original Applied Engineering Phasor manual, board photos, `mb-audit`, and
  real-card AppleWin decode define A5/A6, channel A/B, VIA, IRQ, Q3, and DIV2
  behavior.

The SC-02 reconstruction includes external changes made for its discrete board.
The chip model excludes its AD817, LM386, speaker, pots, and line-driver stages.

## SC-01 removal

The speech source closure now consists of:

```text
hdl/apple/ssi263_sc02_rom.mem
hdl/apple/ssi263_xck_ce.sv
hdl/apple/ssi263_sc02_core.sv
hdl/apple/ssi263_sc02_audio.sv
hdl/apple/ssi263_voice.sv
hdl/apple/mockingboard.sv
```

No tracked SC-01 or Votrax source file remains, and neither name appears as a
speech implementation input in `hdl/hdl_sources.txt`. VIA PCR and ORB state no
longer select a hidden SC-01/Votrax path. Software written only for an SC-01
interface will not speak through this Phasor model.

## ROM identity and addressing

The imported source is:

```text
C:\Users\hasse\OneDrive\Documents\Apple2\Appletini\Mockingboard\SC-02\ssi263a.bin
size:    2,048 bytes
CRC32:   CC0A72EE
SHA-256: 9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941
```

The active table is the first 512 bytes. The remaining 1,536 source bytes are
zero. The address is exact:

```text
rom_address = {phone[5:0], selector[2:0]}
            = ((phone & 0x3f) << 3) | (selector & 7)
```

Selectors 0-6 target F1, F2, F2 resonance, shared F3/F4, filter amplitude,
voice amplitude, and fricative amplitude. Selector 4 substitutes the host
amplitude for the upper ROM nibble. Selector 7 writes no parameter state. The
low nibble remains a set of circuit controls; the model does not turn it into
SC-01-like phone traits.

Sheet 5 gives these timed rules:

```text
decoded /PHO_WRITE is high at U29A while the phone write is active
U37 advances on U29A's falling edge at write assertion or DURCLK rise
DONE_RB holds U37 at zero while its low nibble is zero; that hold wins a clock
U38 equal = U37.low == (TPARM0 ? 2 : 6)
PW0 set = U38_equal AND WR_SEL0
PW1 set = U38_equal AND WR_SEL1
PW0/PW1 clear while /PHO_WRITE is low, otherwise hold
PW2 = selector-2 TPARM2
PW5 = NOT selector-2 TPARM2
PW3 loads (latched_CTRL OR NOT TPARM1)
    only when U38_equal AND WR_SEL2; otherwise it holds
```

TPARM0 chooses the comparator delay; it does not become PW0 or PW1 data.
TPARM1 reaches PW3 through U180C/U30E/U11/U34 only on the qualified load.
Static per-selector ROM assignments do not reproduce these held states.

U21B, U28, U29A, U36, U37, and U23B now run as the sheet-5 circuit rather
than from a phoneme-end timer. U21B Q is `RATECLK/2`; its complement clocks
U28 and U36. U28 loads `12+D`, counts only on the Q-falling half-cycle, and
holds terminal count through the Q-high half-cycle. Thus:

```text
DURCLK period = (4-D) * 256 * (16-R) effective FASTCLK ticks
frame period  =          4096 * (16-R) effective FASTCLK ticks
phoneme       = (4-D) * 4096 * (16-R) effective FASTCLK ticks
phoneme       = 16 DURCLK rises
```

U28, U36, and U37 have no phone-active or power-down clock gate. A phone
write resets U36 and drives the shared U29A level; it does not force an
unconditional U28 reload. U36 enters low-nibble `F` on its fifteenth count
after reset and requests DONE in retained frame mode. In retained phoneme
mode, the sixteenth DURCLK wraps U37 from `F` to zero and requests DONE.
Phone-write reset, CTL/power-down reset, and frame-mode WR1/WR2 acknowledge
reset U23B with priority over a same-edge request. DR=00 keeps the prior
frame/phoneme selection.

## Clock and selector correction

Both Phasor sockets assert DIV2. Original-board evidence shows SSI pin 23 tied
to the pin 24 +5 V rail on both sockets. The model feeds both XCK inputs from
Apple Q3, about 2.045 MHz, and the SSI DIV2 stage produces an effective rate of
about 1.023 MHz. A card-mode change leaves this clock alone. The Phasor's AY
clock doubling is a separate circuit and does not double SSI XCK.

The direct pin-22 Q3 trace is the only source-supported route that fits the
required raw clock with DIV2 high. A scope or continuity test can add physical
proof, but the implementation does not replace it with a tuned oscillator.

The prior selector scan included one extra divide-by-two. The corrected trace
is:

- U44A/U44B divide FASTCLK by four;
- U45 Q1 and Q2 drive the WRITE/LATCH timing gates;
- U45 Q3/Q4/Q5 drive SEL0/SEL1/SEL2;
- one selector slot spans four SLOWCLK edges, or sixteen effective FASTCLK
  ticks;
- all eight selector slots span 128 effective FASTCLK ticks.

The transition path now keeps the four storage terms drawn on sheet 4 for all
eight selectors: U89 `RESA`, U88 `RESB`, U90 `RESC`, and U87 direction carry.
An `A_CLR` setup holds A, writes `B=(target-A) mod 16`, writes `C=8`, and stores
`CY=(target>=A)`. Each later permitted pulse adds B to C. An upward transition
increments A on carry; a downward transition decrements A on no carry;
`RESA_EQUALS` stops the write path at the target.

The prototype U96 route is also explicit. Slots 0, 1, and 3 use
`TCEDGE AND U32B`; slot 2 uses `TCEDGE`; slot 4 uses
`DUREDGE AND U166B./Q AND AMP_NONZERO`; slots 5 and 6 use `PW0 AND RATEEDGE`
and `PW1 AND RATEEDGE`; slot 7 is grounded. The fitted prototype state grounds the
P2/R301 RATE=F option. A host write owns a same-edge data-path collision.

## Exact source circuit

### U60 glottal path

U60 `/RST` and CET are tied high, while U53C feeds `/VOICED` back to CEP. P0
and P1 are tied high, P2 is grounded, and P3 is tied high. U34A therefore
loads `1011` after the applicable U61/U62 edge. Normal Phi1 clocks advance
`11..15`; at 15,
`/VOICED` lowers CEP and U60 holds terminal count until the next parallel load.
There is no asynchronous clear and no free-running wrap.

U118A connects U201 to the selected C205 voice-amplitude bank at the same Phi1
boundary. The model uses U60's post-clock state for that transfer: `14->15`
selects AGND immediately, while a `15->11` load selects the positive source
immediately. This fixes the counter start, stop, and source edge from the
drawn pins; it is not a pitch or timbre setting.

U61 `/CLR` follows the physical `T/PD_/RST` net. A low level clears the two
U61 stages and removes a pending U34 load, but leaves U60 unchanged.

The drawing includes POT3 in the U3C/U117/U3B voiced-source path but gives no
wiper setting. `VOICE_TRIM_U116_STEP_Q16` keeps that one physical calibration
value explicit. Its default is one normalized rail step; it is not selected by
phone and is not changed to improve a listen test. A real-chip AO capture is
needed to set the absolute voltage.

### U75 and CD4006 U73

U75 counts on U41C rising edges. P0 is high and P1/P2/P3 are grounded. Its
terminal-load path jams one, so the steady counter sequence is `1..15`.
The post-rise counter state forms:

```text
U42C = NOT(U75.Q2 OR U75.Q3)
```

The CD4006 sections shift on the following U41C falling edge:

```text
D1 input = D3+4
D2 input = D4+5
D3 input = D2+5
D4 input = U42C XOR D1+4 XOR D2+5 XOR D4+4 XOR D4+5
```

The schematic source is D3+4, not D4+5. The final gate is:

```text
U104C     = PW3 AND U62./Q
FRICATIVE = NOT(D3+4 OR U104C)
             AND (U62./Q OR VOICE_AMP_ZERO)
```

The U41C edge advances the recurrence without `phone_active`, CTL, a decoded
phone class, or a stop flag. Voice amplitude zero and the drawn PW3/U62 term
provide the source gating.

### U68/U85 U62 reset path

The source now follows the sheet-6 amplitude counter instead of
`PW2 && !PW3` muting:

- U68 runs as a CD4029 binary counter with B/D tied high and grounded jam
  inputs;
- Q2-Q4 form `AMPCT1..3`, and their NOR is `AMPCT_ZERO`;
- `U104C = PW3 AND U62./Q`, with its inverse supplying `AMPCT0`;
- the voice- and fricative-amplitude zero terms control the shown direction and
  enable logic;
- the complete gated SEL2 level clocks U68, including a rising edge caused by
  a gate opening while SEL2 is high;
- U68 `/CO` and U104C feed U85C, whose output resets U62.

This path now decides the source envelope and U62 state. There is no special
B/D/P/T/K mute and no synthetic stop burst.

### Parameter and phase latch layers

The selector path has three distinct storage layers. U89 holds RESA for each
selector. U106-U114 follow only during that selector's LATCH window, eight
FASTCLK ticks after WRITE. U170-U176 then pass the first-stage code only while
the matching capacitor bank is grounded: F1/F3/voice in Phi0_X and
F2/F2-res/F4/fricative in Phi1_X.

U111 holds raw filter amplitude. U70 gates its four bits with
`{AMPCT3, AMPCT2, AMPCT1, AMPCT0}`, and U206 captures that result only on the
positive Phi0 edge. It does not follow later U111 or AMPCT changes in Phi0.

### U20B/U112/U166 route state

The two fricative routes are not one ROM bit and its live complement.

The gated WR_SEL2 path clocks TPARM3 into U20B. Its clock gate contains the
drawn PW0, PW1, PW2, `AMPCT_ZERO`, and `FRIC_AMP_ZERO` terms. U112 remains
transparent while Phi1_X is high and supplies `FRIC1_SW` from U20B. U166A
samples `/U20B` on the positive Phi0_X edge and holds `FRIC2_SW`. Their
different clocks preserve the route handoff drawn on sheet 7.

## Ideal switched-capacitor implementation

The model keeps one serial tract:

```text
VOICE -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2) -> U146 -> U148
```

The implementation does not turn a four-switch bank into one code-to-gain
table. It keeps one source-plate voltage for each switched capacitor. For a
final switch mask `M`, source target `t`, retained plates `h[i]`, and feedback
capacitor `Cf`, one atomic topology event is:

```text
delta_out = -sum(i in M, C[i] * (t - h[i])) / Cf
h[i]      = t for i in M; every open h[i] holds
```

This covers source changes, switch changes, reset precharge, reconnects, and
multi-bit changes such as `7->8`. A code change at a steady source creates no
FRIC2 impulse; its final complementary switch mask weights the next real
source edge.

The per-switch values are copied from the drawing:

| Bank | Switched capacitors, low bit first (pF) | Fixed or feedback capacitor (pF) |
| --- | --- | ---: |
| U116 voice amplitude | 220, 430, 870, 1800 | C205=3300 |
| U157 FRIC1 amplitude | 270, 512, 1068, 2160 | C133=3900 |
| F1 frequency | 160, 330, 660, 1300 | fixed 250; feedback 11500 |
| F2 resonance | 220, 430, 870, 1800 | fixed load 200; feedback 6800 |
| F2 frequency | 280, 560, 1120, 2300 | fixed 500; feedback 6800 |
| F3 frequency | 210, 420, 820, 1640 | fixed 820; feedback 4700 |
| F4 frequency | 200, 400, 820, 1620 | fixed 1670; feedback 4300 |
| U146 filter amplitude | 76, 150, 300, 600 | C172+C173=2750 |
| U152 FRIC2 input | 270, 530, 1082, 2160 | C153=3900 |

For state `pN` at the first integrator and `yN` at the second, the phase-entry
work uses the raw capacitor equations, not rounded pole values:

```text
p1' = [11500*p1 + 2700*(y1-x) - 2700*x] / 11700
y1' = y1 - [250*(p1'-hfix) + sum F1 Ci*(p1'-hi)] / 11500

p2' = [6800*p2 + 4700*(y2-y1) + sum RES Ci*ri]
      / [7000 + sum RES Ci]
y2' = y2 - [500*(p2'-hfix) + sum F2 Ci*(p2'-hi)
            - 1000*(Lold-Lnew)] / 6800

p3' = [4700*p3 + 3900*(y3-y2) + 2000*(y1old-y1new)] / 4900
y3' = y3 - [820*(p3'-hfix) + sum F3 Ci*(p3'-hi)] / 4700

p4' = [4300*p4 + 4700*(y4-y3)] / 4500
y4' = y4 - [1670*(p4'-hfix) + sum F4 Ci*(p4'-hi)] / 4300

p5' = [3450*p5 + 4700*(y5-y4)
       - 1150*delta_C150 - 3700*delta_C151] / 3730
y5' = y5 - 4700*(p5'-hfix) / 3450
```

The mid-Phi1 C128 path applies `-5400*delta_x/11700` to F1. C127 then consumes
that same event's F1 change, not the prior event. U157 resets in Phi1 and feeds
F2 through C143 in Phi0. U152 and U154 keep state and follow their drawn
`12/13`, `19/13`, and `31/13` edge recurrences. C150 and C151 see only U152
changes while Phi1 is active; C151 keeps its source plate while its switch is
open. U146 has a 2700 pF held path plus the four filter-amplitude switches.
CLOSURE copies its completed value to U148. Phone completion does not mute or
clear the tract.

State uses a normalized signed Q16 voltage scale in wider signed registers.
Each physical division rounds once to the nearest signed integer and saturates
only at the 24-bit state rail. There are no Q14 pole or gain tables. The PCM
boundary shifts once from Q16 to signed Q15; it adds no output gain. The
removed `LINE_OUTPUT_SHIFT=3` had no schematic basis.

The FPGA arithmetic keeps the charge result exact for the drawn integer
capacitors. Six shared 25-by-16 signed lanes form each numerator. The only live
denominators are the 28 schematic capacitance sums. For each, a 37-bit
reciprocal product returns the exact quotient or one less; a registered
`(q+1)*d` product makes the final correction. One analog stage takes nine
fabric clocks. Phi1 and code jobs need at most 101 clocks, and the worst Phi0
job needs 110 clocks before the next event can pop. A 16-entry FIFO preserves
event order. Routed out-of-context verification at a 7.500 ns fabric period
uses 11 DSP48E1 blocks and reports `+0.641 ns` WNS. The one full-card build for
this hardware test uses 26 DSP48E1 blocks in all, including 22 for the two SSI
engines, and reports `+0.039 ns` WNS, `+0.061 ns` WHS, and `+0.265 ns` WPWS.

The chip drawing includes C381 but omits its external load. It therefore does
not define a high-pass pole. The model exports the reconstructed AO/U148 node
without the old 255/256 digital high-pass. A later board model may add C381
only after the load and capture point are known.

## Original Phasor integration

The card has two fixed AP-revision instances:

| Select | Physical role | Phasor output | Request route |
| --- | --- | --- | --- |
| A5 | upper, secondary SSI socket | channel A | matching VIA CA1 in Mockingboard mode; direct IRQ in native mode |
| A6 | lower, primary SSI socket | channel B | matching VIA CA1 in Mockingboard mode; direct IRQ in native mode |

The bus and channel rules are:

- A5 writes only the channel-A chip; A6 writes only the channel-B chip;
- A5+A6 writes both chips;
- native reads expose the selected chip's D7, with A5 priority on a dual read;
- request, register, transition, excitation, filter, and audio state remain
  independent;
- Echo+ masks the SSI host route without clearing either chip;
- the FPGA maps channel A to left and channel B to right, with no speech mono
  pre-sum; the original card itself names the outputs A and B, not left/right;
- both chips use the same Q3 edge stream and asserted DIV2 strap, independent
  of Phasor mode.

The prior firmware also forced a hidden post-card warmth value of `+8` over
the complete PSG and SSI mix. That filter is not part of either schematic and
reduced speech treble before the DAC. The reset and firmware default are now
neutral (`0`). Explicit user bass, mid, treble, and volume controls remain
optional board-output processing; keep them at zero for a direct SSI listen.

## Card-enable and reset behavior

`card_enabled` now masks only the virtual card boundary:

- it blocks new host writes;
- it removes D7 and A/R from the Apple bus;
- it mutes the exported speech sample;
- it does not reset the core, source registers, counters, routes, or filter
  state.

Apple RES drives the AP `PD/RST` input. The global FPGA `rstn` still provides a
defined emulator start. Treating a menu or slot-disable flag as a die reset did
not match original wiring and has been removed.

## Removed non-schematic behavior

The current engine has none of these earlier rules:

- `stop_class` or `PW2 && !PW3` source suppression;
- `phone_voiced`, `phone_fricative`, or equivalent phone traits;
- abstract `voiced` or `fricative` gates into the audio engine;
- `phone_active` source gating or tail muting;
- an inverted D4+5 noise source tap;
- direct `FRIC1_SW=ROMbit` and `FRIC2_SW=!ROMbit` assignments;
- a 255/256 C381 high-pass estimate;
- `LINE_OUTPUT_SHIFT=3`;
- the hidden forced `+8` warmth filter;
- an asynchronous U60 clear, load-to-zero rule, or free-running terminal wrap;
- a phone-specific burst, gain, pitch, presence, or resonance adjustment.

Earlier listen-test firmware used one or more of these discarded paths. All of
those images are rejected. Their paths and hashes are intentionally absent from
this report so that none can be mistaken for the next test image.

## Deterministic power-up state

An FPGA needs known power-up bits even where the reconstructed chip circuit has
no useful reset. The model uses these repeatable seeds:

```text
U60       = F
U75       = F
U73 D1    = 0001
U73 D2    = 00000
U73 D3    = 0000
U73 D4    = 00000
U68       = 0
U20B      = 0
U28       = F
U36 low   = 0
U37 low   = 0
FRIC1_SW  = 0
FRIC2_SW  = 1
filter and charge state = 0
```

These values make simulation, synthesis, and hardware runs repeatable. They do
not assert that all SSI-263AP dies start at the same unreset state. Verification
must compare the recurrence after initialization, not use the seed as proof of
silicon power-up.

## Verification status

The source corrections now cover the full known digital path. Tests based on
phone classes, stop muting, or guessed C381/output terms are not proof. The
final pre-build suite now checks:

- ROM identity, padding, and all 64 by 8 addresses;
- register, D7, A/R, repeat, DR, CTL, AP reset, and collision traces;
- U44/U45 selector cadence;
- upward and downward U83/U84 DDA vectors and the full prototype U96 route;
- sheet-5 timed PW0/PW1, PW2/PW5 polarity, and comparator-gated CTRL/TPARM1
  load into `PW3`;
- all 64 rate/duration U28 recurrences, sixteen DURCLK rises per full phone,
  U36 count-15 frame response, U37 wrap phoneme response, and reset collisions;
- U61 `PD/RST` clear without U60 clear, U60 load to 11, `11..15`, terminal hold,
  and post-clock Phi1 source selection;
- U75 rising count, U73 falling shift, post-rise U42C, feedback taps, D3+4
  source, and the full FRICATIVE gate;
- U68/U85 count, direction, terminal, and U62 reset traces;
- U20B/U112/U166 clock and hold behavior;
- all filter cap tables, ratios, signs, route histories, and persistent state;
- no non-schematic CTL hard clamp on the source or PCM output;
- boundary-only `card_enabled` behavior;
- A5/A6 decode, both VIA CA1 paths, native IRQ, A/B isolation, and two-chip
  concurrent speech;
- source and synthesis closure with no SC-01 or Votrax implementation.

The current source checkpoint passes:

- 48 ROM, interface, formula, selector, duration, DONE, DDA, and U96 reference
  tests;
- the raw-Q3 XCK edge bench and the complete native core bench;
- 4,032 exact-divider vectors across all 28 live denominators;
- 1,745,273 checks over all 64 phone rows plus the natural ten-phone
  `HF EH1 L O OU PA E N D PA` sequence at the physical fabric/Q3/DIV2 rates;
- 15 Phasor source and decode tests;
- 74 dual-card checks for A5/A6, channel A/B, request, mode, reset, both RESA
  latch layers, row `$01`, simultaneous audio, and both engine-overrun flags.

The phone sweep checks the timed PW, DDA, route, and output invariants without
assuming that every settled parameter must equal its ROM target in one scan.
Source closure finds zero tracked or production SC-01/Votrax paths.

## Fidelity limits

| Area | Current claim | Limit |
| --- | --- | --- |
| ROM | Exact to the supplied binary | Does not prove the binary matches every mask-ROM revision |
| Register and timing behavior | Source-derived synchronous model with independent collision and boundary vectors | CMOS gate delay and pin captures remain unmodeled |
| Selector and transition | Drawn U44/U45 cadence, U83/U84 DDA, and prototype U96 routes | FPGA enables do not model CMOS propagation delay |
| U60/U68/U75/U73 routes | Drawn counter, gate, and held-state recurrences | Deterministic seeds are emulator choices for unreset state |
| Filter topology and ratios | Ideal schematic charge equations with one retained state per switch | Q16 voltage units and finite 24-bit state width add small numeric error |
| Source and output level | Unit Q16 normalization | Absolute U150/AO voltage needs a real-chip capture |
| Nonideal analog behavior | Not modeled | LF356 slew/bandwidth, CD4016 resistance and charge injection, parasitics, tolerance, temperature, and chip spread remain |
| C381 | Omitted from chip output | External load and desired capture point are not given |
| Phasor clock | Q3 plus DIV2-high source route | Pin-22 continuity or scope capture remains useful physical proof |

The ideal model can follow every shown capacitor and switch while still
differing from real silicon. Do not change a drawn ratio to hide that gap. Add
a nonideal term only when the part model and same-vector capture support it.

## Firmware and build status

The version is fixed at `F0.9.99`. Every prior firmware image is rejected: each
predates the corrected U21B/U28/U29A/U36/U37/U23B path and its natural-phone
proof. Do not use the earlier WNS `+0.013 ns` image. The new image comes from
one clean, full, non-incremental Vivado build. Positive setup, hold, and
pulse-width slack is the hardware-listen gate; the normal `+0.300 ns` release
margin remains a later timing task.

```text
Current pre-build suite: passed
Current full build:      20260825T085343Z-bf0d8d29-full; one run
Source commit:           bf0d8d293afc676dbd13a1ff8f1dd9fce357d088
Build mode:              full, clean tree, no incremental reference
Route and bus skew:      PASS
Timing:                  WNS +0.039, WHS +0.061, WPWS +0.265 ns
Current F0.9.99 image:   FIRMWARE_F0.9.99_DUAL_SSI263_SC02_WNS0p039.BIN
Firmware size:           4,309,964 bytes
Firmware SHA-256:        096d9bf884156a041ecb60a34113a67c9e02bb15560e83540fed1f8b44b527ed
Hardware listen:         pending
```

The normal export flow kept the `+0.300 ns` release gate and stopped after the
one implementation. The guarded test export reopened that same final
checkpoint and made no placement or routing change. Its checks found zero
setup, hold, pulse-width, route, constraint, or unconstrained-endpoint faults.
The candidate DCP, route, and bus-skew reports all passed.

Artifact hashes are:

```text
BIT:            bfa19f921bbec2bbcec593b0e2b328696b24a36b54197575595fb6ca038593b0
XSA:            bcb2622fb6206275ecc8bf3ac0f771c72af2d5c5ea75006a9a6e6f816a096375
FSBL ELF:       7e47637e290b70701381d8ac3c91c267129dde7e4b12e172e76b4634786f7079
Core-1 ELF:     cba77543cece3125c6d06e8561a8e7b6e7c8a50000ec6e1c010881d93144271e
Frontend ELF:   339b492e66c2d6a3a65448e958103a07af326cc25665daee784680c646c8d72c
Firmware BIN:   096d9bf884156a041ecb60a34113a67c9e02bb15560e83540fed1f8b44b527ed
```

The archived XSA and the Vitis input XSA have the same SHA-256. One Vitis run
built the platform and all applications. One Bootgen run used the archived BIT
path explicitly. Bootgen readback names that BIT and shows two frontend load
partitions at `0x00100000` and `0x0027c000`. The frontend ELF contains
`F0.9.99`.

## Hardware validation for this candidate

The next Appletini test should first check that the new image boots, Phasor is
seen, both SSI chips respond, A5/A6 route to separate channels, both request
paths work, and simultaneous speech stays independent. Then listen to fixed
register vectors across voiced, fricative, mixed, and transition phones.

For a real-chip match, capture XCK, DIV2, A/R, D7, U150 or AO, and post-C381
audio from an original SSI-263AP Phasor with the same vectors. Measure source
level, filter peaks, width, decay, and route changes. Use those captures only
for terms absent from the schematic. Do not restore any phone-specific tweak.

The correct release claim before those captures is: two native SSI-263AP
digital models with an ideal SC-02 schematic charge model and original Phasor
card wiring, with analog hardware matching still pending.
