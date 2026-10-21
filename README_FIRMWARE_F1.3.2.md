# Firmware F1.3.2

F1.3.2 adds USB input parity with Multitini One, fixed controller controls in
the configuration menu, standalone speech clocks, correct VBL and mid-line
video timing in ONE//e, and a new FPGA bitstream. Golden B2.0.6 stays
unchanged.

The local package is `firmwares/firmware-F1.3.2/FIRMWARE.BIN` (4,493,484
bytes). Its SHA-256 is
`5eb4294a37ee25d4d2b9d6742194ccc7fad011b5df1ca35ad6e3991e9a884c66`.

## USB input (from dev1 and dev2)

- Wired Xbox 360 controllers, collection-aware HID parsing and ten selectable
  axis sources. Saved device identities keep paddle mappings stable when USB
  ports change. See `README_FIRMWARE_F1.3.2-dev1.md`.
- Fixed menu controls on every controller: D-pad moves, LB/RB change tabs, A
  selects, B goes back and Home opens or closes the menu. See
  `README_FIRMWARE_F1.3.2-dev2.md`.

## ONE//e (from dev3 to dev5)

- Speech: both edges of the virtual PHI0 clock the SSI-263 engines. Before,
  the engines had no clock in standalone mode and the first phrase timed out.
- `$C019` RDVBLBAR changes one native cycle after the raw scanner counter, as
  on a //e. VBL and vapor-lock syncs now agree.
- The renderer delays `$C050` TEXT one cycle and advances `$C00D` 80COL and
  `$C00F` ALTCHARSET one cycle. ALTCHARSET now takes the vTW one-record
  normalization. The VIDSYNC test shows all switches at 0 in ONE//e.
- The TransWarp tab shows "Accelerate the Apple II (ONE//e)" checked and
  dimmed, because ONE//e always enables vTW.

See `README_FIRMWARE_F1.3.2-dev3.md`, `-dev4.md` and `-dev5.md` for the
analysis.

## FPGA

Vivado 2025.2 full build `20261021T142320Z-8af06741-full`:

| Check | Result |
| --- | --- |
| Setup WNS | +0.047 ns |
| Hold WHS | +0.034 ns |
| Pulse width WPWS | +0.265 ns |
| Failing endpoints | none |
| Route, bus skew, XDC objects | PASS, PASS, 0 missing |

`a2fpga_dir_a` now uses `DRIVE 12 SLEW FAST`, the same as `a2fpga_dir_d`.
Earlier builds failed timing on that pin. The build met the requested 0.040 ns
setup minimum. It is below the 0.050 ns promotion gate of the audited
packagers, so this package was assembled with `make_firmware_bin.bat`.

## Checks

On the board:

- VIDSYNC in ONE//e: VBL and vapor-lock syncs agree; all tested switches at 0
  (with F1.3.2-dev4).

Not yet checked on the board:

- VIDSYNC on native video. The renderer phase table also applies there.
- The TransWarp tab in ONE//e and on a physical Apple.
- The DIR_A edge with a scope, and physical bus access on //e, II+ and IIgs.
- Standalone speech completion and the USB controller list from dev1/dev2.

Host tests: SSI-263 and ONE//e simulations, the timing-reset bench with the
per-cycle RDVBLBAR check, renderer phase and host render harness, vTW static
checks, USB input and configuration menu tests, and the image manifest check.
`test_onee_video_standard.py` fails on a stale check of
`hdl/video2/video_timing_gen.sv`, which this release does not change.
