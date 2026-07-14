# Appletini One

Appletini One is firmware and FPGA logic for an Apple //e expansion card built
around a Xilinx Zynq-7020. The PL handles cycle-sensitive Apple bus behavior;
the two ARM cores provide storage, video rendering, USB services, configuration,
and higher-level peripheral emulation.

## Virtual Hardware

- SmartPort storage and Disk II disk-image support
- PCPI Appli-Card compatible Z80 coprocessor in slot 5
- Phasor/Mockingboard audio with SSI-263/SC-01 speech
- Uthernet II compatible W5100 interface
- Mouse card, RamWorks memory, and no-slot clock
- VidHD-compatible video controls and SuperSprite graphics
- Accurate Apple //e video capture, HDMI output, borders, scanlines, and profiles
- USB keyboard, mouse, storage, firmware update, and SuperDuperDisplay support

The boot menu configures virtual cards, storage images, video, audio, networking,
USB behavior, and named profiles. Settings are stored on the card's SD volume.

## Repository Layout

- `hdl/`: SystemVerilog/VHDL sources and the active source manifest
- `hdl/constraints/`: board pin and timing constraints
- `ps_sources/`: golden updater, frontend, renderer, and shared bare-metal code
- `scripts/`: Vivado, Vitis, image-generation, programming, and regression tools
- `software/`: Apple II demos, diagnostics, and disk images
- `tools/mcp/`: serial control server for automated Apple II interaction
- `Datasheets/`: hardware and compatibility references
- `third_party/`: external components with their own license terms

`project/` and `vitis_workspace/` are generated and are not source directories.

## Toolchain

- Vivado 2025.2
- Vitis 2025.2
- A compatible JTAG programmer for hardware generation or direct flash recovery
- UART access for diagnostics and serial firmware recovery

## Build

Generate the Vivado project, build the bitstream/XSA, and build all Vitis apps:

```bat
vivado -mode batch -source scripts/create_project.tcl
vivado -mode batch -source scripts/build_and_export_xsa.tcl
vitis -s .\scripts\create_vitis_workspace.py
```

Create the two flash images:

```bat
scripts\make_boot_bin.bat
scripts\make_firmware_bin.bat
```

- `BOOT.BIN` contains the FSBL and golden updater.
- `FIRMWARE.BIN` contains the FSBL, PL bitstream, CPU1 renderer, and frontend.

C-only frontend changes require a Vitis rebuild and a new `FIRMWARE.BIN`; HDL,
clock, AXI, or constraint changes require the full Vivado and Vitis sequence.

See [README_VIVADO.md](README_VIVADO.md) for hardware-build details and
[README_BOOT_UPDATE.md](README_BOOT_UPDATE.md) for the flash layout and recovery
flow. Script-specific usage is in [scripts/SCRIPTS_README.md](scripts/SCRIPTS_README.md).

## Firmware Installation

The normal update path is:

1. Put `FIRMWARE.BIN` in the root of the card's SD volume.
2. Reboot into the golden updater.
3. Let the updater program and verify the firmware slot.
4. Confirm that it renames the file to `FIRMWARE.OK` and boots the frontend.

Serial recovery is available through the golden monitor:

```bat
python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
```

Direct QSPI programming scripts are available for recovery and bring-up:

```bat
scripts\program_boot.bat .\BOOT.BIN
scripts\program_firmware_slot.bat .\FIRMWARE.BIN
```

## Validation

Source-level regression scripts live in `scripts/test_*.py`. Hardware behavior
is additionally checked through UART/JTAG and the serial MCP tools. Generated
images report their firmware, golden-updater, and RTL versions at runtime so a
mixed build is visible immediately.

The Appli-Card interface and demo media are documented in
[README_APPLICARD.md](README_APPLICARD.md). Apple II software can identify the
card through the documented SmartPort interface described in
[README_APPLETINI_DETECTION.md](README_APPLETINI_DETECTION.md).

## Licensing

External components and bundled compatibility software retain their own
license and attribution files. No repository-wide license is granted by this
README.
