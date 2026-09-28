# Build and Utility Scripts

Run scripts from the repository root unless a script's help says otherwise.

The Appletini demo-disk, HGR asset, SuperSprite builders, and demo/network
software tests moved to
[appletini-software](https://github.com/hasseily/appletini-software/blob/main/demos/appletini_demos/README.md).
The AUX, AD8088, and Appli-Card software builders and CP/M disk utility
also moved there, under `diagnostics/`. The obsolete SHR_Test disk,
builder, and exclusive images were removed.

The border/overlay and AD8088 source checks use the sibling software
checkout; override its location with `APPLETINI_SOFTWARE_ROOT`.

## Hardware

- `create_project.tcl`: recreate the Vivado project from `hdl/hdl_sources.txt`.
- `build_and_export_xsa.tcl`: synthesize, implement, write the bitstream, and
  export `project/appletini_yarz_top.xsa`.
- `create_vitis_workspace.py`: recreate the Vitis platform, golden updater,
  CPU1 renderer, and frontend applications from `ps_sources/`.

```bat
vivado -mode batch -source scripts/create_project.tcl
vivado -mode batch -source scripts/build_and_export_xsa.tcl
vitis -s .\scripts\create_vitis_workspace.py
```

Normal builds start from current synthesis with no incremental reference.
Recreate the project when its source list or block design changes. To request
an incremental comparison, name a tested checkpoint explicitly:

```powershell
$env:APPLETINI_INCREMENTAL_REF_DCP = ".vivado_cache/appletini_yarz_top_known_good.dcp"
vivado -mode batch -source scripts/build_and_export_xsa.tcl
Remove-Item Env:APPLETINI_INCREMENTAL_REF_DCP
```

Set `APPLETINI_TIMING_DIAGNOSTICS=1` on a full build to add congestion,
high-fanout-net, and QoR reports. Each build writes an immutable record under
`.timing_runs/<build-id>/`. The two CSV files at the root of that directory
track build results and the top ten setup paths.

`configure_vivado_run_profile.tcl` gives new and existing projects one fixed
profile: default synthesis, Explore logic optimization, Default placement,
Explore pre-route physical optimization, and Explore routing. It clears old
hooks and extra options and sets eight worker threads in each run. `-jobs 8`
alone does not set the worker count. Full post-route Explore, forced TNS
cleanup, and automatic rescue passes are disabled.

Before logic optimization, the flow applies a temporary `0.200 ns` fabric
setup margin and tightens the Apple direction limits by the same amount.
After routing, `finish_video_timing.tcl` applies `0.200 ns` to the pixel clock
and runs routing, cell and pin optimization only for that clock's paths.
It then restores both clocks and the Apple output requirements. Applying
the pixel margin before placement was measured and rejected.

The build reopens the final design and checks both clocks' nominal
uncertainty, the original board requirements, all 32 Apple/Gray-pointer
bounds, and the full timing/route reports. Export requires global setup
WNS of at least `+0.150 ns`, nonnegative hold and pulse width, and no failing
or unconstrained internal endpoint. Never package a bitstream from a run
that stopped at that gate. See `README_VIVADO_RUNTIME_AUDIT.md` for the trials.

Promotion requires two consecutive clean full builds of the same commit. Both
must have setup WNS of at least `+0.150 ns`, nonnegative hold and pulse width,
no timing failure, no bad route or bus skew, no missing XDC object, and no
extra rescue pass. Both builds must also use the same Vivado version and the
same synthesis, placement, route, and physical-optimization settings. Package
the first build's exact XSA and bitstream, test its named firmware on hardware,
then bind that firmware hash to the build:

```powershell
python scripts/package_timing_firmware.py <tested-build-id>

vivado -mode batch -source scripts/mark_timing_hardware_validated.tcl `
  -tclargs <tested-build-id> `
  boot-menu,disk-ii,smartport,vtw,mb-audit,linear-overlay,sdd,uthernet,ssc,reset

vivado -mode batch -source scripts/promote_timing_candidate.tcl `
  -tclargs <tested-build-id> <confirm-build-id>
```

Promotion copies the tested build's checkpoint to the known-good incremental
reference. See `docs/FABRIC_TIMING_MARGIN_PLAN.md` for the full process.

Run the timing-tool tests after changing this flow:

```powershell
vivado -mode batch -source scripts/test_timing_tooling.tcl
python scripts/test_timing_margin_hooks.py
python scripts/test_vivado_run_profile.py
python scripts/test_timing_manifest_format.py
python scripts/test_timing_firmware_packaging.py
# After a normal full build; checks saved properties without changing them:
vivado -mode batch -source scripts/test_timing_run_properties.tcl
```

## Images and Programming

- `make_boot_bin.bat`: create manifest-appended `BOOT.BIN` from the FSBL and
  golden updater; fail if the final file exceeds a 1 MiB golden slot.
- `make_firmware_bin.bat`: create `FIRMWARE.BIN` from the FSBL, bitstream,
  CPU1 renderer, and frontend, then append its manifest.
- `image_manifest.py`: append or verify the fixed image role, recovery, size,
  and CRC32 manifest.
- `program_boot.bat`: factory and bench recovery tool that programs `BOOT.BIN`
  at QSPI offset `0x00000000`; it is not the field-update path.
- `program_firmware_slot.bat`: program `FIRMWARE.BIN` at `0x00200000`.
- `serial_firmware_update.py`: upload `FIRMWARE.BIN` through the golden UART
  monitor with XMODEM-CRC.

```bat
scripts\make_boot_bin.bat
scripts\make_firmware_bin.bat
python scripts\image_manifest.py verify .\BOOT.BIN --role golden --require-recovery-capable
python scripts\image_manifest.py verify .\FIRMWARE.BIN --role firmware --require-recovery-capable
python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
```

The serial updater requires pyserial. The normal field-update path installs
`FIRMWARE.BIN` from the SD root and verifies the complete programmed image.
`make_boot_bin.bat` passes `--max-size 0x100000`; this limit includes the
32-byte manifest.

The field path for every golden-boot update uses frontend Firmware F1.0.1 or
later. Boot the matching frontend, put `BOOT.BIN` in the SD root, stop USB or
FTP SD sharing, and enter `:selfupdate` on USB0. See the
[Golden Boot Update guide](../README_BOOT_UPDATE.md#golden-boot-update).

Self-update stages and checks the new file in golden B at `0x00100000`. B must
boot and validate itself before it copies the image to golden A at offset zero.
B stays valid as A's backup.

## Generated Apple II Artifacts

The `build_*` and `gen_*` scripts assemble ROMs and demo programs or convert
verified binary assets into C/SystemVerilog data. Generated source identifies
its generator and input where practical. Regenerate the artifact instead of
editing generated byte arrays by hand.

## Regression Checks

`test_*.py` scripts perform focused source, protocol, model, or simulator
checks. Most run directly with Python from the repository root:

```bat
python scripts\test_applicard_card.py
python scripts\test_uthernet2_card.py
python scripts\test_config_profiles.py
```

Run `python scripts\test_vtw.py` for the simulator-backed vTW gate. It includes
pin-level Disk II response timing, native and vTW raw WOZ read/write, and an
end-to-end vTW-core-to-Disk-II run at every speed preset plus the slug override.

Run `python scripts\test_axisimple_wrapper.py` after PS-to-PL AXI write-path
changes. It checks write-address/data skew, bursts, backpressure, reset, client
selection, and write-response order through the real AxiSimple wrapper.

Run `python scripts\test_psram_driver_iddr_reset.py` after PSRAM capture or
reset changes. It compares reset modes, both capture phases, a mid-read reset,
and the first read after reset through the real tape and input-DDR path.

Run `python scripts\test_ssi263_start_timing.py` after SSI263, Votrax, or
formant-start changes. It checks the same-edge backend start and VIA clear,
the saved phoneme tuple, reset cancellation, and formant pipeline restart.

Run `python scripts\test_ssi263_filter_finalize.py` after SSI263 formant MAC or
filter-pipeline changes. It checks every filter stage, exact accumulator and
history timing, saturation limits, reset/restart cancellation, and sample output.
Use `python scripts\sim_ssi263_formant_rtl.py --votrax ...` for direct SC-01
phone sweeps; omit `--votrax` for SSI263 mode.

Simulator-backed checks require the Xilinx simulation tools on `PATH`.
Hardware-facing scripts document their required UART, JTAG, SD, or USB setup in
their command-line help.

## Flash Layout

| Region | Offset | Size |
| --- | ---: | ---: |
| Golden A | `0x00000000` | `0x00100000` |
| Golden B | `0x00100000` | `0x00100000` |
| Firmware | `0x00200000` | `0x00DF0000` |
| Metadata | `0x00FF0000` | `0x00010000` |

Keep these offsets synchronized with
`ps_sources/bootloader/updater_layout.c` and `ps_sources/image_versions.h`.
