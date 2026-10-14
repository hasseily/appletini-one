# Firmware F1.3.0

F1.3.0 on `custom-glyphs` includes the F1.3.0-dev10 multiboot base and the
SmartPort character-ROM extension. The comparison starts at the F1.2.5
release, commit `886ea50`.

Changes since F1.2.5:

- Upload custom character ROMs through SmartPort `$81`, with CRC checks,
  frame-boundary activation, abort, and restore to the menu-selected ROM.
- Choose Appletini or Multitini at startup, or switch once without changing
  the saved choice. Switch checks and boot menu status run faster.
- Show the installed Golden version, update Golden from About, and correctly
  read multiboot metadata. Disk/SD handoff and CPU1 cold startup are fixed.
- Start saved vTW before the Apple //e prompt, apply its saved speed after
  handoff, and pause ONE//e correctly in the menu.
- Fix native ProDOS boot with Appletini-supplied RAM: `PR#3` can open the
  internal 80-column display on the first boot, without a `PR#7` reload.
- Add 32, 64 and 128-cycle vTW slowdown settings; keep the 512-cycle default.
- Save full-output screenshots with display masks and effects, with fewer
  SD writes.
- Raise SSI speech output and set the default SSI gain to 0 dB.
- Clarify the standalone 5 V power instructions.

[Glyph Lab](../appletini-software/demos/glyph_lab/README.md) provides three
animated text scenes with original fonts. Its bootable disk exercises the
new API and restores the configured ROM on exit.

The firmware uses the Appletini multiboot product manifest and the 6 MB
firmware slot. Golden B2.0.6 is a separate image; this firmware package does
not replace Golden or Multitini. The build pins the existing FPGA bitstream,
XSA and FSBL, compiles both ARM frontend cores, and audits the embedded
renderer, image partitions, source archives, linked memory limits and CRC.

The earlier glyph test worked according to the user. The final F1.3.0 image
has build and host test evidence; it has not been flashed or retested here.

The native boot regression runs the actual Enhanced IIe reset ROM, ProDOS
2.4.3 slot scan, BASIC.SYSTEM startup and `PR#3` code. The old C8 helper
reproduces `$BF98=$B0`, `$BF99=$86`, `$BE16=$9AEE` and error 3; the fixed
helper produces `$B2`, `$8E`, `$C300` and enables 80 columns on first boot.
Tests also cover vTW, physical auxiliary RAM, disabled Appletini RAM,
delayed RAM acknowledgement, warm reloads and startup cancellation.
Run `scripts/test_native_slot3_boot.py` and `scripts/test_vtw_boot_start.py`.

The rebuilt image is `firmwares/firmware-F1.3.0/FIRMWARE_F1.3.0.BIN`.
Its SHA-256 is
`ff1e1574defce3bd3ac55c756013274ccb959556c684af9fb0846dfe457db9f8`.
The package includes build audits, source archives and `TEST_RESULTS.md`.
