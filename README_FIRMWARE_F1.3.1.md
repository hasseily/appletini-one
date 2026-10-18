# Firmware F1.3.1

F1.3.1 adds a warm-reset shortcut to the USB keyboard in standalone ONE//e.

- **Ctrl+Delete** requests a warm Control-Reset.
- **Ctrl+Alt+Delete** requests a cold Control-Open-Apple-Reset.

Both shortcuts use forward Delete, not Backspace. Each chord fires once while
held. Releasing its keys does not trigger another reset. These shortcuts apply
only to an active standalone ONE//e session.

This ARM-only release rebuilds both ARM frontend cores and retains the existing
FPGA bitstream, XSA and FSBL. It needs no HDL change or FPGA binary rebuild.
Golden B2.0.6 stays unchanged.

Download the firmware from the
[F1.3.1 release](https://github.com/hasseily/appletini-one/releases/tag/F1.3.1).
The local audited package is
`firmwares/firmware-F1.3.1/FIRMWARE.BIN` (4,469,996 bytes).
Its SHA-256 is
`0d57f4dc89f70b2f7ecc3804ed6a8ddd236633b21851e24e97630d2c63975647`.

Vitis 2025.2 built both ARM frontend cores. The input regressions, reset
simulations, configuration tests and package tests passed. The package audit
checked the loaded version, image partitions, recovery and product markers,
CRC, embedded CPU1 bytes, linked memory limits and selected FPGA/XSA identity.
The package includes `ARM_BUILD.json`, `FIRMWARE_AUDIT.json`, source archives,
checksums and `TEST_RESULTS.md`.

The retained FPGA timing record reports setup slack +0.054 ns, hold slack
+0.064 ns and pulse-width slack +0.265 ns, with no failing endpoints or route
errors. These are the existing FPGA build's results; this release ran no new
FPGA build.

On 2026-10-18, the user confirmed that this F1.3.1 image works on hardware.
That confirmation covers the image with the SHA-256 shown above. The host
tests and build audit provide separate evidence.
