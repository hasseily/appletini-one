# Repository Guidelines

## Project Overview

Appletini One is an Apple //e expansion card based on the Xilinx Zynq-7020 SoC FPGA. The card plugs into the Apple //e bus and emulates multiple classic expansion cards in a single device, including:
- **Mockingboard**: Sound card providing music and audio capabilities
- **RGB Cards**: Enhanced video output cards
- **Mousecard**: Mouse interface support
- **Appli-Card**: PCPI-compatible Z80 coprocessor in slot 5
- **Storage and networking**: Disk II, SmartPort, and Uthernet II interfaces

This Xilinx Vivado FPGA design interfaces with the Apple //e bus, implements USB communication for modern connectivity, handles I2C peripherals (RTC, temperature sensors, PMIC), performs memory testing, and provides LED status indicators.

## Hardware Components

From `Datasheets/AppleTini_BOM.txt`, the card includes:

**Core SoC and Memory**:
- FPGA: Xilinx XC7Z020-2CLG484I (Zynq-7020, 85K logic cells, speed grade -2)
- DDR3L: Samsung K4B4G1646E-BYMA (2 chips, 1GB total, DDR3L-1866)
- PSRAM: Lyontek LY68L6400SLIT (2 chips, 16MB total, SPI/QPI interface)
- Flash: 128Mbit / 16MB Quad SPI NOR flash on production boards

**Connectivity**:
- Ethernet: WIZnet W5100S (10/100 Base-T, parallel MCU interface)
- USB PHY: Microchip USB3300-EZK-TR (2 chips, USB 2.0 ULPI)
- USB-UART: Silicon Labs CP2105-F01-GMR (dual UART bridge)

**Video and Audio**:
- DVI: Texas Instruments TFP410PAP (165MHz TMDS transmitter)
- Audio DAC: Asahi Kasei AK4493SEQ (32-bit stereo, 123dB SNR, supports up to 768kHz PCM)

**Peripherals**:
- RTC: PCF8563DTR (I2C real-time clock)
- Temperature: TMP102AIDRLR (I2C sensor, 12-bit resolution)

## Repository Structure

Key files:
- `Datasheets/Xilinx_Zynq-7020_FormalTruthTable.csv`: Complete pin mapping (347 pins)
- `Datasheets/AppleTini_BOM.txt`: Bill of materials with all major components

## Project Structure & Module Organization
- `hdl/`: SystemVerilog design sources (`appletini_yarz_top.sv` is the top-level PL wrapper).
- `hdl/constraints/`: Vivado constraints (`appletini_yarz.xdc` for pins/timing).
- `ps_sources/` : Vitis PS code sources.
- `scripts/`: Build automation and image/flash scripts (`create_project.tcl`, `build_and_export_xsa.tcl`, `make_*_bin.bat`, `program_*.bat`).
- `software/`: Apple II demos, diagnostics, and compatibility-test media.
- `project/` and `vitis_workspace/`: Generated Vivado/Vitis artifacts; regenerate, do not hand-edit.
- `Datasheets/` and root `README_*.md`: hardware references and workflow docs.

## Build, Test, and Development Commands
- `vivado -mode batch -source scripts/create_project.tcl`: create/regenerate Vivado project.
- `vivado -mode batch -source scripts/build_and_export_xsa.tcl`: run synth/impl and export `project/appletini_yarz_top.xsa`.
- `vitis -s .\scripts\create_vitis_workspace.py` : create the full vitis workspace with all the apps.
- `scripts\make_boot_bin.bat`: create golden boot image (`BOOT.BIN`).
- `scripts\make_firmware_bin.bat`: create firmware image (`FIRMWARE.BIN`).
- `scripts\program_boot.bat .\BOOT.BIN`: program golden boot image to QSPI base.
- `scripts\program_firmware_slot.bat .\FIRMWARE.BIN`: program firmware image to the firmware slot.
- `python scripts/test_applicard_card.py`: run the Appli-Card source checks.

## Agent-Runnable Hardware Workflows
- Codex may run `vivado -mode batch -source scripts/create_project.tcl` to create the PL project from scratch.
- Codex may run `vivado -mode batch -source scripts/build_and_export_xsa.tcl` to build the PL project.
- Codex may run `vitis -s .\scripts\create_vitis_workspace.py` to create and build the PS side.
- Codex may run `xsdb .\scripts\launch_amp.tcl` to run the frontend through JTAG; the Apple computer must be powered on.
- Codex may run the QSPI programming scripts for the 16MB Quad SPI flash; the card must be in programming mode.

## Repo-Specific Working Notes
- `vivado -mode batch ...` typically needs elevated execution in this environment because Vivado writes temp files outside the repo/sandbox.
- Treat `hdl/constraints/appletini_yarz.xdc` object queries as fragile: if hierarchy changes, verify there are no "No valid object(s) found" warnings in Vivado logs.
- Keep async clock groups and generated-clock constraints bound to stable object names/queries; timing failures can be false if constraints silently fail to bind.
- Prefer shared CDC/reset helpers in `hdl/` for new crossings (`cdc_bit_sync.sv`, `cdc_bus_sampled.sv`, `reset_sync.sv`) instead of ad hoc inline synchronizers.
- `cdc_bus_sampled.sv` is for debug/status sampling only (non-coherent multi-bit capture); do not use it for functional buses.

## Coding Style & Naming Conventions
- Use 4-space indentation and keep alignment readable in HDL port lists and C macros.
- SystemVerilog modules/files: `snake_case` (for example, `video_timing_gen.sv`).
- C files/functions: `snake_case`; macros/constants: `UPPER_SNAKE_CASE`.
- Prefer small, hardware-focused helper functions and explicit register names/offsets.
- Keep comments practical: explain hardware intent, clocks, reset behavior, and bus assumptions.

## Testing Guidelines
- Run the relevant `scripts/test_*.py` source regressions before building.
- Validate hardware changes with UART output, clock/debug TCL scripts, and JTAG load/run flow.
- Name ad-hoc tests descriptively (`test_<feature>.c`, `test_<feature>.bat`).
- For video or peripheral work, document observed behavior in a `README_*.md` update.

## Hardware Bring-up Notes
- USB3300 `RESET` pin is **active-high** (this is not `RESETB`).
- In normal USB3300 operation, keep `RESET` **low**; driving it high holds the PHY in reset.

## Commit & Pull Request Guidelines
- Use short, imperative commit summaries (for example, `Fix 1080p timing`, `Refactor video modules`).
- Keep commits scoped to one logical change (HDL, software, or tooling).
- PRs should include: purpose, affected paths, validation steps run, and hardware used.
- Link related issues and include logs/screenshots for UART/video/flash workflows when relevant.
