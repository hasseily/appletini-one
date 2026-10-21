# Firmware F1.3.2-dev4

This development image corrects the HDMI position of three mid-scanline soft
switches. It keeps the dev3 FPGA bitstream and XSA. Only the ARM frontend cores
change.

## Soft-switch phase

The VIDSYNC test in ONE//e standalone mode measured each switch after a beam
sync. With dev3, the `$C019` and vapor-lock syncs agree. These switches were
off:

| Switch | dev3 result | Correction |
| --- | --- | --- |
| `$C050` TEXT | 1 cycle early | delay 1, the same as `$C052` MIXED |
| `$C00D` 80COL | 1 cycle late, in text and double hi-res | advance 1 |
| `$C00F` ALTCHARSET | 2 cycles late | advance 1, plus vTW normalization |

`$C052`, `$C05E`, `$C055`, `$C056` and `$C001` were exact and do not change.

The soft-switch manager updates every video switch on the same `serve_en`
edge. Thus the capture records all switches the same way, and the CPU1
renderer causes the per-switch errors. The renderer moves 1 MHz vTW switch
states one record ahead, because a vTW `$C0xx` write lands one capture record
late. ALTCHARSET was the only video switch outside that mask. This added one
cycle in vTW and ONE//e only.

`apple_cycle_renderer.c` now includes ALTCHARSET in `VTW_PHASE_SW_MASK`. The
phase table delays TEXT one cycle and advances 80COL and ALTCHARSET one cycle.
The table applies to native video too. Native timing is not checked. The
prediction is that native showed the same TEXT, 80COL and ALTCHARSET errors
of one cycle. `test_renderer_phase.py` and host harness test T5 check the new
table.

## Build

This image uses the dev3 FPGA build `20261021T142320Z-8af06741-full`. See
`README_FIRMWARE_F1.3.2-dev3.md` for its timing and its FPGA changes.
