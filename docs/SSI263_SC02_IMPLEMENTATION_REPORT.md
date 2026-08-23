# Dual SSI-263 / SC-02 Phasor implementation report

## Result and scope

This branch replaces the Phasor speech path with two independent SSI-263AP
cores. A5 selects the secondary channel-A chip and A6 selects the primary
channel-B chip. Each chip has its own registers, request state, timers,
transition state, source state, filter state, and audio output. The active
build inputs no longer include the SC-01 core, SC-01 coefficient ROM,
SSI-to-SC-01 phone map, or Votrax interface.

The register, bus, request, ROM, and reconstructed digital timing paths now
follow SSI-263 sources directly. The audio path follows the SC-02 source and
filter controls, but its resonators are still a stable fixed-point model of the
analog network. It is not a transistor-level or phase-by-phase nodal copy of
the LF356/CD4016 switched-capacitor circuit. Real-chip captures remain the
final test for clock straps, spectra, level, and part tolerance.

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
| 0 | F1 target and fricative source flag |
| 1 | F2 target and voiced source flag |
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
They now drive the held voiced, fricative, PW, and FRIC controls. U52C CLOSURE
is U49 terminal count gated by U43B Q. The RTL exposes it as the matching
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
It does not prove that SSI XCK changes with card mode.

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

The native audio block has no SC-01 phone map or coefficient table. It uses:

- the U59 VOICECLK event, U62 toggle, U61 sampling, and U60 voice shape;
- the U41C noise clock gate, U75 counter, and HCC4006 recurrence;
- independent positive and negative fricative amplitude paths;
- separate FRIC1 and FRIC2 paths;
- persistent F1, F2, F3, F4, fixed F5, FRIC1, and FRIC2 section state;
- F2 resonance as a real pole-radius control;
- one filter phase source shared by every section;
- a held chip-side output sampled at the existing 48 kHz board rate.

The noise clock is not the edge of the held PW3 value. The traced gate is
`~(PW3 & U62_/Q) & ~SEL1 & ~FRIC_AMP_ZERO`; its rising edges clock U75 and the
HCC4006 path. The regression test requires many HCC shifts while one
fricative phone keeps PW3 high.

One digital detail remains uncertain in the reconstruction. U85C combines an
U104C term with U68 carry to reset U62, but the shown polarity self-locks for
one PW3/U62 state and the U68 carry state has not yet been transcribed. The
RTL therefore initializes U62 only on hard reset and then lets it free-run. It
does not invent a CTL, phone-active, or PD/RST reset. `FRIC_AMP_ZERO` currently
uses the held fricative-amplitude code being zero. A real-chip logic trace or
a corrected schematic net is needed to close this point.

The current filter uses seven stable fixed-point resonators. Source capacitor
totals set each code ordering, F2 resonance changes decay, and FILT changes the
phase rate for every section. A 53-clock scheduler performs the seven section
updates, a registered output sum, and output gain. Vivado maps the two
registered RTL product lanes to four DSP48E1 cells per chip. The default
fastest phase gap is 149 fabric clocks; the fastest intended comparison profile
still leaves 133 clocks. An overrun latch is checked in simulation.

The first scheduler draft passed XSim but failed a 7.500 ns out-of-context
synthesis check with `WNS=-5.105 ns`. Registering every multiply result before
rotation, saturation, addition, or state commit changed the same check to
`WNS=+0.602 ns` on `xc7z020clg484-2`. That result is only a local timing check;
the full placed-and-routed design remains the release gate.

This is a large gain over the SC-01-based sound path, but it remains the least
proved part. Pole angles, radii, section drive, output scale, and the mapping
from capacitor ratio to a discrete resonator are engineering estimates. They
do not yet solve charge transfer on each Phi0/Phi1 switch state.

## Dual-chip Phasor integration

The card implementation now has these rules:

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
| Voiced and noise digital recurrences | Synchronous state equivalent | Raw/final pitch and sustained noise sequence tests |
| Phasor address and request routing | Implemented and exhaustively modeled | 256 offsets per mode and two-chip request model |
| Dual audio and state isolation | Implemented and simulated | A5-only, A6-only, simultaneous stereo, no-overrun, and disable/re-enable checks; hardware listen test pending |
| Switched-capacitor analog response | Approximate | Stable resonators use source controls and capacitor ordering, not a nodal solve |
| XCK and DIV2 straps on an original Phasor | Unproved | Default follows data sheet; continuity or scope capture needed |
| Absolute level, spectrum, and analog tolerance | Unproved | Real SSI-263AP captures needed |

The U85C/U68 reset term described above is the one known digital-circuit gap;
it prevents an unconditional claim that every internal phase matches a die.

## What should be done next for closer analog fidelity

The next work should improve evidence, not add phone-specific tuning:

1. Measure an original Phasor's XCK and DIV2 pins in Mockingboard and native
   modes. Record raw XCK, effective clock, and phase continuity across mode
   changes.
2. Capture A/R, D7, XCK, and audio for the same register vectors. Include all
   DR modes, every rate, every articulation value, immediate and transitioned
   inflection, filter values `00`, `01`, `7F`, `E9`, `FE`, and `FF`, and reset
   collisions.
3. Capture isolated voiced and fricative phones at fixed amplitude. Include
   phone 01 and phone 30, then all 64 phones. Record the chip output before the
   Phasor output amplifier when possible.
4. Build a double-precision two-phase nodal model from the LF356, CD4016, and
   capacitor network. Solve Phi0 and Phi1 charge transfer for each section,
   including the fixed F5 and the two noise sections.
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

The final branch gate includes:

- native ROM/reference tests;
- native digital-core XSim;
- XCK rational-enable XSim;
- excitation/filter XSim;
- exhaustive Phasor source and address tests;
- all repository source regressions;
- fresh Vivado synthesis and implementation;
- two clean full builds at the same commit with routed WNS at or above
  `+0.300 ns`;
- firmware packaging from the named accepted timing run, not from a stale
  project bitstream;
- source and synthesis-log checks that no SC-01/Votrax implementation enters
  the image.

Hardware speech quality and real-chip matching remain pending until the new
firmware runs on an Appletini and the test vectors above are captured.
