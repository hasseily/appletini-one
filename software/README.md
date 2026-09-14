# Apple II firmware inputs and test media

Standalone applications now live in
[appletini-software](https://github.com/hasseily/appletini-software):

- [Appletini Demos](https://github.com/hasseily/appletini-software/blob/main/demos/appletini_demos/README.md):
  the showcase disk, launcher, network apps, sources, and builders.
- [AUXSTRESS / AUXTOOLS](https://github.com/hasseily/appletini-software/blob/main/diagnostics/aux_memory/README.md):
  auxiliary-memory diagnostics.
- [AD8088 tests](https://github.com/hasseily/appletini-software/blob/main/diagnostics/ad8088/README.md):
  monitor, memory, and coprocessor diagnostics.
- [Appli-Card CP/M](https://github.com/hasseily/appletini-software/blob/main/diagnostics/applicard/README.md):
  programs, compatibility media, and disk tools.
- [Appletini detection](https://github.com/hasseily/appletini-software/blob/main/examples/detect_appletini/README.md):
  the standalone SmartPort GETDIB example.

The obsolete SHR_Test disk, builder, and `shr_testimages/` corpus were deleted.

Boot-menu, mouse-card, and `applicard/APPLICARD.ROM` sources remain firmware
inputs. Shared base disks and `legacy_demo_images/` / `shr4_demo_images/`
remain as inputs for diagnostics and renderer regression tests. The renderer
tools and hardware tests stay in this repository.

Border/overlay and AD8088 source checks read the migrated software from a
sibling `appletini-software` checkout. Set `APPLETINI_SOFTWARE_ROOT` when it
is elsewhere. Firmware build inputs remain local to this repository.
