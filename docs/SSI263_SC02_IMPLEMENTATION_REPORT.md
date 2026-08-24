# Dual SSI-263 / SC-02 Phasor implementation report

## Result and scope

This branch replaces the Phasor speech path with two independent SSI-263AP
cores. A5 selects the secondary channel-A chip and A6 selects the primary
channel-B chip. Each chip has its own registers, request state, timers,
transition state, source state, filter state, and audio output. The active
build inputs no longer include the SC-01 core, SC-01 coefficient ROM,
SSI-to-SC-01 phone map, or Votrax interface.

ROM contents and addressing, plus the public pitch, filter, frame, and
duration laws, are exact to the supplied sources. Register, D7/A-R, selector,
transition, excitation, and Phasor routing are source-derived synchronous
models checked in simulation. They are not yet proved against SSI silicon,
chiefly because the U85C/U68 reset path remains unresolved. The formant engine
now solves the ideal charge transfer drawn for every switched-capacitor
section. LF356 bandwidth, CD4016 resistance, stray capacitance, output voltage,
and part tolerance still need circuit and real-chip checks.

## Sources and what each one proves

The implementation uses each source only for the part it can prove:

- The SSI-263A data sheet defines the register format, bus timing, D7 read,
  open-collector A/R output, DR modes, CTL and PD/RST behavior, public timing
  laws, pitch target split, and filter clock law.
- The supplied `sc-02_Final_Schematic_V1.00.pdf` defines the reconstructed
  digital divider chains, selector scan, transition stores, source gates,
  source recurrences, capacitor selections, and filter topology.
- The supplied `ssi263a.bin` defines all 64 phones and eight selector bytes per
  phone. Its active 512-byte region is used without a phone translation.
- `mb-audit` results and AppleWin's Phasor decode define card-visible address
  aliases, read priority, mode changes, and request routing that original-card
  tests have exercised.
- A physical Phasor and real SSI-263AP must settle facts that the documents do
  not show, chiefly the XCK and DIV2 straps, analog transfer functions, output
  level, and process-dependent timing.

The SC-02 PDF documents a discrete 2020 reconstruction. Its board fixes,
external-clock changes, RAM substitutions, output amplifiers, and speaker
driver are not assumed to be part of the SSI die.

Public reference links used in the audit:

- [SSI-263A data sheet](https://downloads.reactivemicro.com/Electronics/Speech/SSI-263A%20Data%20Sheet%20v2.pdf)
- [Applied Engineering Phasor manual](https://downloads.reactivemicro.com/Apple%20II%20Items/Hardware/Phasor/Phasor%20Manual.pdf)
- [AppleWin SSI-263 card behavior](https://github.com/AppleWin/AppleWin/blob/master/source/SSI263.cpp)
- [mb-audit SSI-263 hardware tests](https://github.com/tomcw/mb-audit/blob/main/chip-ssi263.a)

## Audit of the removed implementation

The old path was useful as a speech substitute, but it was not a native
SSI-263 model:

- It translated each SSI phone to an SC-01 phone and drove an SC-01 digital
  core and SC-01 coefficient tables.
- It advanced the speech controls from the 48 kHz audio path through a guessed
  20 kHz update rate instead of using XCK, DIV2, SLOWCLK, RATECLK, and the
  filter divider.
- It used SC-01 ROM fields and interpolation instead of the supplied SSI ROM,
  selector scan, seven transition stores, and lower-ROM control bits.
- It treated `FILT=FF` as silence. The SSI law makes `FF` the fastest filter
  setting: `XCK / (2 * (256 - FF))`.
- It used phone maps, hand-tuned presence, gain, attack, and other sound fixes
  that have no SSI-263 circuit source.
- It populated A5 as an SC-01/Votrax path and A6 as the only SSI-263AP path.
- It made synthetic VIA interrupt-flag pulses instead of presenting each
  active-low A/R level to its VIA CA1 input.
- It summed the two speech paths to mono before the left and right outputs.
- Its read-drive expression suppressed A5 and A5+A6 native reads.

Those points made further tuning converge on an SC-01-based approximation.
They also made it hard to tell whether a mismatch came from the Phasor bus,
the SSI digital circuit, or the audio model.

## Native ROM and transition path

The checked-in ROM has these source properties:

- input size: 2,048 bytes;
- active bytes: `000-1FF`;
- unused bytes: `200-7FF`, all zero;
- CRC32: `CC0A72EE`;
- SHA-256:
  `9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941`.

The address is `{phone[5:0], selector[2:0]}`. The upper nibble is the target,
except selector 4 uses the host amplitude. The implemented stores are:

| Selector | State or action |
| ---: | --- |
| 0 | F1 target and active-low voiced-class bit PW0 |
| 1 | F2 target and active-low fricative-class bit PW1 |
| 2 | F2 resonance plus PW and FRIC switch controls |
| 3 | shared F3 and F4 target |
| 4 | filter-amplitude target from the host amplitude |
| 5 | voiced-amplitude target |
| 6 | fricative-amplitude target |
| 7 | scan slot only; no parameter write |

One articulation event starts a seven-slot sweep. Each selected store moves by
one count toward its target when its slot passes. A host write on the same
fabric edge owns the data path and consumes the old sweep slot. The F3 and F4
sections share selector-3 state as shown on the reconstruction.

The lower-ROM values present in the source are `0, 1, 4, 6, 8, A, C, E`.
PW0 and PW1 cross the two source paths and are active low: voiced is
`!PW0 && VOICE_AMP != 0`, while fricative is
`!PW1 && FRIC_AMP != 0`. Thus `01` selects voiced, `10` selects fricative,
and `00` permits a mixed phone when both amplitude targets are nonzero. This
is required for Z, J, V, and voiced TH; treating each bit as a same-slot,
active-high enable muted both sources for those four phones. U52C CLOSURE is
U49 terminal count gated by U43B Q. The RTL keeps it as the matching internal
filter-phase pulse; it is neither a pitch-terminal signal nor a phone-name
flag.

## Registers, response, and reset

The core captures address and data when the selected write condition ends.
Registers 4 through 7 alias the filter-frequency register. Reads expose only
D7; D0-D6 remain the Apple bus value at the card boundary.

The request rules now match the documented interface:

- writes to registers 0, 1, and 2 clear the pending request;
- a register-3 write clears it only when CTL is one;
- filter writes and reads do not clear it;
- a response boundary always sets internal D7;
- DR=00 disables the external A/R drive but leaves D7 and the prior response
  and inflection modes intact;
- a phone repeats while pending and does not restart when software
  acknowledges it;
- the next boundary requests service again;
- hard card reset wins first, AP PD/RST wins over a colliding host write, and
  an accepted host write wins over a colliding response boundary.

Each virtual socket is fixed to the AP revision. Apple RESET drives PD/RST,
which powers the AP part down, clears D7 and A/R, and retains the other
registers. An AP ignores an entire write that ends while PD/RST is low. The
separate P-revision behavior remains in the focused chip test, but the Phasor
does not instantiate a P part.

## Clock findings

The well-known Phasor clock doubling applies to its AY chips in native mode.
The AY base enable comes from the Apple bus cadence. Native mode inserts one
extra AY enable after each base enable, so the AY cadence is twice the
Mockingboard cadence, about twice the Apple 1 MHz bus rate. This is not the
133 MHz fabric clock, and it does not prove that SSI XCK changes with card
mode.

The firmware synchronizes the physical Apple Q3 pin into the fabric domain and
emits one shared XCK enable for each observed rising edge. Both SSI sockets see
that same card clock. Each AP core keeps DIV2 high, so a nominal
`2,045,454.29 Hz` Q3 input gives an effective `1,022,727.14 Hz` time base. The
SSI clock does not change when software switches the Phasor mode.

This changes the card input, not the SSI circuit. The reconstruction's local
crystal path divides `3.579545 MHz` by four and reaches `894,886.25 Hz` at
FASTCLK. Its explicit external path accepts TCLKIN and divides it by two before
that same node. Apple Q3 on TCLKIN therefore reaches `1,022,727.14 Hz` without
removing a drawn divider. The standard Mockingboard uses the numerically
equivalent choice of PHI2 at XCK with DIV2 low. The Phasor board has no separate
speech crystal, so the slot-clock profile is the better card model.

The former profile made the common programming-guide inflection `I=$A80`
settle at `79.4466 Hz`. The Q3 profile makes it `90.7961 Hz`, an exact `8/7`
rise. This corrects the measured fundamental without changing the SSI circuit.
It did not make the rejected all-pole listening checkpoint sound right: that
image still sounded low, monotonous, and muffled because its tract response
was wrong. The drawn FASTCLK-to-SLOWCLK divide by four, the 12-bit pitch counter,
U62's final divide by two, transition timing, ROM data, and register packing
remain unchanged.

The XCK source uses two fabric-clock synchronizer flops and a synchronized
rising-edge detector. It does not create an FPGA clock net or a second nominal
oscillator. The focused bench varies Q3 phase and duty cycle, checks one enable
per captured rise, and proves that a held level cannot retrigger the clock.

These public laws are implemented in effective XCK ticks:

```text
pitch_hz      = XCK_effective / (8 * (4096 - I))
filter_hz     = XCK_effective / (2 * (256 - FF))
frame_ticks   = 4096 * (16 - R)
phoneme_ticks = frame_ticks * (4 - D)
```

The reconstruction divides FASTCLK by four before the 12-bit pitch counter.
Its raw VOICECLK terminal therefore occurs every `4 * (4096 - I)` effective
ticks. U62 and the rising-transition detector divide that by two, which gives
the public `8 * (4096 - I)` glottal period. The source tests cover both the raw
and final cadence so the divider cannot be applied twice.

The reconstructed rate chain gives:

```text
RATECLK rising edge = 128 * (16 - R) effective ticks
U94 articulation    = 256 * (16 - R) * (8 - T) effective ticks
U66 inflection step = 128 * (16 - R) * (8 - S) effective ticks
```

Here `T` is the articulation setting and `S` is I5:I3. The transitioned pitch
state converges on `{I10:I6, 3'b000}`; I5:I3 set its movement pace, while I11
and I2:I0 remain immediate.

The data sheet says articulation does not depend on speech rate, while the
reconstruction clocks U94 from RATECLK/2 and therefore makes it rate-dependent.
The RTL follows the supplied circuit. A real-chip transition trace should
settle this conflict.

## Excitation and audio path

The native audio block has no SC-01 phone map, SC-01 coefficient table, or
phone-name sound fix. It uses the source bits, values, and switch controls
from the SSI ROM and transition engine.

### Source circuits

The voiced source follows U59, U62, U61, U34, and U60. The prior model drove
`+A` and `-A`, which made a two-amplitude step and a large DC term. The drawn
source is unipolar: it is zero while U60 rests at terminal count and `+A`
during the 15-count pulse. The focused test checks the load, count, terminal
hold, and rising-edge-only reload.

The fricative source follows U41C, U75, and the four HCC4006 stages. Its output
tap is the inverted U73 D4+5 output, not the adjacent D3+4 tap used before.
The PW3/U62 path gates that tap. The U157 and U152 amplitude banks remain
separate because their capacitor totals and phase-reset behavior differ. Tests
compare the full HCC recurrence with an independent bit model and check the
tap, gate, and both source nodes directly.

The programming guide says B, D, P, T, and K make no output unless another
phone follows. In the ROM, `PW2 && !PW3` selects exactly those five phones.
The RTL mutes their held source and lets the next phone's live source pass
through the tract while its values move away from the stop target. It does not
add a fixed, repeated, or articulation-timed burst. This rule removes the
standalone P/T/K exhaust heard from the first F0.9.99 image.

This stop rule is guide-correct but not yet a proved gate copy. The drawn
U68/AMPCT and U85C paths do not yield one clear stop envelope: the shown U85C
polarity would hold U62 reset for most U68 counts, while a settled pure stop
leaves the drawn noise gate open. The delayed U183 route latch also affects
whether T keeps its FRIC2 path during a following phone. A real-chip trace or
a corrected net list must settle U68, U85C, and the T-to-HF route handoff.

### Filter graph

The schematic gives one serial tract, not three output paths:

```text
VOICE -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2) -> U146 -> U148
```

FRIC1 comes from U157 through switched C143, 1000 pF, into F2's second
integrator. Its exact term is `-(1000/6800)*U157`. U156B grounds the selected
input bank and U156C resets U157 in Phi1, so U157 regenerates
`-(Cselected/3900)*HCC` on every Phi0. It is not an HCC-edge queue. C143 has no
cross-pair route history or reconnect impulse because both of its plates reset
in Phi1.

FRIC2 comes from the distinct U152 node into F5's first integrator. C150 is
1150 pF and remains connected. C151 is 3700 pF and is controlled by U159C.
The two paths keep separate histories, and only C151 holds its source-side
charge while open. U152 has no phase reset, so it changes only on an HCC edge:

```text
U152[n] = U152[n-1] - (Cselected/3900)*(HCC[n] - HCC[n-1])
```

The FRIC_AMP code used for U152 is the code held at that edge. A code change
while HCC stays fixed does not change U152. The old implementation made FRIC1
and FRIC2 separate fixed resonators and added them after F5. The new graph
removes those resonators and applies every noise term at its drawn node.

The capacitor audit corrected three earlier readings. F2 frequency bit 0 is
280 pF. Its resonance bank is 220, 430, 870, and 1800 pF. F4 frequency bit 0
is 200 pF, and F4 always has the fixed 1200+470 pF C155 branch.

The four filter-amplitude caps are 76, 150, 300, and 600 pF. C172 is 2700 pF
and C173 is 50 pF. The implemented ideal U146 recurrence is:

```text
out[n] = (2700/2750)*out[n-1]
         + (Cselected/2750)*(F5[n-1] - F5[n])
```

U145D copies the completed U146 hold into C100/U148 only on CLOSURE. An open
switch holds the prior reconstructed value; it does not clear either node.

Each section now keeps the first-integrator charge `p` and second-integrator
output `y`. The shared state form is:

```text
p[n] = alpha*p[n-1] + a*(y[n-1] - u[n])
       + g*(v[n-1] - v[n])
y[n] = y[n-1] - b*p[n]
```

`u` is the serial input. F1 also uses its prior voice input for `v`, with
`g=a`. F3 uses the direct C127 F1 path for `v`. The other three sections set
`g=0`. The exact unrounded ratios are:

| Section | `alpha` | `a` | `b` | `g` |
| --- | ---: | ---: | ---: | ---: |
| F1 | 11500/11700 | 2700/11700 | (250+F1 bank)/11500 | 2700/11700 |
| F2 | 6800/(7000+RES bank) | 4700/(7000+RES bank) | (500+F2 bank)/6800 | 0 |
| F3 | 4700/4900 | 3900/4900 | (820+F3 bank)/4700 | 2000/4900 |
| F4 | 4300/4500 | 4700/4500 | (1670+F4 bank)/4300 | 0 |
| F5 | 3450/3730 | 4700/3730 | 4700/3450 | 0 |

The RTL stores rounded Q14 ratios. Static tests derive every table again from
the capacitor values, test all 256 F2 frequency/resonance pairs for stable
poles, and test the other sections for stable paired poles. Higher F2 RES code
loads its first integrator more, which lowers its pole radius and broadens F2.
No guessed radius or cosine table remains.

An eight-vowel harmonic check uses Q3, `FILT=E9`, the ROM codes, the 15-phase
voice pulse, the exact section transfer functions, and U146's pole and delta
zero. Before the uncertain C381 boundary it records a mean centroid of
736.866 Hz, 10.586 percent of power from 30-500 Hz, and 8.882 percent from
1-4 kHz. Fixed source-based bounds accept this capacitor model and reject the
low, uniform response without using an audio sample or a phone-specific gain.

Two registered multiplier lanes update five sections and U146 in 34 fabric
clocks. F1-F4 each use six clocks. F5 uses seven clocks so the always-on C150
term and switched C151 term join the same wide charge sum before one round. A
separate clock registers `old_F5-new_F5` before U146 uses two clocks. The
fastest supported phase gap is 133 clocks, and an overrun latch covers both
chips in focused and card tests.

The first charge-engine simulation found a scheduler ordering fault: stage 2
read `engine_charge_next` before the same combinational block calculated it.
The second integrator therefore used a stale charge from the prior section.
That fault left the first F1 output at zero, moved one section's charge into
the next section, and drove later sections to their rails. The scheduler now
calculates all values from registered products before it selects the next DSP
operands. Directed vectors check both `p` and `y` for F1, F3, C143/F2, and
C150/C151/F5.

Phi0 now saves both source nodes, both route switches, FL_AMP, and all F1-F4
frequency and F2 resonance codes. A control update during the 34-clock run
cannot split one physical transfer across two phone states. Route tests keep
FRIC1 out of F1, apply it only to F2's second integrator, and keep FRIC2 out of
F1-F4.

### Reconstruction, level, and hardware fault trail

U146 finishes during Phi1. CLOSURE then copies that completed internal hold to
C100/U148 at the Phi1-to-Phi0 boundary. The 48 kHz board tick reads the held
U148 value. Phone end stops new source energy but does not mute this post-U148
path, so the tract and output holds can decay. Powerdown remains the explicit
PCM mute.

C381 AC-couples the chip output. The chip schematic does not show its external
load, so it does not fix the analog pole. The current 255/256 high-pass is a
boundary estimate near 30 Hz at 48 kHz, not a chip-exact term. Its input
difference and feedback sum stay wider than the 24-bit tract; it applies the
leak before one clamp. Full-scale reversal tests guard against an early clamp.

Hardware tests and the new model separated four faults:

- image 1 was almost silent except for weak T/S/Z/C-class traces because five
  serial quarter-scale gains suppressed voice far more than a short noise
  path;
- image 2 made P/F/C-class phones produce a strong exhaust while most voiced
  phones stayed weak and shared a medium-pitch robotic ring;
- the Q3 correction raises the fundamental by the source-derived 8/7 ratio,
  but the rejected `cdb43efd` all-pole image sounded worse: low, monotonous,
  and muffled, while its five guessed low-pass sections put about half of
  voiced power below 500 Hz;
- the charge solve removes those guessed poles, and the scheduler-order fix
  stops the second integrator from reading a prior section's charge;
- a later schematic audit found that an edge-only U157 model dropped every
  repeated high HCC sample after the first pair. U157 now regenerates on each
  Phi0, while U152 alone keeps edge charge;
- the same audit removed a post-U148 `phone_active` mute that cut valid output
  tails and made transitions abrupt.

The current focused vectors reach PCM peaks of 4,342 for the voiced path and
10,703 for the fricative path, with no clip in that bench. These figures guard
loss of output and gross rail faults; they do not set the real voice/fricative
balance. The normalized HCC drive is 0 or 2^16, and `LINE_OUTPUT_SHIFT=3` maps
the ideal AO state to PCM. Neither absolute scale comes from the schematic.
They remain explicit calibration items for a real AO capture. They do not
change an internal capacitor ratio, state equation, pitch law, or phone code.

This audio model follows the source circuit at the ideal switched-capacitor
level. It still is not an exact analog copy. U150 pulse swing, LF356 settling,
CD4016 on-resistance, stray capacitance, output voltage, U68 behavior, C381's
external load, and route handoff need circuit checks plus captures from a real
SSI-263AP.

## Dual-chip Phasor integration

The design has two fixed `ssi263_voice` instances. Both set `REVISION_AP=1`
and `DIV2=1`. They share only the physical-Q3 XCK enable; each retains separate
core, request, transition, excitation, filter, and audio state. A5 maps to the
secondary channel-A chip, VIA0 CA1, and left audio. A6 maps to the primary
channel-B chip, VIA1 CA1, and right audio.

The card implementation now has these rules:

- each SSI write select is decoded at SERVE and held through DATA, while its
  register number and data remain live for the same Apple cycle;
- A5 writes the secondary/channel-A SSI; A6 writes the primary/channel-B SSI;
- A5+A6 writes both chips;
- in native mode, A5 and A6 reads expose the matching D7 status;
- an A5+A6 read uses A5 D7 and still asserts the read-drive enable;
- Echo+ hides SSI reads and writes without clearing running chip state;
- Mockingboard mode sends each A/R level to its matching VIA CA1 input;
- native mode sends the OR of the two enabled A/R levels to Apple IRQ;
- acknowledging one chip cannot clear the other chip's request;
- A5 speech goes left and A6 speech goes right without a mono pre-sum;
- disabling Slot 4 hard-resets both chips, releases IRQ and read drive, and
  mutes both speech and PSG output before a clean re-enable;
- VIA PCR state no longer selects a hidden Votrax path or alters VIA0 AY
  reset and drive behavior.

An exhaustive software model checks all 256 slot offsets in Mockingboard,
native Phasor, and Echo+ modes. It also checks all D7 combinations, dual-read
priority, separate CA1 states, and native IRQ release only after both requests
clear. The card-level XSim also drives both real filter engines, proves
A5-left/A6-right and simultaneous stereo output, checks both overrun flags,
and disables and re-enables Slot 4 during a live native read and IRQ.

## Fidelity status

| Area | Status | Evidence or remaining gap |
| --- | --- | --- |
| ROM identity and address | Exact to supplied binary | Hash, all 512 active bytes, all 64 by 8 lookups |
| Register and D7/A-R rules | Implemented and simulated | Write-end, aliases, DR modes, repeat, collisions, P/AP tests |
| Public pitch, filter, frame, and duration laws | Implemented and simulated | Boundary and full-range timing tests |
| Reconstructed selector and transition logic | Synchronous state equivalent | All selector targets, low-ROM classes, and cadence settings tested |
| Voiced and noise digital recurrences | Source-derived and cycle-tested | Raw/final pitch and sustained noise tests pass; unresolved U62 reset phase can alter excitation and the noise gate |
| Held-stop behavior | Guide-correct checkpoint | B/D/P/T/K mask, held decay, and P-to-HF pass; U68/U85C and T route handoff remain unproved |
| Phasor address and request routing | Implemented and exhaustively modeled | 256 offsets per mode and two-chip request model |
| Dual audio and state isolation | Implemented and simulated | 105 card checks: A5-only, A6-only, simultaneous stereo, no-overrun, zero clipping, and disable/re-enable; corrected-image listen pending |
| Ideal switched-capacitor charge response | Implemented and simulated | Exact capacitor-ratio state equations, F1/F3 direct paths, F2 load, all-code stability, and spectral guard |
| XCK and DIV2 straps on an original Phasor | Effective profile implemented; exact board trace unproved | Q3 plus DIV2 gives the standard Apple-card time base and follows the reconstruction's external path; continuity or scope capture remains useful |
| Digital level and spectrum guards | Implemented and simulated | Exact eight-vowel harmonic bounds, 64-phone zero-rail sweep, ROM phone metrics, and route isolation |
| Nonideal analog response and absolute voltage | Unproved | LF356/CD4016 circuit checks and AO/post-C381 captures from real SSI-263AP parts needed |

The U85C/U68 reset term described above is the one known digital-circuit gap;
it prevents an unconditional claim that every internal phase matches a die.

The final pre-build run passed 27 ROM/reference tests, the native core, the
physical-Q3 edge bench, the focused charge/audio bench, 14 Phasor source tests,
all 64 phones with 277 checks and no clipped or unknown samples, and the
dual-chip bench with 105 checks.

## What should be done next for closer analog fidelity

The next work should improve evidence, not add phone-specific tuning:

Two test levels remain. Appletini validation must cover boot, both chip
selects, simultaneous stereo, left/right routing, CA1 and native IRQ, all
three card modes, reset, clipping, and long-run stability. Original SSI-263AP
and Phasor comparison must cover XCK/DIV2, A/R and D7 timing, all phones,
filter extremes, spectra, level, part tolerance, and the U85C/U68 behavior.

1. Measure an original Phasor's XCK and DIV2 pins in Mockingboard and native
   modes. Record raw XCK, effective clock, and phase continuity across mode
   changes.
2. Capture A/R, D7, XCK, and audio for the same register vectors. Include all
   DR modes, every rate, every articulation value, immediate and transitioned
   inflection, filter values `00`, `01`, `7F`, `E9`, `FE`, and `FF`, and reset
   collisions.
3. Capture isolated phones at fixed amplitude and 96 or 192 kHz. Include E,
   A, AE, AH, and O; F and SCH on FRIC1; S and TH on FRIC2; J and Z with mixed
   excitation; held P/T/K; and stop-to-voice and stop-to-noise transitions.
   Record AO before C381 and the post-C381 signal, then repeat enough vectors
   on a second part to expose chip spread.
4. Add LF356 bandwidth, CD4016 on-resistance, and listed stray capacitance to a
   circuit model around the proved ideal charge equations. Do not replace a
   drawn capacitor ratio with a sound adjustment.
5. Compare impulse response, peak frequency, bandwidth, decay, DC bias, and
   level for every four-bit code against that model and real captures.
6. Quantize only a nonideal term that both the circuit model and captures
   support. Keep one state set per chip and share only arithmetic.
7. Set one chip-level output scale from measured voltage. Do not restore
   phone-name gain, presence, attack, or stop-burst rules.
8. Run simultaneous two-chip phrases and confirm channel isolation, request
   independence, clipping headroom, and FPGA timing on hardware.

Until those captures and the circuit model agree, the right release claim is
"native SSI-263 digital implementation with an ideal schematic charge model,
hardware validation pending." It should not be called a perfect analog copy.

## Verification and firmware

### Historical build evidence

Full build `20260824T023831Z-941d6531-full` at commit
`941d6531c11d68468bb7c2efc7cc56808eac59c9` showed that the larger design could
clear the normal route margin without an incremental checkpoint. It recorded
setup WNS `+0.316 ns`, hold WHS `+0.058 ns`, pulse-width WPWS `+0.265 ns`, and
no route, constraint, or bus-skew fault. This is historical timing evidence;
it predates the current charge engine and does not qualify current firmware.

The later Q3/all-pole build `20260824T075008Z-cdb43efd-full` at commit
`cdb43efd062731ec5b215debac755ab0d4312cf6` also met its temporary timing gate,
with setup WNS `+0.243 ns`. Hardware listening rejected its firmware. Speech
sounded much worse: low in pitch, monotonous, and muffled. That image uses
the discarded guessed all-pole tract, must not be treated as current firmware,
and must not be used for the next listen test.

Full build `20260824T055427Z-a335e4e2-full` tested an intermediate serial tract
and failed setup at `-1.058 ns`. It was rejected and produced no firmware. Its
worst paths led to the later Phi0 input retime and held SSI socket selects.

The historical builds also exposed two timing fixes outside speech. All 16
width-parallel frame-reader FIFO RAM blocks must stay in their placement block,
and the 36 vTW shadow RAM blocks must remain independent with no depth cascade.
These fixes do not relax a speech path or hide one with a false or multicycle
constraint. The Apple bus direction outputs retain their raw-PHI0 limit.

### Current source and pending firmware

The current audio source uses the exact capacitor-ratio charge equations above,
not the rejected all-pole tables. Each chip keeps independent `p`, `y`, input,
and route history. Two registered multiplier lanes update the five sections
and U146 in 34 fabric clocks. The final 7.500 ns isolated out-of-context route
mapped one audio instance to five DSP48E1 blocks and met setup with WNS
`+0.113 ns`. It includes the final U157, U146, output-tail, control snapshots,
and registered F5 output delta. Because this isolated check has no board
input/output delays or `HD.CLK_SRC`, it screens internal audio paths but does
not replace the one full-card timing route.

The final branch gate includes:

- native ROM/reference tests;
- native digital-core XSim;
- physical-Q3 synchronizer and edge-enable XSim;
- excitation/filter XSim;
- all-64-phone PCM rails, unknown, stop, sustained-source, RMS, occupancy, and
  representative level checks;
- mixed-phone source-class checks for the active-low PW0/PW1 decode;
- closure/reconstruction checks that keep the filter carrier out of PCM;
- exhaustive Phasor source and address tests;
- all relevant repository regressions listed in the validation record;
- fresh Vivado synthesis and implementation;
- one clean full build from the final commit with routed WNS at or above
  `+0.100 ns`;
- firmware packaging from the named final passing timing run, not from a stale
  project bitstream;
- source and synthesis-log checks that no SC-01/Votrax implementation enters
  the image.

The test firmware version remains `F0.9.99`. No firmware made from the current
charge engine has yet passed the clean full-build and package gate. Run exactly
one clean full build from the final source, keep it if it passes the temporary
`+0.100 ns` routed-WNS floor, and package from that run's exact XSA and
bitstream.

```text
Final F0.9.99 firmware path: pending
Final routed WNS: pending
Final firmware SHA-256: pending
```

Earlier `F0.9.99` hardware runs proved card detection, VIA/request handling,
and Phasor mode. They did not validate the current tract: one was almost silent,
one made P/F/C-class noise far too strong, and the Q3/all-pole image sounded
low, monotonous, and muffled. The next image must contain the charge engine,
the scheduler-order and timing fixes, both independent SSI-263 instances, and
no SC-01 path. Hardware listening and real-chip analog matching remain pending.
