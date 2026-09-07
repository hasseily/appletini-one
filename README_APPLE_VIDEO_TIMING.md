# Apple scanner timing

On a physical Apple host, `apple_timing_gen` advances once per
`ab_read.sss_en` strobe from the PHI0 sampler in `apple_bus_wrapper`.
The counter uses 65 cycles per line and 262 NTSC or 312 PAL lines per frame.
VBL starts at line 192. The same line and cycle feed capture timestamps,
MouseCard VBL, the HDMI activity pulse, and vTW scanner reads.

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

Run `python scripts/test_apple_timing_reset.py` for the reset regression.
It compiles the production PHI0/RES# sampler, activity detector, and timing
counter with the timing wiring and reset block extracted from `apple_top`.
It checks NTSC
and PAL timing through repeated resets, active-display and frame boundaries,
a reset held for more than one frame, reset with PHI0 stopped, and later ROM
commands. It also checks calibration after host clock loss, ONE//e reset
behavior, and return to physical PHI0.
