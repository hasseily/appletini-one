# Native IIgs capture records

This is the source contract for support checkpoints 2 and 3. It does not
establish board timing or image qualification. Full FPGA and ARM builds and
packaging are deferred until the checkpoint source work is complete.

Native video requires the physical-slot-3 profile and a confirmed IIgs ROM
identity. The FPGA observes video-control writes before identification so
it can retain ROM initialization. This observation grants no host-memory
ownership. ONE//e keeps its existing private capture path.

## Record layout

All records remain 64 bits. Kinds 0, 1, and 2 retain their existing layouts.
An all-zero record remains the capture-gap marker.

Kind 3 is a native GS register write. It uses the existing kind-1 address,
data, line, and cycle fields for `$C022`, `$C029`, `$C02B`, `$C034`, and
`$C068`. This distinguishes native `$C029` from the synthetic SHR control.

Kind 5 is a pre-identification GS shadow write. It uses the kind-0 memory
half: address at bits 32:9, write-valid at bit 8, and data at bits 7:0.
Bits 60:33 are zero. The explicit slot-3 observer emits only these qualified
memory records before GS identity. Egress updates the shadow without running
synthetic graphics hints. Timing and native controls start after the confirmed
identity snapshot. This record grants no bus ownership or display permission.

Kind 4 is an ordered native state snapshot:

| Bits | Field |
| --- | --- |
| 63:61 | Kind 4 |
| 60 | Native GS active |
| 59:51 | Scan line |
| 50:44 | Cycle in line |
| 43:33 | Existing 11 soft-switch bits |
| 32:25 | `$C022` text colors |
| 24:17 | `$C029` New-Video |
| 16:9 | `$C02B` language and video standard |
| 8 | Scanner phase locked |
| 7 | Frame selected for rendering |
| 6:5 | Reserved, zero |
| 4:1 | `$C034` border-color nibble |
| 0 | Reset notification |

Snapshots occur on native entry, exit, reset, phase-lock changes, and at each
native frame start. They travel through the same FIFO as writes. The renderer must not
read a newer live MMIO state ahead of queued records. Snapshot admission
must preserve chronology when a time marker or write occurs at the same
edge. If capture cannot retain a record, it must report a gap.

Native timing records use the kind-0 frame half on the scanner update pulse,
independent of `/M2SEL`. Native memory writes also carry their scan position.
The kind-4 frame-selection bit preserves the configured frame decimation.
Memory writes still require a qualified bus cycle, including writes made
while SHR is off. The mirror covers both text pages and `$2000–$9FFF` in
main and auxiliary memory, including native SHR pixels, controls, and palettes.

## Timing and memory

The physical PHI0 tick advances the scanner during fast-memory activity.
Its update pulse supplies a time marker before that cycle's CPU write.
GS horizontal counter values `$00,$40…$7F` map to cycles 0…64; active
pixels occupy cycles 25…64. Native SHR uses lines 0…199. VBL starts at
line 192 even in SHR. `$C02B` bit 4 selects 262 or 312 lines per frame.
Phase recovery uses observed horizontal, vertical, horizontal reads. The
measured PHI0 interval validates both horizontal samples and locates the
vertical read if the sequence crosses one scan line. Invalid or stale
sequences cannot establish phase lock.
[Apple Technical Note 39](https://apple2.gs/technotes/tn-iigs-039/) and
[Technical Note 40](https://apple2.gs/technotes/tn-iigs-040/) describe the counters
and GS VBL polarity.

The slot-7 GS boot path reads `$C02F`, `$C02E`, then `$C02F`. This can
establish phase without host-memory DMA. An alternate-slot boot may omit
that path. A change of standard or a stopped PHI0 clock clears the lock;
another coherent counter-read sequence must restore it. The renderer holds
the last completed image until it has locked phase and a complete new frame.
Warm reset preserves scanner phase while PHI0 continues; board tests must
check that assumption against both ROM revisions.

M2B0 identifies an odd slow-side bank. When `$C029` bit 0 enables its latch,
a high M2B0 selects auxiliary memory; otherwise the memory soft switches
select the bank. Native capture ignores RamWorks banks. Apple's prose and
the existing GS emulator implementation support this polarity, but the
Hardware Reference tables conflict with the prose. The physical matrix of
E0/E1, shadowed 00/01, RAMWRT, 80STORE, and PAGE2 remains required.
[Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/) describes the
M2B0 sampling window and the soft-switch information it does not carry.

## Rendering order and output

Egress already applies memory writes one record at a time. A native
pre-record callback renders elapsed pixels from the old shadow before
applying the current write. The renderer then applies that record's control
change. Future records used by legacy lookahead must not affect native
pixels. Reuse the existing SHR palette and 320/640-pixel cell decoders.

Native replay latches each line's SCB and palette at the named
`GS_LINE_FETCH_CYCLE` boundary, currently the first active cycle. A later
palette write affects a later line. The real VGC fetches these values
before pixels; the exact fetch position is not yet measured on this card.
Tests prove the chosen replay order, not exact VGC raster timing. Pixel
writes retain cycle order within the line.

Native `$C029` bit 7 selects SHR without requiring bit 6. Bit 5 controls
legacy monochrome behavior and must not desaturate native SHR. Native mode
ignores Appletini synthetic graphics signatures. `$C022` supplies text
foreground and background colors.

Display mode 3 carries a 704×232 BGRA canvas in the existing 1 MiB slot.
The 640×200 active area starts at (32,16). The compositor doubles native
rows vertically and uses the existing SHR viewport. Border pixels belong
to this canvas, allowing within-frame border and mode changes. Legacy
14-pixel cells map to the same 16-pixel cycle span; legacy video ends at
line 192, and SHR continues through line 199. Synthetic mode 1 keeps its
640×400 layout.

A gap invalidates the in-flight frame. A later snapshot can recover control
state, but it cannot reconstruct a lost RAM write. That data needs a host
redraw; no GS DMA rescan is permitted. Tests and board reports must keep
control recovery separate from complete memory recovery.

The egress gap marker can precede older records still in the capture FIFO
or egress staging. Native recovery therefore skips the first locked frame
boundary after a gap and consumes at least 4160 timed records before
accepting another boundary. This exceeds the 4096-entry capture FIFO, the
16-entry egress stage, and the small packing queues. An old state snapshot
cannot clear recovery early. If those queue sizes change, review this bound.
Reset or loss of phase clears the recovery count; another gap restarts it.
