# Virtual Printer (Super Serial Card + ImageWriter II)

Appletini emulates an Apple Super Serial Card in slot 1 with an
ImageWriter II behind it. Anything the Apple prints becomes a PNG file
on the SD card.

## How to print

1. Open the config menu, go to the **Printing** tab, and enable the
   Super Serial Card. It is on by default.
2. On the Apple, print to slot 1: `PR#1` in BASIC, or select an Apple
   Super Serial Card in slot 1 (printer: ImageWriter or ImageWriter II)
   in your software.
3. Each printed page lands in `0:/printouts` as `print_NNNN.png`
   (US Letter, 144 dpi color). A print job closes five seconds after
   the Apple stops sending data; a partial last page is saved at that
   point. A form feed always completes a page immediately.
4. The **Browse Printouts...** entry opens a file browser with a
   preview. ENTER renames the selected printout, SPACE deletes it after
   a confirmation.

## Slot sharing

The SSC and the Uthernet II both live in slot 1 with disjoint hardware
decodes, so Ethernet and printing work at the same time:

| Slot-1 resource | Owner |
|---|---|
| $C0n1/$C0n2 (DIP readback), $C0n8-$C0nF (6551 ACIA) | SSC |
| $C0n4-$C0n7 (W5100 window) | Uthernet II |
| $C100-$C1FF slot ROM, $C800-$CFFF expansion ROM | SSC |

The two enables are independent: the Uthernet II uses slot-mask bit 1,
the SSC uses feature bit 2 (`printing.ssc.enabled` in the config file).

## What the Apple sees

The firmware is the real 1981 Apple SSC ROM (a2retronet source split,
built by `scripts/build_ssc_rom.py` with SLOT=1, SSC=1). DIP switches
are hardwired to the classic printer setup: 9600 baud, printer (PPC)
mode, no delays, no width formatting, LF after CR for the BASIC entry.
Ctrl-I commands work as on a real SSC. The virtual 6551 always reports
transmit-ready, and every transmit byte enters a 2 KB FIFO the ARM
drains through the card-control registers ($4A-$4D).

## Printer language coverage

The interpreter (`ps_sources/frontend/imagewriter.c`) renders draft
text in all eight fixed pitches, bold, underline, half-height,
super/subscript, and double width, plus ESC G/S/g/V dot graphics at the
per-pitch densities, ESC T/A/B line spacing, ESC F/L positioning, and
ESC H form length. `ESC K 0` through `ESC K 6` select black, yellow,
magenta, cyan, orange, green, and purple on the emulated four-color
ribbon. Separate strikes over the same dot mix their ink. LQ-only
commands, MouseText, and user-defined characters are consumed and
ignored.

## Tests

`python scripts/test_ssc_card.py` runs the source regressions, rebuilds
and compares the ROM, executes the real firmware under py65 against a
mocked ACIA, compiles and runs the interpreter unit test on the host,
and runs `hdl/sim/tb_ssc_card.sv` under xsim.
