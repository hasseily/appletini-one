# Vivado Build Guide

The Vivado project is generated from source. Do not hand-edit files under
`project/`; update RTL, constraints, `hdl/hdl_sources.txt`, or the Tcl scripts
and regenerate the project.

## Requirements

- Vivado 2025.2
- Xilinx part `xc7z020clg484-2`
- Enough free disk space for synthesis, implementation, and exported hardware

## Build

From the repository root:

```bat
vivado -mode batch -source scripts/create_project.tcl
vivado -mode batch -source scripts/build_and_export_xsa.tcl
```

The first command recreates `project/appletini_yarz.xpr` from the source
manifest. The second runs synthesis and implementation, writes the bitstream,
and exports `project/appletini_yarz_top.xsa` with the bitstream included.

To inspect the generated project in the GUI:

```bat
vivado project/appletini_yarz.xpr
```

## Hardware and Vitis Must Match

The XSA defines the PS configuration, clocks, interrupt wiring, AXI address
map, and the bitstream used by Vitis. Regenerate the XSA and Vitis workspace
after changing any of these:

- RTL or the HDL source manifest
- XDC constraints
- processing-system configuration
- clocks, resets, interrupts, or AXI interfaces
- top-level ports or address assignments

Build the PS applications against the exported XSA:

```bat
vitis -s .\scripts\create_vitis_workspace.py
```

`scripts/make_firmware_bin.bat` packages the FSBL, bitstream, CPU1 renderer,
and frontend produced by this build chain.

## Constraints

`constraints/appletini_yarz.xdc` is the board-level source of truth for pins,
I/O standards, generated clocks, asynchronous clock groups, and external bus
timing. Hierarchical object queries can silently stop matching after an RTL
rename, so review the log for `No valid object(s) found` after every constraint
or hierarchy change.

Do not waive timing failures caused by an unmatched clock or path query.
Confirm that generated clocks bind to the intended objects and that clock-group
exceptions cover only truly asynchronous domains.

## Build Review

Before using an exported XSA for firmware:

1. Confirm synthesis and implementation completed without critical warnings.
2. Confirm timing is met for all constrained clocks.
3. Check that every XDC object query matched.
4. Review DRC output instead of suppressing new violations globally.
5. Confirm the exported XSA timestamp belongs to the completed implementation.
6. Regenerate Vitis outputs and package `FIRMWARE.BIN` from the same build.

The runtime firmware reports its image and RTL versions. A version mismatch
usually means the bitstream, XSA, platform, or frontend came from a different
build chain.
