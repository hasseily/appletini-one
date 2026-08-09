# Apple II Software

This directory contains software for exercising Appletini One from the Apple II
side.

- `Appletini_Demos.po`: 32 MB ProDOS demo volume. Its AD8088 item boots
  Reboot Camp '83 MS-DOS 2.0 and runs the animated `HGRCUBE.COM` demo.
- `appletini_webserver/`: 6502-compatible HTTP server and client demos
- `applicard/`: PCPI CP/M media, Appli-Card ROM input, and validation programs
- `AD8088_Test.dsk` / `AD8088_Test.po`: virtual AD8088 monitor and RAM validation disks
- `*.a65`: ACME assembly sources for card, memory, video, and storage tests
- `*.dsk`, `*.do`, and `*.po`: bootable demo or compatibility-test media

The source files and disk images are test inputs, not part of the Zynq firmware
build. Build helpers in `scripts/` regenerate the Appletini-authored programs
that are included on the demo volume.
