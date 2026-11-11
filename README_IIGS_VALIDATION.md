# Apple IIgs support validation

The current F1.3.7dev16 package uses the user-selected single-Phasor PL in
`build/gs-support-single-phasor-pl-m250`. Its +0.106 ns setup result clears
the separate +0.050 ns package gate; hold is +0.033 ns and pulse width is
+0.265 ns, with no failing endpoints. Its fresh matching ARM build and
image audit passed. Physical board acceptance remains pending.

The earlier F1.3.7dev15 package used `build/gs-support-pl-m350`, which passed
its supplied +0.050 ns gate at +0.051 ns. Those artifacts remain unchanged.

New source work after dev15 adds a continuous GS AY clock and a single
Phasor selectable in slot 4 or slot 2. The tester's Pure Noise detection
failure remains open. See the
[separate diagnosis and validation record](README_PHASOR_GS.md).
The discarded dual-Phasor run, `20261111T191525Z-09a3bd15-full`, repeated the
passing m350 recipe but failed detail placement: 53,989 combined LUTs
exceeded the 53,200 limit, and remaining logic needed 9,351 slices where
8,169 remained. No retry, ARM build, or package followed. The single-card
run `20261111T194546Z-09a3bd15-full` then ended during synthesis: its main
process exited without a result, leaving orphan workers. No placement or
timing result exists; the cause is unknown. No retry or ARM/package followed.

## Source and simulation

The focused checks cover live slot ownership, the opt-in slot-3 bootstrap,
continuous physical clocks, separate IRQ permission, and legacy/private bus
routing. The profile remains off by default.

| Check | Result |
| --- | --- |
| `python scripts/test_iigs_bus_safety.py` | All 11 XSim benches passed, including live slot ownership, default IIgs and IIe bootstrap, final pad isolation, and native bank/timing recovery. |
| `python scripts/test_pc_transporter.py --rtl-only` | PC Transporter and shared Disk II DDR benches passed. Remapping prevented C8 prefetch and side effects. The pointer and private register survived remap and became accessible after a new Cn claim. |
| `python scripts/test_smartport_data_phase.py` | Legacy and ONE//e routing equivalence passed with 1,272,185 checks. |
| `python scripts/test_slot2_card.py` | Mouse, FourPlay, SNES MAX, and slot selection checks passed. |
| `python scripts/test_onee_bus_integration.py` | Joined ONE//e bus simulation passed. |
| `python scripts/test_iigs_ps_policy.py` | All 9 checks passed, including compiled C policy tests and rejected profile readback holding Apple in reset. |
| `python scripts/test_uthernet2_card.py` | Deferred replies expired at the cycle boundary; a physical W5100 read could finish its bookkeeping without reopening the Apple reply. Both directed expiry cases fail against the original RTL. |
| Disk II simulations | All 7 benches passed. Rotation continued through unselected cycles and an internal slot-6 mapping without register side effects. |
| Timing and wrapper simulations | Continuous GS ticks advanced video timing through 260 unselected cycles. Reset timing reported 87,642 ticks, including 37,661 reset ticks; VBL and RDVBL each advanced 4 times. All 128 final isolation truth-table cases passed. |
| Firmware regressions | Boot reset 21/21, ONE//e config 12/12, online update integration 12/12, vTW startup, Appli-Card, PC Transporter host checks, and 5,632 Phasor UI combinations passed. |

The live slot bench runs the production boot card, soft-switch manager, and
phase and reply gates extracted from `apple_top.sv`. It uses synthetic host
cycles and does not model IIgs pin timing.

The existing `test_config_profiles.py` modern-shell assertion fails on both
the starting revision and this branch. It is not a passing check. Earlier
ARM syntax checks used the old BSP. The selected candidate below has a fresh
platform, BSPs, FSBL, CPU1, and CPU0 built from the matching XSA.

The Sol 6.1 coding agents reviewed each other's RTL and firmware changes.
The supervisor reviewed the combined policy and physical output paths before
starting the full FPGA build. The build inputs and the original artifact
hashes are recorded under `build/gs-support-validation/`.

## FPGA timing work

The first implementation run, `20261111T071915Z-2d5a594a-full`, exposed a
long path from GS C8 ownership to the physical data-direction output.
Placed setup WNS was -4.675 ns. The supervisor stopped that run during
routing at an intermediate WNS of -4.186 ns; these are diagnostic results,
not final routed qualification.

The revised source registers the qualified reply owner at the serve edge
and keeps a short live veto at the physical output. It also feeds the
soft-switch manager, Disk II, SSC, and Uthernet from the existing captured
write byte. This removes a long path through the ONE//e live reply mux.
The registered legacy ownership grant and direct pad fault isolation keep
their separate roles. Timing constraints remain unchanged.

All 10 GS benches and the affected ONE//e, SmartPort, SSC, Uthernet, and
7 Disk II benches passed again after these changes. A directed case also
confirmed that a new unselected address expires a buffered reply when the
preceding cycle has no data strobe.

The second implementation run, `20261111T073741Z-2d5a594a-full`, removed
the original long C8 path. Placed setup WNS improved to -1.994 ns. Routing
reported -1.557 ns, and post-route optimization reached -1.427 ns before
the supervisor stopped the run to address the remaining paths. This run
also failed the timing target and produced no qualified replacement image.
The intervening launcher attempt required resetting the interrupted run.

Later source changes register the internal identity-fault decision and a
per-cycle data owner, while retaining direct fault isolation at the pads.
The GS, SmartPort, PC Transporter, and ONE//e checks passed again. At that
point the older XSA and firmware predated these changes.

Full builds were deferred while the remaining source checkpoints and focused
checks completed. The next combined FPGA run finished on 2026-11-11
at 09:04:20 UTC and failed the timing gate:

| Failed run `20261111T084000Z-2d5a594a-full` | Result |
| --- | --- |
| Nominal setup WNS / required margin | -0.787 ns / +0.200 ns |
| Setup TNS / failing endpoints | -777.873 ns / 2,770 |
| Hold WHS / failing endpoints | +0.052 ns / 0 |
| Pulse-width slack / failing endpoints | +0.265 ns / 0 |
| Routing / bus skew / checked constraint bounds | Passed |
| Missing constraint objects / unconstrained internal endpoints | 0 / 0 |

The worst path runs from `machine_identity_fault_q_reg_replica_2` to the
physical data-direction output `a2fpga_dir_d`, with six logic levels.
The worst reported fabric paths run from the virtual bus's registered
address bit 11 through slot/card qualification to SuperSprite PSG envelope
counter reset inputs, at -0.760 ns.
The address-direction output also fails at -0.344 ns, from the ONE//e
isolation guard. The placed design occupies 98.55% of slices and 76.57% of
LUTs; timing work must account for that placement pressure.

Vivado wrote an unqualified bitstream in the implementation directory, but
the signoff script refused to export hardware. At that point the project
still held the older XSA with SHA-256
`95bf65e3329c13432de21d660d0f32dc05e70eaed1d6fc848f351985820d422f`.
Do not pair that older XSA with the failed run's bitstream.

The full reports are in `.timing_runs/20261111T084000Z-2d5a594a-full/`.
`build/gs-support-validation/final-source-inputs.json` records 907 source
hashes; they still matched after routing began. The final outcome and
selected failing paths are also recorded in `final-build-outcome.json`.

## Selected routed test candidate

The user selected run `20261111T110934Z-2d5a594a-full` from
`build/gs-support-pl-m350`. The run used a 0.350 ns implementation setup
margin. The figures below are nominal routed results. The test candidate
passes the supplied gate and falls short of the earlier +0.200 ns
release target.

| Check | Result |
| --- | --- |
| Setup WNS / supplied minimum | +0.051 ns / +0.050 ns |
| Hold WHS / pulse-width slack | +0.043 ns / +0.265 ns |
| Failing endpoints | 0 setup, 0 hold, 0 pulse width |
| Route, bus skew, and checked constraint bounds | Passed |
| Missing constraint objects / unconstrained internal endpoints | 0 / 0 |

The selected bitstream SHA-256 is
`dc65e9ca5042fae715dbea944d2f61e6af1418f41df970bc880e168b6d011014`.
The selected XSA SHA-256 is
`adf3ed9dcc549076f36635c492f28f49c4aae1b4b622c3bca1115477461fe45f`.
Both match the timing manifest. The project XSA matches the selected copy,
and all 676 entries in the saved PL source inventory match the working
tree. The timing run records base commit `2d5a594a` with working-tree changes.

The slot-policy reviewer checked the final timing changes across five RTL
files and two benches. All 11 GS benches passed. SmartPort routing
equivalence passed 1,272,185 checks. A later PS-only Phasor menu-help
correction passed the Phasor checks 12/12, GS policy checks 9/9, and the
video output configuration suite's 66 checks and 5,632 Phasor UI
combinations. The PL inventory still matches.

The matching F1.3.7dev15 ARM build and package passed. No further Vivado run
was started: the package uses the selected bitstream without changes.
The fresh Vitis completion receipt binds the source XSA, both platform
XSAs, FSBL, CPU0, CPU1, and generated CPU1 blob to this build. All 1,611
saved PL/script and ARM source hashes matched the source archive; the
production sources still matched after the build and package.

The test image is `build/gs-support-firmware-m350/FIRMWARE.BIN`,
4,972,844 bytes, SHA-256
`b97168535090f5d03998de6a7959eac538b26b2429027e1e6414ed4a193785a6`.
The recovery-capable manifest passes with payload CRC32 `0xCE71B3D6`.
The four-partition audit verifies the selected FPGA payload, all stored
FSBL and CPU0 ELF load bytes, and CPU1's generated and embedded image.
It also verifies the version, frontend memory limits, and both platform
XSAs. A second reviewer checked the audit inputs and completion receipt.

`ARM_BUILD.json`, `VITIS_COMPLETE.json`, `FIRMWARE_AUDIT.json`, source
archives, input copies, and build logs are in the same directory. The
root firmware, published firmware copies, and Golden `BOOT.BIN` retain
their prior hashes. This image has not been flashed or tested on a board.

## Native video source work

The native record format and replay order are documented in
[the capture contract](README_IIGS_CAPTURE_PROTOCOL.md). Capture tests now
cover native beam ticks through unselected cycles, ordered controls and
memory writes, snapshots, overflow notification, and a lost exit snapshot.
The capture, TURBO video/egress, ONE//e video and ROM boot, and 17 VidHD/SHR
checks passed. The native timing bench covers both sides of a counter wrap,
PAL and NTSC frame wrap, stale and incoherent samples, next-tick phase
seeding, clock loss, and warm reset. Production RTL also parsed with `xvlog`.

`python scripts/test_iigs_video_renderer.py` compiles production renderer
helpers and the egress write-dispatch body. It passed state, bank-write
replay, native SHR selection, 320/640 decode, line palettes, C022 colors,
border changes, mixed modes, phase gating, full-canvas coverage, and gap
recovery. Native A2Li and synthetic SHR signatures did not change the output
mode. The existing VidHD/SHR 17/17 and AppleWin SHR 6/6 checks passed again.
Renderer, egress, and handoff passed ARM syntax checks using the existing BSP.

Cross-review found and fixed three further cases: pre-ID writes now use
shadow-only records that bypass synthetic signatures; native legacy-only
frames get the IIgs badge; and gap recovery waits beyond the maximum older
FIFO backlog before accepting a new frame boundary. Directed tests cover
pre-ID A2Li/SHR hints, older and duplicate state snapshots, reset, phase loss,
and recovery. Missing video-memory bytes still require a host redraw.

Native composition, border/flood, effects, clipping, and buffer guards passed
compiled pixel checks at 1x and 2x. Mode transitions passed across all three
framebuffer slots and six output sizes. PNG checks validated every pixel in
the 1280x800 native crop, 1408x928 bordered image, and fallback image.
The legacy renderer/compositor oracle passed 29,088 frame checks. Compositor,
debug overlay, main, screenshot service, and UART passed ARM syntax checks.

The supervisor reran `python scripts/test_iigs_video_renderer.py --benchmark`
with GCC 9.5.0 and `-O2`. Over 120 selected NTSC frames, the host fixture
processed about 119.8 million tick/snapshot records per second, and about
146.3 million records per second with a separate write at every tick. These
short cached-memory runs omit ARM DDR, ring transfer, USB, the compositor,
and legacy text work. They are a host diagnostic, not a CPU1 capacity result.
At 1.024 MHz, native tick traffic is about 8.192 MB/s, or 16.384 MB/s with a
separate write every tick. Each selected canvas contains 653,312 bytes.

Phase acquisition needs a coherent observed C02F/C02E/C02F read sequence.
The slot-7 boot fixture now includes one on its GS path. Alternate-slot boot,
runtime standard changes, and clock recovery can remain unlocked until
another sequence occurs. A gap can recover control state at a snapshot,
but missing RAM writes remain stale until the host redraws them.

Native video retains the current enhanced Apple II glyph data. It does not
yet implement GS LANGSEL alternate character-ROM maps. C029 bit 6 is
retained in the native state; its legacy linear-address behavior and the
addresses visible at the slot still need a board trace. These limits must
not be described as complete GS video compatibility. A leading zero in
320-pixel color-fill mode uses palette entry zero; Apple's Hardware
Reference leaves that case undefined.

## ROM bootstrap trace

Two separate GSSquared sessions ran the normal cold-start scan with slot 7
external. The debugger traced writes to `$C02D` and stepped the first
`$C705` probe. The ROM wrote `$80` at both listed program addresses before
the probe, which returned `$03` in each session.

| ROM | Program addresses of `$C02D` writes | Program address of first `$C705` probe |
| --- | --- | --- |
| ROM 01 | `$FF:B723`, `$FF:F9E5` | `$FF:FA05` |
| ROM 03 | `$FF:BBC3`, `$FF:F9E6` | `$FF:FA06` |

This establishes executed instruction order in the emulator. Apple's
description of internal I/O and the fast-side exceptions identifies the
slot and state register reads as fast. The writes are expected to use
slow I/O. A board trace still needs to show the writes reaching Appletini
before the first slot-7 probe after reset release. [IIgs Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/)

## Pending acceptance

- The selected test image is built and audited. Release qualification still
  requires nominal setup WNS of at least +0.200 ns. This does not authorize
  another FPGA build; the user requested the selected m350 image.
- Check cold, warm, and alternate-slot boot on ROM 01 and ROM 03. Capture
  reset, `/M2SEL`, physical `/DEVSEL`, address, and data around C02D and the
  first C7 probe. Test the default physical-slot-7 setup alongside slot 3.
- Retest cold and warm boot on unenhanced and enhanced IIe, Europlus in
  physical slot 4, vTW reboot, and default-profile LINTXT when another
  slot boots.
- Confirm slot-3 SmartPort boot, reads, and writes before testing the other
  virtual cards. Scope data-direction release, shared IRQ, and the continued
  absence of Appletini `/INH`, `/DMA`, and address/RW drive. Measure GS read
  setup and hold at the connector and the IRQ level through the TXS0108.
- Measure internal C3, internal C8, and CFFF visibility. Test external C8
  reclaim after `$C007` or `$C068` enables internal ROM and after a
  `$C02D` remap. Run GS/OS and 8-bit software with both fast and slow CPU
  settings.
- Validate native M2B0 video and within-frame SHR changes in their own
  checkpoints.
