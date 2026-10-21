# Firmware F1.3.2-dev3

This development image adds standalone ONE//e speech clocks to the USB input
and fixed controller-menu changes in F1.3.2-dev2.

F1.3.1 and the retained FPGA in dev2 clocked both SSI-263 engines only from the
physical Apple Q3 pin. Standalone mode has no motherboard to drive Q3. The
engines could accept register writes but could not advance speech or complete
a phoneme. VIA timers, music and other effects used the virtual bus and kept
running. The user reported a two-second timeout on the first speech phrase.

In standalone mode, both edges of the free-running virtual PHI0 now supply the
two SSI XCK enables per bus cycle. DIV2 gives each engine one effective tick
per virtual cycle: 130 fabric clocks in NTSC and 131 in PAL. This follows the
existing virtual motherboard cadence and continues when the CPU pauses.

Physical Apple and vTW modes retain the physical Q3 synchronizer and noise
filter. The shared 62-clock minimum XCK spacing also applies across source
changes; changing mode drops pending work from the old source. The raw Apple
activity guard still stops standalone mode on physical clock activity.
Speech gain, voice synthesis, bus decoding and completion policy stay the same.

## ONE//e RDVBLBAR phase

In standalone mode, `$C019` RDVBLBAR changed one cycle early. The VIDSYNC test
showed it: a Sather `$C019` lock ran one cycle ahead of an HGR vapor lock. A //e
shows each VBL edge one native cycle after the raw scanner counter. The vTW
status path already had this lag. ONE//e reads `$C019` through
`onee_motherboard_io`, which used the raw counter. `onee_video_vblank` in
`apple_top` now classifies the previous cycle at cycle 0. VBL starts at line
192 cycle 1 and ends at line 0 cycle 1. The floating-bus bytes do not change.
`test_apple_timing_reset.py` checks the lag on every cycle of NTSC and PAL
frames. The vTW path on a physical host does not change.

## DIR_A pad edge

`a2fpga_dir_a` now uses `DRIVE 12 SLEW FAST`, the same as `a2fpga_dir_d` since
F0.9.97. This removes 0.54 ns from the DIR_A pad delay under its 10 ns bound.
Earlier F1.3.2 builds failed timing on that pin. Check the address-direction
edge with a scope on the board.

## Build

These fixes need a new FPGA bitstream and matching XSA. An ARM-only rebuild with
the retained F1.3.1 FPGA cannot contain them. Build and simulation results must
remain separate from listening, speech-completion and VIDSYNC checks on the
board.
