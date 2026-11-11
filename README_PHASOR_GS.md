# IIgs Phasor diagnosis and selectable slot

This source checkpoint follows F1.3.7dev15. The dual-Phasor version failed
FPGA placement. The current source replaces it with one Phasor that can
occupy slot 4 or slot 2. The user-selected m250 FPGA build passed timing
and now has a matching F1.3.7dev16 package. The published dev15 image still
fixes its Phasor in slot 4.

## Tester report and Pure Noise

A tester reports that a 4 MB ROM 1 IIgs with Appletini in physical slot 3
works except for Phasor, including when GS slot 4 is set to Your Card.
French Touch's **Pure Noise_v11** identifies the IIgs but does not detect
a Mockingboard. This is a user report; no UART or connector capture is
available yet.

The [author's source archive](http://fr3nch.t0uch.free.fr/PNO/PNO_2016.zip)
contains the same disk as the [v1.1 download](http://fr3nch.t0uch.free.fr/PNO/PNO_v11.dsk).
The disk SHA-256 is
`f18cefc7aa8f9bf1479bea0aecaf45215d1d20fcdea48b3c88fc2f26469910c0`.
In `Sources/boot.a`, the demo clears SPEED bit 7 at `$C036` before detection.
It scans slots 7 down to 1 and reads VIA Timer 1 low at `$Cn04` twice,
eight CPU cycles apart, expecting a difference of `$F8`. It repeats that
test without first writing the VIA. This detects a running timer, not AY
or SSI audio.

`test_iigs_phasor.py` reproduces both timer-read pairs. They pass with
external slot-4 ownership even with the new sound-clock fix disabled.
The bench models host cycles; it does not prove the tester's physical
PHI0 cadence or slot configuration. **The detection failure remains open.**

First collect `:machine` and `:irqdbg` from UART while the demo fails.
The leading colon starts a command. Check for reported machine ID 4,
slot-3 profile on, no identity fault, a live map, and ROM-mask bit `$10`
for slot 4. The physical slot-3 setting requires an Appletini restart.
Also record whether startup passed through Appletini's C7 boot ROM or
booted slot 6 directly: optional GS cards need the ROM's host report.
Setting GS slot 4 to Your Card alone does not enable Appletini's profile.

If those checks pass, capture `$C402` write/readback and the two `$C404`
timer reads with `/M2SEL`, PHI0, address, R/W, and data. This separates
missing register replies from incorrect elapsed timer counts. No physical
sampling taps changed without that evidence.

## Confirmed sound-clock defect

The GS per-slot access gate removes `data_en` during unrelated and fast
cycles. The AY clock used that gated strobe, so programmed tones could stop
or advance only when software accessed the Phasor. VIA timers already used
the continuous `phi0_tick`; that distinction matters for the demo's probe.

The physical wrapper now exports a continuous pulse at the existing late
data-sample phase. The enabled GS Phasor uses it for AY time and the native
mode's extra tick. Reads and writes still require slot ownership. Legacy,
TransWarp, and ONE//e keep their existing data-strobe phase.

## One Phasor in slot 4 or slot 2

There is one hardware Phasor: two VIAs, four AY/YM chips, two SSI voices,
and one set of audio controls. The Phasor tab selects Off, Slot 4, or Slot 2.
The Slot 2 tab also offers Phasor alongside Off, Mouse, 4Play, and SNES MAX.
The card never responds in both slots.

Selecting Phasor in slot 2 disables its slot-4 placement. Choosing another
slot-2 card then turns Phasor off; it does not move it to slot 4. Selecting
Phasor in slot 4 releases slot 2 if Phasor held it, while leaving a selected
Mouse or gamepad card alone. Moving Phasor clears its runtime mode, VIA,
AY, speech, and IRQ state. Pan, EQ, volume, and saved preferences follow
the card.

Existing `phasor.*` audio settings remain shared. Slot 2 uses
`slot2.card=PHASOR` with `phasor.slot4.enabled=off`; older slot-4 profiles
keep their meaning. A profile requesting both locations selects slot 2
regardless of key order. The discarded draft's `phasor2.*` settings are no
longer emitted. Only the chosen, supported slot forces TransWarp slowdown;
the user's requested slowdown mask remains saved.

On older FPGA images, firmware preserves the saved slot-2 choice and
settings but disables that unavailable placement, keeps slot 4 off, and
leaves its slowdown choice editable. The menu states that an FPGA update
is required.

| Register index | Function |
| --- | --- |
| `$AD` | Selector bits 2:0: 0 Off, 1 Mouse, 2 4Play, 3 SNES MAX, 4 Phasor in slot 2; 5-7 select Off. Presence bits 7:4 stay unchanged. Read-only bit 9 advertises the single relocatable Phasor; signature `$5332` remains in bits 31:16. |
| `$08`, `$0A`, `$0C` | Shared Phasor pan low, pan high/SSI pan, and audio controls, unchanged from the original interface. |
| `$2C` | Shared Phasor SSI diagnostics, unchanged from the original interface. |

The second core, its audio summation, and registers `$B8`-`$BB` are removed.
The location choice is independent of the slot enable and GS access masks:
denying slot 2 cannot make Phasor fall back to slot 4. A live GS mapping
change closes access and IRQ permission without erasing an enabled card's
state or stopping its audio. Phasor uses neither `/INH` nor `/DMA`.

On a IIgs, use the physical slot-3 profile and set the chosen logical slot
to Your Card. Slot 2 displaces the internal modem service. Using slot 2
alone allows ROM 1 owners to leave slot 4 internal for native mouse support.
Other host modes retain their existing bus policy.

## Validation and remaining work

Focused single-card checks passed:

- Slot-2 XSim: 2,562 checks cover relocation, silence in the old slot,
  reset and IRQ clearing, state across remaps, Mouse coexistence, and
  disabling slot 2 without falling back to slot 4.
- All 11 IIgs safety benches and production RTL parsing passed.
- The GS Phasor bench checks both locations against live `$C02D` mapping.
  Mapping only the other slot blocks reads and IRQ output; remapping the
  chosen slot restores the pending IRQ and saved VIA state.
- Host tests cover shared audio settings, both profile key orders,
  older FPGA images, and slowdown for only the supported chosen slot.
  The SSI UI checked 5,632 mixer cases; gamepad service, runtime card
  selection, GS firmware policy, video, and VidHD checks passed.
- Seven frontend C files passed ARM syntax checks against the
  existing BSP. This is not a firmware build.

The full config-profile suite still has a pre-existing menu-shell assertion
failure, also reproduced against HEAD. The selected m250 FPGA build and
matching ARM package passed the checks below. Physical board operation
remains unverified.

The pan functions now sum gated shifts instead of selecting among many
precomputed sums. They retain each term's truncation and the same latency.
`test_phasor_pan_equivalence.py` checks the production functions against
frozen reference functions: all 8,192 PSG and 2,097,152 SSI combinations
pass. The m250 design uses 40,424 routed LUTs; no separate build isolates
the savings from the pan change.

## Selected m250 build and F1.3.7dev16 package

The user selected `build/gs-support-single-phasor-pl-m250`, run
`20261111T203601Z-09a3bd15-full`, for packaging. Its nominal setup, hold,
and pulse-width slacks are +0.106, +0.033, and +0.265 ns, with no failing
endpoints. Route, bus skew, and checked constraint bounds pass. All 680
supplied source hashes match the current HDL/scripts.

The original run records a +0.000 ns export gate. A separate package check
requires actual setup slack of at least +0.050 ns and passes. It preserves
the original manifest and exact bitstream/XSA/checkpoint; no FPGA rebuild
or substitution occurred while packaging.

`build/gs-support-single-phasor-firmware-m250` contains the fresh matching
FSBL/BSPs, CPU0 and CPU1, image audit, source records, and installable
`FIRMWARE.BIN`. The image audit passed FPGA identity, all ELF load bytes,
embedded CPU1, version, CRC, and recovery checks. Golden Boot B2.0.6 and
prior published images remain unchanged. No flashing or publication ran.

See `docs/releases/F1.3.7dev16.md` for setup, image hash, and hardware limits.

## Single-Phasor run: synthesis did not finish

Run `20261111T194546Z-09a3bd15-full` used the same fresh m350 recipe in
`build/gs-support-single-phasor-m350/`. All 680 copied source files matched
their frozen hashes and the working tree. Project creation and supporting
IP synthesis passed. Main synthesis stopped writing its log at 19:48:17 UTC;
its native process had exited when checked, leaving task workers open.
No crash report or synthesis completion/error marker explained the exit.

After confirming the main process was absent, the supervisor stopped only
this run's orphan workers and scheduler. The launcher returned code 1 at
19:53:59 UTC. Placement and routing never started, so this is an incomplete
tool run, not evidence of a fit or timing failure. No retry, ARM build, or
package followed. The selected m350 artifacts retain their original hashes.

## Discarded dual-Phasor run: placement failed

Run `20261111T191525Z-09a3bd15-full` used a fresh project in
`build/gs-support-dual-phasor-m350/`, with 680 copied and hashed source files.
It repeated the passing m350 recipe: Vivado 2025.2, eight jobs and worker
threads, a 0.350 ns implementation margin, full synthesis/implementation,
no incremental checkpoint, and a final setup requirement of +0.050 ns.
All four timing-hook hashes matched m350. The copied sources matched
the working tree at the end of that run. They retain the discarded
two-core implementation and differ from the current single-card source.

Synthesis completed without errors or critical warnings and reported
52,452 LUTs (98.59%). Detail placement then failed with `Place 30-487`:

| Placement diagnostic | Result |
| --- | --- |
| Combined LUTs / device capacity | 53,989 / 53,200 |
| Control sets | 1,806 |
| Registers / device capacity | 37,935 / 106,400 |
| Remaining slices / slices needed by unplaced logic | 8,169 / 9,351 |

That design needed both less logic and better slice packing.
The raw synthesized LUT count did not establish fit. Routing and final
timing signoff did not run; there is no WNS result, bitstream, or XSA for
this candidate. The run stopped after this attempt, with no retry.

The user authorized ARM and packaging only if timing passed, so neither
ran and the version remains F1.3.7dev15. The selected m350 artifacts remain
unchanged. The user then requested one selectable Phasor instead of two
independent cards. Hardware acceptance must separately check Pure Noise
detection, each slot location, speech, IRQs, and legacy hosts after a future
candidate fits and passes timing.
