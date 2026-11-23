# Apple scanner timing

On a physical Apple host, `apple_timing_gen` advances once per
`ab_read.sss_en` strobe from the PHI0 sampler in `apple_bus_wrapper`.
The counter uses 65 cycles per line and 262 NTSC or 312 PAL lines per frame.
VBL starts at line 192. The same line and cycle feed capture timestamps,
MouseCard VBL, the HDMI activity pulse, and vTW scanner reads.

Synthesized `$C019` RDVBLBAR changes one native cycle after the raw counter,
as on a //e: VBL starts at 192:1 and ends at 0:1. The vTW status path and the
ONE//e `onee_video_vblank` signal both apply this lag. Floating-bus reads use
the scanner position two cycles before the raw counter.

Apple RES# must neither stop this counter nor force it to frame zero.
The old `apple_top` connection seeded line 0, cycle 0 when RES# rose and
cleared the VBL phase lock while RES# stayed low. CTRL-RESET could therefore
shift both the scanner phase and the next VBL pulse.

The physical-host path now keeps its counter and phase lock through both
RES# edges. The first boot-ROM `CMD_VBL_START` after FPGA reset still
calibrates the counter to line 192, cycle 15. Later commands keep that phase.
The existing physical activity guard allows a fresh calibration after the
host clocks stop, so a host power cycle still works while the FPGA stays on.
Changing between physical PHI0 and the ONE//e virtual clock also allows a
fresh calibration. ONE//e keeps its existing frame-zero seed and calibration
reset on virtual CPU reset.

PAL Accurate Composite and PAL Accurate TV stay saved when the live timing
is unknown or NTSC. The renderer uses Composite Monitor until it detects PAL,
then applies the saved mode. This includes ONE//e startup with PAL selected.

Run `python scripts/test_apple_timing_reset.py` for the reset regression.
It compiles the production PHI0/RES# sampler, activity detector, and timing
counter with the timing wiring and reset block extracted from `apple_top`.
It checks NTSC
and PAL timing through repeated resets, active-display and frame boundaries,
a reset held for more than one frame, reset with PHI0 stopped, and later ROM
commands. It also checks calibration after host clock loss, ONE//e reset
behavior, and return to physical PHI0.

## ONE//e legacy flicker on F1.3.9

With ONE//e in NTSC and the legacy picture visible at 1920×1080, the 1 MiB
Apple video capture ring reached 1,048,560 bytes in use and reported gaps.
The HDMI scanout underrun and AXI read error counts stayed flat. The ARM
consumer previously released ring space only at the end of a drain. It now
releases processed records in increments during long drains and retains any
records that still need lookahead. This change alone did not stop the gaps
in an HGR menu test on the board.

The composite NTSC renderer now selects its lookup table once per cell and
keeps the phase and signal history local until it has emitted the cell.
Capture checkpoints poll the CGA raster only when CGA graphics capture is
active. The CPU1 main loop still checks for changes to the CGA mode.

On the same ONE//e, the F1.3.9dev24 candidate rendered 3,362 frames over
56 seconds of NTSC tests. The HGR launcher ran at 1920×1080, and Ultima V
ran at 1920×1080 and 1024×768. The samples recorded zero new capture gaps,
renderer resyncs, HDMI underruns, or AXI read errors. The highest sampled
ring occupancy was 624 bytes. These checks measure the board counters.
