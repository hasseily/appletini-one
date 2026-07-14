# Apple II Software

This directory contains software for exercising Appletini One from the Apple II
side.

- `Appletini_Demos.po`: 800 KB ProDOS demo volume
- `appletini_webserver/`: 6502-compatible HTTP server and client demos
- `applicard/`: PCPI CP/M media, Appli-Card ROM input, and validation programs
- `*.a65`: ACME assembly sources for card, memory, video, and storage tests
- `*.dsk`, `*.do`, and `*.po`: bootable demo or compatibility-test media

The source files and disk images are test inputs, not part of the Zynq firmware
build. Build helpers in `scripts/` regenerate the Appletini-authored programs
that are included on the demo volume.
