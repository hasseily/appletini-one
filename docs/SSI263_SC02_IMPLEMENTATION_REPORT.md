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
chiefly because the U85C/U68 reset path remains unresolved. The resonators,
section gain, output scale, and analog response are circuit-guided fixed-point
approximations. They are not a transistor-level or phase-by-phase nodal copy
of the LF356/CD4016 switched-capacitor circuit. Real-chip captures remain the
final test for clock straps, timing phase, spectra, level, and part tolerance.

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

The firmware uses one shared rational clock-enable source for the two SSI XCK
pins. The default pin rate is `3,579,545 / 2 = 1,789,772.5 Hz`; each AP core has
DIV2 high, so its effective time base is `894,886.25 Hz`. This is within the
data sheet's stated 800-1000 kHz range and follows its colorburst-derived
clock suggestion. The SSI clock does not change when software switches the
Phasor mode.

The XCK source uses the exact generated fabric rate, 133,333,344 Hz, and emits
evenly spaced one-cycle enables. It does not create an FPGA clock net. Build
parameters permit direct tests of a raw two-times-Apple-bus clock, a 1 MHz
effective clock, and the reconstruction's lower external-clock profile.

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
The PW3/U62 gate sets its polarity. The positive and negative amplitude banks
remain separate because their capacitor totals differ. Tests compare the full
HCC recurrence with an independent bit model and check the tap and gate
directly.

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
VOICE -> F1 -> F2 -> (+FRIC1) -> F3 -> F4 -> (+FRIC2) -> F5 -> output
```

FRIC1 enters through 1000 pF at the F2/F3 node. FRIC2 enters through
2700+1000 pF at the F4/F5 node. Their digital coupling gains keep that 1:3.7
ratio. The old implementation made FRIC1 and FRIC2 separate fixed resonators
and added them after F5. That bypassed the ROM-selected F3/F4 tract for many
P/F/K-class phones and made them share a loud broad hiss. The corrected graph
removes both extra resonators and injects noise at the two drawn nodes.

The capacitor audit also corrected two F2 values: frequency bit 0 is 280 pF,
for a 4260 pF full-scale total, and the resonance bank is 220, 430, 870, and
2300 pF, for a 3820 pF total.

The four filter-amplitude caps are 76, 150, 300, and 600 pF. Their gain is
applied after F5 and before closure/output. A final nodal model must also
include C172A at 2700 pF, C173A at 50 pF, and the U145 phase switches; the
current normalized gain table is still a calibrated stand-in for that node.

Each tract section now uses this stable two-pole low-pass form:

```text
y[n] = 2*r*cos(theta)*y[n-1] - r*r*y[n-2]
       + (1 - 2*r*cos(theta) + r*r)*x[n]
```

The unity-DC numerator takes the second-integrator-style output. The former
complex rotation exposed a first-integrator-like zero and made many vowels
peak around one common medium frequency. Static checks parse the exact RTL
tables and ROM, prove every pole stays inside the unit circle, check each code
ordering, and require distinct low formant ranges for E, I, A, EH, AE, AH, O,
and UH. The current radii still set estimated bandwidths; component totals set
the frequency order, but no phase-by-phase charge solve yet proves the exact
Q or gain.

At the usual `FILT=E8` test rate, the current calibrated tables span these
centres and widths:

| Section | Centre range | Estimated bandwidth |
| --- | ---: | ---: |
| F1 | 177-890 Hz | 367 Hz |
| F2 | 534-2536 Hz | 759-120 Hz from minimum to maximum resonance |
| F3 | 1335-3412 Hz | 431 Hz |
| F4 | 2225-4302 Hz | 495 Hz |
| F5 | fixed 4451 Hz | 625 Hz |

These are useful speech ranges, but the source caps do not prove the radius
values. Changing a radius would alter voiced formants and the SCH/J noise gain
at once, so this checkpoint keeps all five radii until a chip capture or nodal
solve supplies a better value.

Two registered multiplier lanes update five sections in 32 fabric clocks.
The fastest supported phase gap is 133 clocks, and an overrun latch covers
both chips in focused and card tests. The final filter-amplitude value applies
after F5, as shown on sheet 2.

The first route of this corrected graph exposed one 8.948 ns Phi0 input path:
fricative gain and sign, both FRIC1 coupling sums, and the final F2/F3 node sum
had become one 18-level chain. Phi0 now saves the resolved source, F2/F4 node
values, and both switch states. Three idle clocks then run the same saturated
adds one at a time. F3 first uses its saved input at engine clock 16 and F5 at
clock 28, so the values and 32-clock output time do not change. A focused test
changes every live source control after Phi0 and proves that both node sums
still use the saved sample.

### Reconstruction, level, and hardware fault trail

U52C clears a switched node during one phase. The board output sees a
reconstructed analog value, not a random switch phase. The RTL therefore
keeps the internal clear but samples the last completed F5 result at the
48 kHz board tick.

C381 AC-couples the chip output. A 255/256 digital high-pass gives a pole near
30 Hz at 48 kHz. Its input difference and feedback sum stay wider than the
24-bit tract; it applies the leak before one clamp. Full-scale reversal tests
guard against the early-clamp fault.

The hardware reports separated three faults:

- image 1 was almost silent except for weak T/S/Z/C-class traces because five
  serial quarter-scale gains suppressed voice far more than a short noise
  path;
- image 2 made P/F/C-class phones produce a strong exhaust while most voiced
  phones stayed weak and shared a medium-pitch robotic ring;
- the current source fixes remove the bipolar DC step, the serial graph removes
  the post-F5 noise bypass, the stop rule removes repeated stop noise, and the
  normalized all-pole sections separate the low vowel peaks.

The current direct focused vector reaches 11,179 PCM for phone 01 and 794 for
S, but that loop advances one noise edge per filter pair and must not set an
absolute balance. The final true 48 kHz card-rate sweep passes 103 checks and
gives these settled chip-level results:

| Phone | Peak | RMS | Occupied samples | Clips |
| --- | ---: | ---: | ---: | ---: |
| phone 01, voiced | 1,203 | 540 | 512/512 | 0 |
| S | 615 | 277 | 512/512 | 0 |
| F | 2,879 | 868 | 512/512 | 0 |
| SCH | 10,581 | 3,949 | 509/512 | 0 |
| J | 15,630 | 5,066 | 512/512 | 0 |
| Z | 6,565 | 1,667 | 512/512 | 0 |
| held P | 0 | 0 | 0/256 | 0 |
| P followed by HF | 4,973 | 1,110 | 456/512 | 0 |

These results reject a common fricative gain increase: SCH and J already use
much more of the range than S. The absolute coupling stays at its calibrated
208/768 Q14 values, with the physical 1:3.7 cap ratio and no phone-specific
boost. The matching card-output paths also have zero clips. Dual-socket windows
fill 494/512 then 512/512 samples per chip and channel, with all eight chip and
card clip counters at zero.

An independent core-plus-audio sweep passes 277 checks across all 64 phones.
It finds no PCM rails or unknown samples, proves the exact five-phone stop
mask, keeps S/F/SCH sustained, decays a voiced tract through held P from an F5
peak of 39,268 to 35, and gives a normal 256/256-sample P-to-HF onset.

This audio model is much closer to the source circuit than the SC-01 path or
the first SSI checkpoint. It still is not an exact analog copy. Exact radii,
section gain, output voltage, U68 behavior, and route handoff need a two-phase
nodal solve plus captures from a real SSI-263AP.

## Dual-chip Phasor integration

The design has two fixed `ssi263_voice` instances. Both set `REVISION_AP=1`
and `DIV2=1`. They share only the rational XCK enable; each retains separate
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
| Dual audio and state isolation | Implemented and simulated | 103 card checks: A5-only, A6-only, simultaneous stereo, no-overrun, zero clipping, and disable/re-enable; corrected-image listen pending |
| Switched-capacitor analog response | Approximate | Correct serial graph and injection nodes use stable all-poles and cap ordering, not a nodal solve |
| XCK and DIV2 straps on an original Phasor | Unproved | Default follows data sheet; continuity or scope capture needed |
| Digital level and spectrum guards | Implemented and simulated | 64-phone zero-rail sweep, ROM phone metrics, low-vowel peak separation, F/SCH tract split |
| Absolute voltage and analog tolerance | Unproved | AO/post-C381 captures from real SSI-263AP parts needed |

The U85C/U68 reset term described above is the one known digital-circuit gap;
it prevents an unconditional claim that every internal phase matches a die.

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
4. Build a double-precision two-phase nodal model from the LF356, CD4016, and
   capacitor network. Solve Phi0 and Phi1 charge transfer for each section,
   including fixed F5 and both noise-injection nodes.
5. Compare impulse response, peak frequency, bandwidth, decay, DC bias, and
   level for every four-bit code against SPICE and real captures.
6. Quantize the proved state equations and reuse the current scheduled-MAC
   structure. Keep one state set per chip and share only arithmetic.
7. Set one chip-level output scale from measured voltage. Do not restore
   phone-name gain, presence, attack, or stop-burst rules.
8. Run simultaneous two-chip phrases and confirm channel isolation, request
   independence, clipping headroom, and FPGA timing on hardware.

Until those captures and the nodal model agree, the right release claim is
"native SSI-263 digital implementation with a circuit-guided audio model,
hardware validation pending." It should not be called a perfect analog copy.

## Verification and firmware

The first clean full build to clear the release margin is
`20260824T023831Z-941d6531-full` at commit
`941d6531c11d68468bb7c2efc7cc56808eac59c9`. It used no incremental
checkpoint and recorded:

- setup WNS `+0.316 ns`, hold WHS `+0.058 ns`, and pulse-width WPWS
  `+0.265 ns`;
- zero setup, hold, pulse-width, route, unconstrained-endpoint, and missing
  constraint faults;
- route and bus-skew status `PASS`, with bus-skew slack `+5.948 ns`;
- a clean Git tree and an exported, hash-recorded bitstream and XSA.

The build also guards two timing fixes outside speech that the larger SSI
design exposed. It requires all 16 width-parallel frame-reader FIFO RAM blocks
to belong to their placement block. It also requires 36 independent vTW
shadow RAM blocks with no depth cascade. These changes preserve the tested
FIFO and shadow-memory behavior; they do not relax a speech path or hide one
with a false or multicycle path. The Apple bus direction outputs keep their
real raw-PHI0 limit as a separate constraint.

This qualifying route proves that the full design can clear the normal margin.
For this F0.9.99 speech checkpoint, the user set a temporary routed-WNS gate of
`+0.100 ns` and asked for one clean full build only. Keep that first route if
it passes; do not spend a second full build on duplicate timing evidence while
hardware speech work remains.

Full build `20260824T055427Z-a335e4e2-full` tested the corrected serial tract
before the input retiming. It routed with no failed nets and passed hold,
pulse-width, bus-skew, and constraint checks, but setup WNS was `-1.058 ns`.
The image was rejected and no firmware was packaged from it. Its ten worst
reported paths all ended at an SSI F3 input. It also found a `-0.791 ns` live
slot-decode path into SSI core write enables. The source now splits the F3/F5
math across the unused phase gap and holds each SSI socket select from SERVE
through DATA. A fresh out-of-context synthesis of one audio core now reports
`+0.976 ns` WNS at 7.500 ns, with five DSP48 blocks and a 3.318 ns worst data
path. The accepted package must still come from a full-card run that clears
the temporary `+0.100 ns` gate.

The final branch gate includes:

- native ROM/reference tests;
- native digital-core XSim;
- XCK rational-enable XSim;
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
- firmware packaging from the named accepted timing run, not from a stale
  project bitstream;
- source and synthesis-log checks that no SC-01/Votrax implementation enters
  the image.

The test firmware version is `F0.9.99`. The package must use an accepted
timing run's exact XSA and bitstream; the root `FIRMWARE.BIN` and any Vitis IDE
fallback bitstream are not source evidence and must not replace it.

The first `F0.9.99` hardware run proved card detection, VIA/request handling,
and Phasor mode, but speech was inaudible apart from weak fricatives. The next
image made P/F/C-class noise far too strong and left most voiced phones weak
with a common medium-pitch ring. Those two listen tests led to the source,
tract, stop, formant, and output fixes above. Hardware listening and real-chip
matching remain pending for the corrected `F0.9.99` image.
