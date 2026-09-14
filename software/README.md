# Apple II Software

This directory contains software for exercising Appletini One from the Apple II
side.

- [Appletini Demos](../../appletini-software/demos/appletini_demos/README.md):
  the 32 MB ProDOS volume, launcher, demo sources, network apps, and builders
  moved to appletini-software. Build it there; its README has the commands.
- `applicard/`: PCPI CP/M media, Appli-Card ROM input, and validation programs
- `AD8088_Test.dsk` / `AD8088_Test.po`: virtual AD8088 monitor and RAM validation disks
- `*.a65`: ACME assembly sources for card, memory, video, and storage tests
- `*.dsk`, `*.do`, and `*.po`: bootable demo or compatibility-test media

Boot-menu, mouse-card, and Appli-Card ROM sources remain firmware inputs.
The other programs and media here support diagnostics and compatibility tests.
Shared base disks and `legacy_demo_images/` / `shr4_demo_images/` remain
as inputs for those tests; the demo project has pinned copies.

The SHR builder and border/overlay source checks use the moved demo sources.
They default to a sibling `appletini-software` checkout; set
`APPLETINI_SOFTWARE_ROOT` when it is elsewhere. Firmware build inputs and
renderer image fixtures still live in this repository.
