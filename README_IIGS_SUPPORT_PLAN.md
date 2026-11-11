# Apple IIgs support plan

Checkpoint 1 adds an opt-in GS profile for an Appletini installed in
**physical slot 3**. The source changes and focused simulations are complete.
The default physical-slot-7 profile remains available. The earlier combined
FPGA run failed timing at -0.787 ns. The user selected the later routed PL
in `build/gs-support-pl-m350` (run `20261111T110934Z-2d5a594a-full`) as a
test candidate. It passed its +0.050 ns setup gate at +0.051 ns and falls
short of the +0.200 ns release target. The matching F1.3.7dev15 ARM build
and package passed their audits. The test image is
`build/gs-support-firmware-m350/FIRMWARE.BIN`. Physical acceptance remains pending.
Checkpoints 2 and 3 cover native GS video capture through M2B0 and
within-frame video changes.
Their source changes passed focused tests and review, under the
[capture and renderer contract](README_IIGS_CAPTURE_PROTOCOL.md).

Per the user's build constraint, finish the source and focused checks for
the checkpoints before another full FPGA or ARM build or firmware package.
Do not rebuild firmware between checkpoints. Simulation, host tests, and
syntax checks remain separate from image qualification.
The user also requires stopping if the final build fails timing. Report
that run's result and failing paths; do not start a retry build or package
an image from it.

The branch is `gs-support`, created from `pctransport` at
`2d5a594a512be603ea45dfdfc00b830ce8803d24` on 2026-11-11. See
[setup and limits](README_IIGS_SAFETY.md) and the
[validation record](README_IIGS_VALIDATION.md).

A user reports that an Appletini already works in a ROM 1 GS with SmartPort
working perfectly. Preserve that working service as the first regression target.
The report does not identify the exact firmware, physical slot, or test workload.
Record those details when reproducing it, without treating them as a reason to
delay the source work. It does not yet qualify other cards or native GS video.

Physical slot 3 supplies M2B0 on pin 35 on both ROM 1 and ROM 3 machines. The
current design already routes an M2B0 input to FPGA pin W18 and samples it in
the bus wrapper. The plan assumes that connection and calls for measurement of
its timing on the board. No extra bank-signal connection is proposed. Apple's
[Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/) describes using
M2B0 to capture GS video-memory writes.

**The GS profile keeps host memory ownership disabled.** `/INH`, `/DMA`, and
host address/RW driving must remain blocked at the final physical output gates,
including during reset, faults, stale requests, and PS control changes. Keep RDY
and NMI released. Retain the separate reset path and the existing physical-host
identity protections. Keep vTW/TURBO acceleration, RamWorks memory replacement,
AD8088 host-memory access, and PS host-memory DMA unavailable on the GS.

Safe slot I/O, passive capture, card-local memory, and IRQ require separate
permissions. A feature must not lose access just because another mode in its
module uses DMA. In particular, treat Z80 Appli-Card and PC Transporter separately
from AD8088. Audit any broader GS service restrictions and lift them only where
the actual operation uses the permitted interfaces.

The feature map follows the existing virtual slot assignments. Each listed
feature still needs board qualification in the slot-3 profile.

| Virtual slot | Feature | Checkpoint scope |
| --- | --- | --- |
| 1 | Uthernet II and SSC printer interface | Enable the existing disjoint I/O windows and applicable ROM ownership. |
| 2 | Mousecard, 4Play, or SNES MAX | Enable the selected device. Include Mousecard interrupts. |
| 3 | Physical Appletini connection | Capture M2B0 and retain internal GS 80-column firmware by default. |
| 4 | Mockingboard / Phasor | Enable sound and interrupts, with the ROM 1 mouse restriction below. |
| 5 | Z80 Appli-Card or PC Transporter | Allow private-memory and slot-I/O modes after their own tests. |
| 6 | Disk II | Enable virtual drives with continuous disk timing. |
| 7 | SmartPort and its existing associated services | Preserve the current service and virtual slot number. |
| 7 alternative | SuperSprite | Enable as the existing alternative to SmartPort, including qualified IRQ support. |
| Existing supported slots | No-slot clock | Enable only with the correct owning ROM window. |

Users must reserve each enabled virtual slot for Appletini. Its logical slot
must not also contain a conflicting physical card. Document the required GS
Control Panel settings and which built-in service each choice replaces. Do not
change the GS slot settings behind the user's back. Physical slot 3 does not
require moving SmartPort from virtual slot 7.

ROM 1's native mouse tools call firmware at `$C400`. Enabling Mockingboard at
slot 4 can displace that firmware. Keep slot 4 internal in the default ROM 1
desktop configuration, while allowing users to select Mockingboard for a
compatible configuration. ROM 3 removes this dependency for native mouse calls.
Do not promise automatic ADB mouse replacement by the virtual Mousecard.
[Apple Technical Note 26](https://apple2.gs/technotes/tn-iigs-026/) documents the
ROM difference. Relocating Mockingboard would be separate work with its own
software compatibility checks.

**Checkpoint 1 covers virtual-slot access, continuous clocks, and IRQ.**
SmartPort must work from physical slot 3 before expanding the board tests.

- [x] Combine configured devices and per-feature capabilities with live GS
  ownership in the PL and PS. Keep host-memory rights off.
- [x] Use active-low `/M2SEL` and the live virtual-slot map for the slot-3
  profile. Keep the physical C0F `/DEVSEL` check in the default slot-7
  profile.
- [x] Track C0n I/O, Cn ROM, and C8 ownership separately. Seed from the
  boot report when needed, mirror qualified writes to `$C02D`, `$C068`,
  and the legacy switches, and expose the map to the PS.
- [x] Gate side effects, PC Transporter prefetch, and buffered replies by
  live ownership. Keep physical and ONE//e routing separate.
- [x] Qualify the explicit slot-3 bootstrap before ROM identity. The card
  requires an observed external slot-7 map for its first C7 reply.
  GSSquared traces confirm that the ROM 01 and ROM 03 normal cold-start
  scan writes the map before probing C7.
- [x] Carry a continuous PHI0 tick separately from qualified bus-access
  strobes for video timing, Mockingboard, and Disk II.
- [x] Gate IRQ by enabled, owned device, independent of the instantaneous
  `/M2SEL` or `/DEVSEL` level. Simulation covers pending IRQ, isolation,
  and remapping.
- [x] Separate PS requested settings from effective ownership. Expose the
  slot-3 setting, feature limits, and live policy in menu and UART output.
- [x] Select a routed FPGA test candidate and verify its bitstream, XSA,
  and PL source inventory.
- [ ] Complete and audit the matching ARM firmware and package.
- [ ] Confirm cold, warm, and alternate-slot boot on ROM 01 and ROM 03.
  Measure boot writes and reads, SmartPort, C8 ownership through internal
  C3 and CFFF, and shared IRQ on physical hardware.

Apple documents `/M2SEL` qualification for multifunction cards using virtual
slots in the [IIgs Hardware Reference, chapter 8](https://downloads.reactivemicro.com/Apple%20II%20Items/Documentation/Manuals/IIgs/IIgs%20Hardware%20Reference.pdf).
IRQ is a supported GS interface with its own electrical requirements, described
in [Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/).

**Checkpoint 2 delivers ordinary native GS video.** Reuse the capture pipeline,
video-memory mirror, and SHR pixel renderer, with explicit GS semantics.

- [x] Add GS bank resolution to `hdl/apple/soft_switch_manager.sv` and the
  capture path. Combine the sampled bank signal with the memory switches
  and C029 bank-latch rules. The source follows Apple's prose; the conflicting
  table and physical sampling still require the board matrix below.
- [ ] Measure M2B0 sampling against PHI0 and Q3 on ROM 1 and ROM 3. Keep the
  sampled bank, address, data, and access qualification aligned to one cycle.
  Use the repository's CDC helpers where suitable, without using non-coherent
  status sampling for functional bus data.
- [x] Capture qualified slow-memory writes, including shadowed writes, into the
  correct main/auxiliary video mirror. Capture prepared screens even while
  SHR is off. Do not require DMA reads or observation of fast-side C035 changes
  to reconstruct writes already qualified by `/M2SEL`.
- [x] Extend `hdl/apple/apple_cycle_capture.sv` with native GS mode/control
  records and ordered state snapshots. Track C068 state writes as well as
  individual legacy switches, with separate native and synthetic provenance.
- [x] Make `ps_sources/frontend/apple_cycle_renderer.c` distinguish native GS
  video from Appletini synthetic SHR. Native SHR selection uses C029 bit 7.
  Handle native SHR and DHGR monochrome controls with their GS meanings.
  Keep Appletini extension signatures from activating on native GS memory.
- [x] Reuse and test 320/640-pixel line rendering, fill mode, scan-line control
  bytes, and palettes. Capture the full SHR memory region, including pixels
  at E1:2000-9CFF, controls at E1:9D00-9DC7, and palettes at E1:9E00-9FFF.
- [x] Render GS text foreground/background colors from C022 and capture border
  changes. Keep native video separate from synthetic graphics modes.
- [ ] Qualify native legacy RGB output, MouseText, C029 bit-6 address behavior,
  and GS LANGSEL character maps. The current glyph data is the enhanced
  Apple II set; alternate GS language maps remain unimplemented.
- [x] Add GS 50/60 Hz selection and counter-based frame-phase recovery.
  Bypass the //e boot-time VBL calibration. Keep VBL at line 192 and SHR
  video at 200 lines. Require a fresh observed counter sequence after timing
  loss or a standard change; alternate-slot boot may omit that sequence.
- [x] Define state initialization and recovery for cold boot, warm reset,
  alternate-slot boot, mode changes, and capture loss. Use observed accesses
  and an appropriate CPU report where needed. Keep the synthetic-mode DMA
  reset write to C029 disabled on a physical GS.

M2B0 does not encode all auxiliary-memory soft-switch effects, so it cannot
replace the existing memory-switch model by itself. The sampling and bank
limitations appear in [Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/).
The [IIgs Firmware Reference, Appendix E](https://www.brutaldeluxe.fr/documentation/cortland/v2_01_AppleIIGSFirmwareReferenceManual.pdf)
describes the GS video controls. Check native behavior independently of the
current synthetic SHR implementation.

**Checkpoint 3 delivers video changes within a frame.** Checkpoint 2 does not
by itself qualify raster effects or games that depend on their timing.

- [x] Replay native video in captured time order. Render each elapsed cell
  before applying that record's RAM write, using the existing shadow memory.
  A separate replay copy is not needed because egress applies one record at
  a time. Native replay bypasses legacy lookahead.
- [x] Latch line palettes and controls, and apply border, mode, and pixel
  changes in order. Retain a stable 704x232 canvas through mixed-mode frames.
- [x] Test line-palette changes, mode switches, near-beam memory writes,
  full border coverage, and gap recovery without a DMA rescan. Keep native
  raster effects separate from synthetic video-extension formats.
- [ ] Measure the real SCB/palette fetch position and mode/pixel latency.
  The current line latch uses a named provisional pre-pixel boundary.
  Qualify 3200-color and other raster software against physical GS output.
- [ ] Measure ARM CPU, DDR, and stream capacity under sustained native GS
  traffic. The host harness provides a throughput diagnostic only.

**Validation proceeds separately for source, builds, packages, and boards.**
Use steps 1 and 2 at each source checkpoint. Defer steps 3 and 4 until the
checkpoint source work is complete, then qualify one matching image before
the board tests.

1. Update the existing GS policy tests to express the new ownership rules
   while retaining default-off host-memory assertions. Extend the bootstrap,
   bus-wrapper, slot-remapping, clock, IRQ, and capture simulations. Include
   invalid `/M2SEL` cycles, fast-memory gaps, pending replies, warm reset, and
   prohibited PS/client requests.
2. Run the relevant source regressions before building. The starting set
   includes `scripts/test_iigs_bus_safety.py`, `scripts/test_iigs_ps_policy.py`,
   the affected slot/card tests, and capture/renderer tests. Check legacy IIe,
   II+, and isolated ONE//e behavior wherever shared logic changes. Add tests
   that check behavior rather than merely matching source text.
3. Build and route the changed FPGA image. Check constraint binding, CDC,
   routing, bus skew, setup, hold, and pulse-width results. Retain the existing
   release requirement of nominal routed setup WNS at least +0.200 ns and
   zero timing failures. The selected test candidate meets the separately
   supplied +0.050 ns gate. Its setup, hold, and pulse-width slack are
   +0.051 ns, +0.043 ns, and +0.265 ns, with no failing endpoints.
4. Build the matching ARM firmware. Package it with the explicitly selected
   FPGA image. Record the source revision and bitstream, XSA, executable, and
   firmware hashes. A package or build result does not prove board behavior.
5. Test physical slot 3 on ROM 1 first, then ROM 3. Preserve SmartPort boot,
   reads, and writes while adding other cards. Test fast and slow CPU modes,
   GS/OS, 8-bit software, each enabled card, shared C8 use, and internal/external
   slot remapping. Check Disk II write/readback and interrupt-driven operation.
   Record the exact board, host ROM, settings, software, and image hashes.
6. Measure the physical bus. Confirm M2B0 capture timing, read-data release,
   IRQ behavior, and the continued absence of Appletini INH, DMA, and
   address/RW driving. Use UART and captured bus evidence alongside functional
   results. Compare native video with the GS output or known test patterns.
7. Update `README_IIGS_SAFETY.md` and the relevant feature documentation after
   qualification. Replace physical-slot-7 guidance with the tested slot-3
   setup and state which checkpoint and hardware combinations passed.

The first physical acceptance result is SmartPort running from physical slot 3
with all prohibited host-memory outputs inactive. Native GS video and raster
effects have their own acceptance tests.
