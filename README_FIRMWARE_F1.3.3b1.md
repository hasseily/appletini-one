# Firmware F1.3.3b1

F1.3.3b1 fixes the frontend stack reservation in the standard Vitis build.
It raises the stack from 8 KiB to 64 KiB and checks the linked ELF before
packaging. The heap stays at 64 MiB.

This is an ARM-only prerelease. It retains the exact F1.3.2 FPGA bitstream,
XSA, FSBL and CPU1 image. Golden B2.0.6 stays unchanged. The GitHub release
has one uploaded asset: `FIRMWARE.BIN`.

## Stack defect

The shipped F1.3.2 frontend reserves only 8,192 bytes for its main stack.
Its compiled `main`, `bind_platform` and `config_menu_load_settings` frames
need at least 14,048 bytes together, before filesystem calls. A nested
configuration save needs still more space. That path crosses the reserved
stack boundary into the heap address range.

The audited multiboot builder already requested a 64 KiB stack, but the
standard workspace builder only set the heap size. The legacy packager
could then accept the default 8 KiB stack without checking it.

Both builders now share the same stack and heap contract. They audit the
linked frontend ELF. The legacy packager also checks the selected frontend
before Bootgen runs. The audit rejects missing or malformed ELF symbols,
small or mismatched reservations, and stack/heap overlap.

## Image and inputs

Local package: `firmwares/firmware-F1.3.3b1/FIRMWARE.BIN`

- Size: 4,493,484 bytes; firmware slot: 6,291,456 bytes.
- SHA-256: `a2ba26c42cb3a88fc18b174d16d5f9b9a1f624f6b084b4b4aa6055e5f1982775`.
- Main stack: `0x04B0A190` through `0x04B1A190` (64 KiB).
- Heap: `0x00B0A190` through `0x04B0A190` (64 MiB).

Retained F1.3.2 inputs:

| Input | SHA-256 |
| --- | --- |
| FPGA BIT | `94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34` |
| XSA | `a08f244f95072b39d180b6f905e64d260a0a1a2b7f0d28de43baee783c4e9e23` |
| FSBL ELF | `7e47637e290b70701381d8ac3c91c267129dde7e4b12e172e76b4634786f7079` |
| CPU1 ELF | `95a07772c323da517dbfa57471ab871e59c82759e49f682b8581314bc4d1bad3` |
| Embedded CPU1 binary | `aeb77a4c559d95e0241acbc2ffba62732ae5082e884220292e805188039294d2` |

No FPGA synthesis or implementation ran for this release. The retained
full run is `20261021T142320Z-8af06741-full`: setup +0.047 ns, hold +0.034 ns,
pulse width +0.265 ns, with no failing endpoints. Its setup slack remains
below the audited packager's 0.050 ns promotion gate. This release uses
explicit retained inputs with the legacy packager; it does not change that
gate or claim a new FPGA timing result.

## Validation

- Vitis 2025.2 CPU0 build passed; final ELF has a 64 KiB stack and 64 MiB heap.
- Eleven memory-audit tests passed. The actual F1.3.2 ELF is rejected.
- Both Windows packaging tests passed, including six batch executions that
  check the selected ELF rather than the default workspace ELF.
- Fourteen multiboot glyph-build tests and the hardware-input audit suite
  passed. Image-manifest tests passed.
- The browser host harness passed eight behavior groups and five source
  guards. An in-memory test of the bundled FatFs passed 100 exFAT cycles
  with 128 KiB clusters, long directory names, parent navigation and config
  temporary-file/backup writes. These tests do not exercise the SD hardware.
- The final image audit passed: Bootgen partitions and load bytes, selected
  FPGA/XSA identity, embedded CPU1, version label, Appletini product ID,
  recovery flag, CRC and the current firmware-slot size limit.
- An extra `test_exfat_support.py` source check still fails because it looks
  for an old updater size-check expression absent from unchanged F1.3.2
  source. This release does not alter that updater.

Physical-board validation is pending. The stack defect is confirmed, but
its link to the reported garbled configuration and `FR=2` is not yet proven.
Retest the reported cold-boot SmartPort/Phasor folder navigation with this
image and a known-good configuration on the same exFAT card.
