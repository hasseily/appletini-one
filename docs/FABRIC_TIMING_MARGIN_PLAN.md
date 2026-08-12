# Fabric Timing Margin Plan

## Goal

Raise intrinsic fabric timing margin at 133.333 MHz. A timing candidate may
become the incremental reference only after it meets all of these gates:

- Two clean, full, non-incremental builds of the same Git commit pass in a row.
- Each build has setup WNS of at least `+0.300 ns` and setup TNS of `0`.
- Each build has nonnegative hold WNS and hold TNS of `0`.
- Each build has nonnegative pulse-width slack and no failing endpoints.
- Each build has no unconstrained internal endpoint, route error, bus-skew
  error, or XDC query that matched no object.
- The exact firmware made from the first passing checkpoint passes the full
  hardware test list.
- The checkpoint, bitstream, XSA, firmware, reports, commit, and tool version
  have saved hashes or IDs.

The `+0.300 ns` setup target gives useful build margin. Two runs with Vivado's
default seed show repeatability on one tool version and host. They do not prove
that all seeds or later Vivado releases will pass. Record both facts and rerun
the gate after a tool change.

## Confirmed Starting Point

The prior `+0.012 ns` result came from a full build. It did not use the known
good checkpoint. Its final timing was:

| Check | Result |
| --- | ---: |
| Setup WNS | `+0.012 ns` |
| Setup TNS | `0.000 ns` |
| Hold WNS | `+0.048 ns` |
| Hold TNS | `0.000 ns` |
| Pulse-width WNS | `+0.265 ns` |

The worst setup path starts at `ab_read_r.addr[11]` and ends in the Apple cycle
capture FIFO RAM. It has about `6.685 ns` of data delay, ten logic levels, and
about 79 percent route delay. The next two paths end at a Disk II
`drive_bit_offset` clock enable and have 15 logic levels. A fix to the capture
path can expose Disk II as the next limit, so each change needs a new top-path
review.

The prior resource report showed 10,223 slices, 30,465 LUTs, 20,153 registers,
74 block RAM tiles, and 1,001 control sets. Vivado reported a minimum of 994
control sets. This leaves little gain from a broad control-set rewrite unless a
later report shows a worse result.

The SmartPort C8 ROM already maps to block RAM. The SSI263 F2 table does not.
The bus write arbiter has 12 clients; vTW is client 11. Most clients already
register their requests.

## Build Records and Promotion

Each build gets an immutable directory under:

```text
.timing_runs/<UTC>-<commit>-<full-or-incremental>/
```

The directory contains a manifest, the final checkpoint, bitstream, XSA, and
reports. `.timing_runs/builds.csv` stores one row per attempt.
`.timing_runs/paths.csv` stores the top ten setup paths, including full start
and end names, cell types, clocks, delay split, route share, logic levels, and
fanout. Failed attempts also get a manifest and CSV row.

The manifest records:

- Git commit and dirty state.
- Vivado version, device, speed grade, build mode, and reference checkpoint.
- Synthesis and implementation strategies and key directives.
- Timing, failing endpoints, route status, bus skew, and constraint warnings.
- Slice, LUT, register, block RAM, DSP, and control-set use.
- SHA-256 hashes for the checkpoint, bitstream, and XSA.

The build script keeps a named checkpoint and XSA in each build directory for
test firmware. It does not replace the known-good checkpoint. Promotion takes
two named build IDs. It checks that they are the two latest consecutive full
builds of one clean commit and that both pass every timing gate. It also checks
an immutable hardware record tied to the first checkpoint and tested firmware.

## Phase 0: Measure the Design

Status: complete for the initial baseline.

1. Add immutable build manifests, `builds.csv`, and `paths.csv` to the normal
   build script.
2. Record setup, hold, pulse width, failing endpoints, route and bus-skew
   status, missing XDC objects, resources, control sets, strategies, tool
   version, and hashes.
3. Add an optional diagnostic set:
   `report_design_analysis -congestion`, `report_high_fanout_nets`, and
   `report_qor_suggestions`. Always write timing, use, control-set, route,
   bus-skew, clock, method, and constraint reports.
4. Add a promotion script that enforces the two-build and hardware gates.
5. Add direct simulator tests at the Apple renderer and SDD capture FIFO
   boundaries before changing their timing.
6. Run a clean full build with diagnostics. Treat its reports as the campaign
   baseline and rank path classes by delay, route share, fanout, and count.

Run the baseline from a clean commit:

```powershell
$env:APPLETINI_FULL_BUILD = "1"
$env:APPLETINI_TIMING_DIAGNOSTICS = "1"
vivado -mode batch -source scripts/build_and_export_xsa.tcl
Remove-Item Env:APPLETINI_FULL_BUILD
Remove-Item Env:APPLETINI_TIMING_DIAGNOSTICS
```

Do not compare WNS alone. Compare the top path class, setup path count, hold,
route share, LUT levels, use, and control sets.

### Phase 0 Baseline Result

The clean full build `20260812T153728Z-7342c1a5-full` used commit
`7342c1a51dd9b1e8b29220e276b0094d885b3038` and Vivado 2025.2. It used no
incremental reference and no rescue pass. It produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.012 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.048 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.496 ns` |
| Route errors | `0` |
| Unconstrained internal endpoints | `0` |
| Missing XDC objects | `0` |

The final design used 10,226 slices, 30,465 LUTs, 20,153 registers, 74 block
RAM tiles, six DSPs, and 1,001 control sets. The routed design had no congestion
window above Vivado level 5. Vivado gave no QoR suggestion because timing met.
This makes broad floorplanning and control-set work lower priority than the
measured path classes.

The top ten setup paths were:

| Rank | End block | Slack | Route share | Levels | Max fanout |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Apple capture FIFO RAM | `+0.012 ns` | 78.6% | 10 | 48 |
| 2-3 | Disk II `drive_bit_offset` enable | `+0.015 ns` | 72.8% | 15 | 81 |
| 4-5 | Apple capture FIFO RAM | `+0.050` to `+0.073 ns` | about 78% | 10 | 48-54 |
| 6 | Apple bus direction output | `+0.079 ns` | 44.7% | 5 | 11 |
| 7-9 | Apple cycle egress space logic | `+0.079 ns` | 57.9% | 13 | 8 |
| 10 | Disk II `drive_phase` enable | `+0.108 ns` | 74.0% | 14 | 81 |

The first five and tenth paths start at `ab_read_r.addr[11]`. The capture path
passes through slot-7 overlay qualification, pending-record selection, and the
FIFO input before it reaches block RAM. The Disk II path passes through slot-7,
boot-menu, vTW Disk II, and drive-state logic. The capture register stage should
remove three top-ten paths, but Disk II will then set WNS near `+0.015 ns` unless
placement changes. Review the Disk II path before assuming bus-snapshot copies
alone can add the remaining margin.

## Phase 1: Remove Route-Heavy Capture Paths

### 1. Register Capture Records

Add one record-assembly stage before each FIFO write path:

- Apple cycle renderer capture.
- SDD capture.

Register the complete source record and a valid bit at the capture event. Feed
the current collision, pending-event, loss, gap, storm, and jam rules from that
stage. Do not delay only `wr_en`; data and all event state must move together.

The tests must prove:

- Normal Apple writes keep address, data, and flags together.
- Frame events keep their order against Apple writes.
- I/O events that arrive first stay first.
- Pending collisions keep both events without duplication.
- A full FIFO sets the same sticky drop and overlay-loss state.
- Acknowledge and a new drop in one clock use set-wins behavior.
- C029, SHR mode, reset, and overlay flags stay on the same record.
- SDD records keep all address, data, and state bits.
- Storm and jam records keep the last intended record and count.
- Disable and reset clear the same state as before.

One extra 133 MHz cycle is about 7.5 ns. CPU1 drains these queues much later,
so this latency should not affect visible behavior. The full regression and
hardware tests still decide acceptance.

After this change, run a full build. Accept it only if the old capture path
class leaves the top paths and all counts remain exact. If Disk II becomes the
new limit, record that result before the next edit.

#### First Capture-Pipeline Build

Commit `e26205d08f3a2fdfcb7bc00d295d669a3f09c537` added the one-cycle
record stage to both capture FIFOs. The direct simulations proved consecutive
records, I/O-before-Apple order, pending-record handling, full-FIFO loss,
set-over-ack behavior, reset, SDD storm and jam trigger records, and exact
record data. The VidHD/SHR, Apple egress, SDD stream, and linear-overlay tests
also passed.

The clean full build `20260812T160459Z-e26205d0-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.006 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.037 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.904 ns` |
| Route errors | `0` |
| Unconstrained internal endpoints | `0` |
| Missing XDC objects | `0` |

No capture FIFO endpoint remained in the top ten. The change removed the
target path class, but it did not raise total margin. The new leading paths
were:

| Rank | End block | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1 | vTW 65C02 `a_q[1]` to `p_q[1]` | `+0.006 ns` | 62.0% | 13 |
| 2 | Apple DMA `line_idx_q[2]` to PSRAM write data | `+0.037 ns` | 72.0% | 9 |
| 3-4 | Disk II `drive_phase` | `+0.044` to `+0.050 ns` | 77-79% | 11-12 |
| 5 | vTW Apple-bus direction output | `+0.057 ns` | 48.2% | 3 |
| 6-7, 9 | SmartPort output FIFO address | `+0.063` to `+0.101 ns` | about 76% | 12 |

The build used 10,072 slices, 30,541 LUTs, 20,282 registers, 74 block RAM
tiles, and 1,008 control sets. Against the baseline, slices fell by 154 while
LUTs rose by 76, registers rose by 129, and control sets rose by seven. The
build used the normal post-route `Explore` pass and no extra rescue pass.

Keep the capture stage because it removes the measured long decode-to-BRAM
path and its tests pass. Do not promote this build. Trace the new 65C02,
DMA/PSRAM, Disk II, and SmartPort paths before the next edit. A local vTW
Disk II enable register is a better first cut for the measured 11- to 12-level
Disk II cone than a broad fanout setting. Bus snapshot copies may still help
the SmartPort paths and the remaining common route delay, but fresh reports
must show the useful copy points.

### 2. Limit Apple Bus Snapshot Fanout

First try a measured `MAX_FANOUT` limit on the bus snapshot source. Check the
post-route net report to make sure Vivado made useful local copies.

If that does not work, make explicit copies in `apple_bus_wrapper` for these
consumer groups:

- Card and slot logic.
- Cycle and SDD capture.
- vTW and video state.

Copy the completed snapshot register. Do not add a cycle to the bus event or
copy a partly assembled bus word. Keep each copy near its consumers. Use
`DONT_TOUCH` only on the copy registers that Vivado would merge; do not put it
on the full cones.

Measure this as its own commit and full build. Remove the copies if they raise
LUT use or create a worse path without a clear timing gain.

## Phase 2: Reduce Congestion

### 1. Move the SSI263 F2 Table to Block RAM

The two audio back ends use the same 4,096 by 16-bit F2 data. Replace duplicate
LUT ROMs with one inferred true dual-port block RAM when both ports need an
independent read. Register the outputs and add the read cycle to the coefficient
load state. Audio sample work has thousands of fabric clocks available, so one
read cycle should fit. Prove address and coefficient order in simulation.

Do not rewrite the SmartPort C8 ROM for this goal. It already uses block RAM.
An inference guard or test is enough unless a later build changes that result.

Measure block RAM, LUT, F7/F8, slice, and timing changes. Keep the edit only if
it frees useful logic or improves placement without an audio fault.

### 2. Test Control-Set Settings

Run a full build with a higher synthesis control-set optimization threshold.
Change no RTL in that experiment. Compare control sets, slices, LUTs, WNS, and
the top paths. Keep the setting only if it gives repeatable gain.

For new RTL, prefer a shared enable and a synchronous reset when the behavior
allows it. Do not remove a needed reset or merge unrelated enables just to cut
the count.

### 3. Test Synthesis Retiming

Test `STEPS.SYNTH_DESIGN.ARGS.RETIMING true` as one flow-only commit. Run all
CDC and source checks. Review every XDC query after retiming; hierarchy-based
queries may no longer bind. Keep retiming only if both timing and function pass.

### 4. Sweep Place Directives

Wait until the Phase 1 and ROM netlists stop changing. Then run a small full
build sweep with one placement directive per build. Keep synthesis, route, and
physical optimization fixed. Record every result, including failures. Use the
best repeatable result as a candidate; do not select one lucky build by WNS
alone.

## Phase 3: Use Only if the Target Still Fails

### 1. Stage Bus Write Arbitration

Review fresh timing paths first. Most of the 12 clients already latch their
requests. If the final priority and response mux becomes critical, add one
fixed output stage after arbitration. Keep client numbering stable and check
vTW client 11 first. Update fixed-latency taps and assertions with the same
commit.

The Apple bus has enough fabric cycles for the added delay, but the HDL and
hardware tests must prove write order, pulse width, and vTW timing.

### 2. Add Small Pblocks

Use Phase 0 congestion reports to choose a target. Start with a soft pblock for
the capture FIFO and its local logic. Check route use and hold after each
change. Do not bind all vTW shadow RAM at once; it owns many block RAMs and can
make placement worse.

### 3. Keep 125 MHz as a Separate Fallback

Changing 133.333 MHz to 125 MHz adds 6.25 percent to the period. It also changes
vTW rates, PSRAM service, timeouts, counters, and any hard-coded fabric-clock
math. Treat it as a clock and firmware change, not a timing directive. List and
retune every clock-derived constant, then run the full test and hardware list.
Do not take this step unless the structural work fails.

## Checks for Each Change

Each structural change gets its own commit. Run the focused test first, then
the source regressions that cover the touched block. Before accepting a timing
candidate, run at least:

```powershell
python scripts/test_capture_fifos.py
python scripts/test_apple_cycle_egress.py
python scripts/test_sdd_stream.py
python scripts/test_linear_text_overlay.py
python scripts/test_vidhd_shr.py
```

Also run the card tests for any changed bus or arbiter path, including
SmartPort, Disk II, Uthernet, SSC, Mockingboard, and vTW checks.

For a hardware candidate, build Vitis and the exact firmware from its named
XSA and bitstream:

```powershell
python scripts/package_timing_firmware.py <tested-build-id>
```

The packer writes `FIRMWARE.BIN` and a component-hash manifest inside the
immutable timing-run directory. Test that file, not a root-level firmware from
another build.

Test boot menu, Disk II boot, SmartPort read and write, vTW, Mockingboard audit,
linear overlay, SDD, Uthernet, SSC, and reset behavior. Record the exact test
firmware only after all pass:

```powershell
vivado -mode batch -source scripts/mark_timing_hardware_validated.tcl `
  -tclargs <tested-build-id> `
  boot-menu,disk-ii,smartport,vtw,mb-audit,linear-overlay,sdd,uthernet,ssc,reset
```

Run the second clean full build from the same commit after hardware validation.
Promote only the two latest consecutive passing full builds:

```powershell
vivado -mode batch -source scripts/promote_timing_candidate.tcl `
  -tclargs <tested-build-id> <confirm-build-id>
```

## Commit and Stop Rules

- Put one structural change in each commit.
- Do not mix RTL, a flow experiment, and a floorplan change.
- Keep all failed build records. Revert a failed experiment in a new scoped
  commit or before starting the next one.
- Do not promote a new fabric feature while setup WNS is below `+0.300 ns`.
- A correctness or safety fix may proceed below the margin bar, but it must use
  a full build and hardware test and must not become the timing reference until
  it passes the normal gate.
- Stop a phase when two clean full builds and hardware tests meet the target.
  Do not add later timing changes without a measured need.

Expected useful margin is `+0.300 ns` to about `+0.800 ns`. The measured gates,
not that estimate, decide when the campaign ends.
