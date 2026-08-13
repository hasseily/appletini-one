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

Current synthesis maps both 4,096 by 16-bit SSI263 F2 copies and the 2,048 by
8-bit SmartPort C8 ROM into LUTs. The earlier assumption that the C8 ROM used
block RAM was wrong. The bus write arbiter has 12 clients; vTW is client 11.
Most clients already register their requests.

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

#### vTW Disk II Gate Build

Commit `4a61407ff25d3e649a967ff854959f9f28383880` registered only the
boot-menu Disk II gate used by vTW. It kept the native handoff gate live. All
14 vTW benches, 19 standard Disk II tests, and 66 WOZ tests passed. The clean
full build `20260812T163431Z-4a61407f-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.004 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.057 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.366 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |

The long address-to-vTW Disk II path left the top ten. Local Disk II paths
now start at registered drive state and have at least `+0.100 ns` slack. Four
of the top ten paths start at `ab_read_r.addr[13]` and end in SmartPort or
Disk II; they have 46 to 74 loads and about 74 to 77 percent route delay. The
worst path is a Mockingboard response to the Apple data-direction pin at
`+0.004 ns`.

The design used 10,077 slices, 30,598 LUTs, 20,305 registers, 74 block RAM
tiles, and 1,053 control sets. Keep the local Disk II gate stage, but do not
promote this build. Test a synthesis `MAX_FANOUT` of 32 on the sampled 16-bit
Apple address register as one flow change. Do not apply it to the whole bus or
to the local capture record stage.

#### Apple Address `MAX_FANOUT` Build

Commit `3802754e1313f12d0a34a9e875a405ce103e2a02` set `MAX_FANOUT` to
32 on the 16 sampled Apple address registers. A post-synthesis cell check found
155 address-register cells instead of 16, so synthesis made 139 copies. It made
the most copies for the low address bits and only two cells for each of bits
11 through 15.

The clean full build `20260812T165450Z-3802754e-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.004 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.014 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.657 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |

Setup WNS did not change from the prior full build, while hold WNS fell from
`+0.057 ns` to `+0.014 ns`. The worst path still started at live
`ab_read_r.addr[11]` and ended in Disk II. Other address-to-Disk II and
address-to-SmartPort paths remained in the top ten. The build used 9,787
slices, 30,577 LUTs, 20,319 registers, 74 block RAM tiles, and 1,041 control
sets.

The broad limit did not make useful per-card groups and did not raise margin.
Commit `f75dc4d` removed it. Do not use a broad `MAX_FANOUT` rule on these
registers. If a later path calls for copies, make explicit same-cycle source
copies for named consumer groups, preserve the Apple bus sample edge, and
measure that change on its own.

#### Physical Response Stage Build

Commit `fbddd81997ee8c0c537ce64c6929aa006f886051` registered the
physical data-response tuple after the 12-client arbiter. Data, normal data
enable, address ownership, R/W, and the INH dependency move together. Address,
R/W, control-line requests, and the early DMA-window control remain live. The
staged byte also feeds the DMA window because both DMA clients hold it well
before that window opens.

The stage keeps the raw PHI0 release at the placed output LUT. It also stores
the INH dependency with an II+ saved read, so closing the machine-mode
interlock releases both INH and the live or held byte. The focused tests proved
late and back-to-back card responses, native and vTW-owned reads and writes,
II+ saved-byte hold, live and saved INH cutoff, reset masking, and the existing
AD8088 DMA window. The AD8088 bench still measured `158.8` to `323.8 ns`, with
zero ghost writes.

The clean full build `20260813T045644Z-fbddd819-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.003 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.045 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.376 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |

The card-to-data-direction path left the top paths. Against the prior accepted
netlist, slices fell from 10,077 to 9,752, LUTs fell from 30,598 to 30,558,
and control sets fell from 1,053 to 1,008. Registers rose from 20,305 to
20,314. Keep the stage for the removed output path and lower use, but do not
promote this build. Setup margin stayed flat because Disk II replaced the
removed path:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1-4 | Apple address to Disk II bit-offset enable | `+0.003 ns` | 79.8% | 10 |
| 5 | PS to Disk II underrun count | `+0.010 ns` | 77.5% | 5 |
| 6-9 | Disk II spin count to latch enable | `+0.014 ns` | 75.3% | 13 |
| 10 | Disk II spin count to step-delay enable | `+0.034 ns` | 77.2% | 12 |

Trace and cut these two Disk II enable classes before adding bus snapshot
copies. The new report does not support a broad address-copy change.

#### Disk II vTW Rotation Stage Build

Commit `03de4daec1d2d03a1c566bdffe669365ec97d0f1` registered the
Disk II rotation state only at the boundary into vTW speed control. Local Q7,
cache-miss, media-ready, and sequencer logic remain live. Motor and drive
changes come through physical Disk II cycles, so the new view settles well
before vTW can start another private cycle.

All 14 vTW benches, 19 standard Disk II tests, and 66 WOZ tests passed. The
boot-menu source checks passed 19 of 20 tests; the one failed source-order guard
is unrelated to this RTL change and existed before it.

The clean full build `20260813T052716Z-03de4dae-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.093 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.047 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.921 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Rescue used | `0` |

The spin-count-to-vTW-to-Disk II feedback paths left the top ten. The prior
Apple-address-to-Disk II paths also stayed out of the top ten in this placement.
The build used 9,916 slices, 30,540 LUTs, 20,285 registers, 74 block RAM tiles,
and 1,007 control sets. Against the response-stage build, LUTs fell by 18,
registers fell by 29, control sets fell by one, and slices rose by 164.

The new top paths are:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1 | Raw PHI0 input to data-direction output | `+0.093 ns` | 39.7% | 3 |
| 2-4 | vTW cycle address to record enable | `+0.112 ns` | 71.9% | 10 |
| 5 | vTW cycle address to record enable | `+0.135 ns` | 72.0% | 10 |
| 6-10 | Slot mapping to Disk II dirty-track enable | `+0.163 ns` | 76.9% | 11 |

Keep the rotation stage, but do not promote this build. The setup gain is
`0.090 ns`, and the target remains `+0.300 ns`. Review the fixed PHI0 release
route, then cut the vTW record-control and Disk II mapping cones. Do not add a
broad fanout rule: none of these paths starts at a high-load source.

#### Data-Direction Pad Build

Commit `d9cb0beefc688798046bfe52a6ae8dba0b4e8016` kept the existing
12 mA drive on `a2fpga_dir_d` and changed only its output slew from slow to
fast. This keeps the raw-PHI0 truth table and clock count unchanged. A routed
checkpoint test predicted `+0.624 ns` on the eight-nanosecond pad-to-pad bound.
All 14 vTW benches passed before the full build.

The clean full build `20260813T055125Z-d9cb0bee-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.036 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.066 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.388 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Rescue used | `0` |

The target path improved from `+0.093` to `+0.476 ns`, even though its route
delay rose from `3.136` to `3.284 ns`. The output-buffer delay fell by about
`0.53 ns`. Keep this constraint because the raw-PHI0 release path now clears
the campaign target without changing its logic. Hardware signoff must still
check the faster control edge for ringing.

Global WNS fell due to placement of a different path. The new top paths are:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1-2, 4-5, 10 | Slot state to SmartPort output-FIFO address | `+0.036` to `+0.207 ns` | 74-76% | 12-13 |
| 3 | Disk II track length to data latch | `+0.161 ns` | 75.9% | 13 |
| 6-9 | vTW cycle address to RamWorks data enable | `+0.205 ns` | 69.1% | 10 |

The build used 10,013 slices, 30,542 LUTs, 20,267 registers, 74 block RAM
tiles, and 1,008 control sets. Do not promote it. Cut the SmartPort FIFO
address path next, then the measured vTW and Disk II paths.

#### SmartPort FIFO Address Build

Commit `12dc4977418f0cc3e4e35812c63ebf772b6a716e` removed the same-edge
post-pop lookahead from the SmartPort output FIFO. The block RAM now reads
from the registered pointer. The word catches up one fabric clock after a
pop, before either the native Apple bus or vTW can issue its next DATA read.
The test bench covers a five-byte word crossing through both paths. All 14
vTW benches, 16 SmartPort service tests, six ROM tests, and 15 menu tests
passed before the full build.

The clean full build `20260813T061638Z-12dc4977-full` produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.017 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.030 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.915 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Rescue used | `0` |

The target SmartPort block-RAM address path left the top ten. Keep the cut.
The lower global WNS comes from a different machine-mode-to-data-direction
route under the ten-nanosecond output bound. The new top paths are:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1 | Machine mode to data-direction output | `+0.017 ns` | 55.5% | 3 |
| 2-5 | Disk II quarter-track to bit-offset enable | `+0.124 ns` | 75.3% | 13 |
| 6-7 | Apple address to Disk II WOZ shift enable | `+0.137 ns` | 77.9% | 11 |
| 8-10 | Disk II quarter-track to stream-position enable | `+0.159` to `+0.165 ns` | 75.2% | 13 |

The build used 9,909 slices, 30,567 LUTs, 20,289 registers, 74 block RAM
tiles, and 1,018 control sets. Do not promote it. First remove the live
machine-mode decode from the direction-output cone. Then cut the measured
Disk II rotation and native-event cones.

#### Bus Safety Interlock Copy Build

Commit `c647de77861ad7cadaf797d77da7be7e61db13ee` added a preserved,
same-edge copy of the machine INH safety flag for `apple_bus_wrapper`. The
copy keeps reset and machine-mode update behavior unchanged. All 14 vTW
benches passed before the full build.

The clean full build `20260813T063714Z-c647de77-full` used Vivado 2025.2,
no incremental reference, and no rescue pass. It produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.116 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.075 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.928 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Unconstrained internal endpoints | `0` |
| Rescue used | `0` |

The build used 9,826 slices, 30,541 LUTs, 20,296 registers, 74 block RAM
tiles, six DSPs, and 1,009 control sets. The new top paths are:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1-7 | vTW CPU state to private RamWorks-bank enable | `+0.116 ns` | 80.8% | 7 |
| 8 | vTW cycle address to card-control read data | `+0.140 ns` | 68.8% | 14 |
| 9 | Apple address to SmartPort input count | `+0.155 ns` | 67.6% | 14 |
| 10 | Disk II quarter-track to bit-offset enable | `+0.159 ns` | 74.4% | 13 |

The first path class starts at a CPU state bit with fanout 93 and ends at all
seven `ss_ramworks_bank` clock enables. Cut this measured class next by using
a private soft-switch apply pulse after the captured vTW cycle tuple is ready.

The copy improved placement, but it did not remove every old
machine-mode-to-data-direction route. The new local-copy path to `DIR_D` has
`+0.445 ns` slack, while the original machine-mode path through the write
arbiter remains at `+0.359 ns`. Neither path limits this build. Keep the copy,
but do not claim that it removed the old route. Do not promote this build;
setup margin remains below `+0.300 ns`.

#### vTW Soft-Switch Apply Build

Commit `bceefabda0ba6449a5fa5a42009c41f13abba786` moved only the
private vTW soft-switch manager to a one-clock apply stage. `X_CAPTURE` saves
the core address, read/write state, write data, and pre-access switch state.
`X_ROUTE` applies that saved tuple to the private manager. The shared
`ssm_pulse` and the `$C074` speed latch remain at `X_CAPTURE`.

The full-system test now checks both `$C071` and `$C073`: the RamWorks bank
stays old at `X_CAPTURE`, changes at `X_ROUTE`, and appears in the next access
snapshot. Its existing checks also kept `$C006/$C007`, the C3/CFFF claim and
release, double `$C083`, video switches, and `$C074` behavior intact. All 14
vTW benches passed. The RamWorks 8 MB contract test, capture-FIFO, cycle-egress,
SDD stream, linear-overlay, and VidHD/SHR regressions also passed.

The clean full build `20260813T071806Z-bceefabd-full` used Vivado 2025.2,
no incremental reference, and no rescue pass. It produced:

| Check | Result |
| --- | ---: |
| Setup WNS / TNS | `+0.009 ns` / `0.000 ns` |
| Hold WNS / TNS | `+0.041 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.911 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Unconstrained internal endpoints | `0` |
| Rescue used | `0` |

The build used 9,855 slices, 30,501 LUTs, 20,263 registers, 74 block RAM
tiles, six DSPs, and 1,008 control sets. Against the bus-safety-copy build,
slices rose by 29, LUTs fell by 40, registers fell by 33, and control sets
fell by one.

The CPU-state-to-`ss_ramworks_bank` enable paths left the top ten, so keep
this structural cut. Global WNS fell by `0.107 ns` because Disk II and live
Apple-address paths took over:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1, 7-10 | Disk II quarter-track to WOZ-shift enable | `+0.009`, then `+0.075 ns` | 76.1-76.2% | 12 |
| 2 | Apple address to Disk II bit-offset data | `+0.017 ns` | 76.7% | 12 |
| 3, 5 | Disk II quarter-track to data-latch enable | `+0.038`, `+0.069 ns` | 76.3-76.5% | 12 |
| 4 | Apple address to SmartPort input-FIFO write enable | `+0.066 ns` | 74.9% | 12 |
| 6 | vTW cycle address to posted-write memory input | `+0.074 ns` | 76.4% | 10 |

Do not promote this build. It passes the nonnegative export gates but remains
well below `+0.300 ns`. Trace the Disk II quarter-track enable cone and the
live Apple-address consumer cone before choosing the next cut. Do not infer a
new change from WNS alone; both classes are route-heavy and need path-level
control-flow review.

#### vTW Disk II Time-Tick Stage Build

Commit `bd99547fb82abcf90b5c4adbf7928926b536be46` stages only the
accepted normal vTW Disk II time tick by one 133.333 MHz fabric clock. The
stage clears when the core session ends. Private Disk II reads keep their
registered direct-access tick, while native and Q7 cycles keep the physical
1 MHz tick path.

All 14 vTW benches passed. The focused checks prove exact accepted and output
pulse counts across a readiness stall, let an accepted tick drain after a
later stall, clear a pending tick on session reset, and keep private, native,
and Q7 tick selection separate. All 19 standard Disk II tests, 66 WOZ tests,
the capture-FIFO and cycle-egress tests, nine SDD tests, the linear-overlay
source and RTL tests, and 17 VidHD/SHR tests also passed.

The clean full diagnostic run `20260813T082040Z-bd99547f-full` used Vivado
2025.2, the default seed, and no incremental reference. Its normal
post-route `Explore` result had `-0.030 ns` setup WNS. The extra
`AggressiveExplore` rescue pass did not improve it, so `rescue_used=1`. The
run failed its setup gate and exported no candidate DCP, bitstream, or XSA.

| Check | Result |
| --- | ---: |
| Build status | `failed` |
| Setup WNS / TNS | `-0.030 ns` / `-0.030 ns` |
| Setup failing endpoints | `1` |
| Hold WNS / TNS | `+0.039 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.994 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Unconstrained internal endpoints | `0` |
| Rescue used | `1` |
| Hardware export | refused |

The run used 9,725 slices, 30,398 LUTs, 20,289 registers, 74 block RAM
tiles, six DSPs, and 1,008 control sets. Against the soft-switch apply build,
slices fell by 130, LUTs fell by 103, registers rose by 26, and control sets
stayed unchanged. Setup WNS fell by `0.039 ns`, hold WNS fell by `0.002 ns`,
and bus-skew slack rose by `0.083 ns`.

The old Disk II quarter-track/readiness/normal-tick paths to the WOZ shift and
data-latch enables left the top ten. The existing vTW posted-write class took
over instead:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1, 5-7, 9 | vTW cycle address to posted-write BRAM enable/data | `-0.030`, then `+0.059` to `+0.123 ns` | 76.3-76.6% | 10 |
| 2 | vTW write-direction state to address-direction output | `+0.003 ns` | 48.4% | 2 |
| 3 | Machine-INH safety copy to data-direction output | `+0.004 ns` | 53.5% | 3 |
| 4, 8 | vTW cycle address to posted-fill state | `+0.040`, `+0.119 ns` | 66.7-67.4% | 13 |
| 10 | Disk II request address to recalibration-sound arm state | `+0.133 ns` | 80.2% | 8 |

Keep the time-tick stage because it cut the measured same-cycle feedback and
passed its functional checks. Do not claim that it removed every Disk II
quarter-track consumer. Do not promote this run: it has one failing setup
endpoint, used the rescue pass, and produced no hardware outputs. Trace the
cycle-address-to-posted-write BRAM enable cone before choosing another cut.

#### vTW Posted-Write Tuple Stage Build

Commit `72903dd21744a04e6f493c650492b66c3b66c4c3` adds one registered
address/data/valid tuple between final core-or-ARM acceptance and the vTW bus
engine's posted queue. The core keeps priority on an exact collision. The
stage can drain and refill on the same edge, so it keeps one write per fabric
clock. It adds one 133.333 MHz clock, about 7.5 ns, before queue entry. Reset,
disable, or Apple `RES#` cancels the pending tuple at the same transaction
boundary that clears the queue.

All 14 vTW benches passed. Away from a queue-clear boundary, the full-system
test now proves that each accepted core or ARM tuple reaches the engine on the
next edge with the same address and data. It also checks a sustained
eight-write ARM burst, exact core-over-ARM collision order, overlay-write order
before slot-7 `DEVSEL`, and reset cancellation of an accepted tuple. The
capture-FIFO, cycle-egress, nine SDD, linear-overlay source and RTL, 17
VidHD/SHR, and 16 SmartPort service tests also passed.

The clean full build `20260813T090405Z-72903dd2-full` used Vivado 2025.2, the
default seed, no incremental reference, and no rescue pass. It exported the
candidate DCP, bitstream, and XSA.

| Check | Result |
| --- | ---: |
| Build status | `exported` |
| Setup WNS / TNS | `+0.121 ns` / `0.000 ns` |
| Setup failing endpoints | `0` |
| Hold WNS / TNS | `+0.058 ns` / `0.000 ns` |
| Pulse-width WNS / TNS | `+0.265 ns` / `0.000 ns` |
| Worst bus-skew slack | `+5.816 ns` |
| Route errors | `0` |
| Missing XDC objects | `0` |
| Unconstrained internal endpoints | `0` |
| Rescue used | `0` |

The build used 10,014 slices, 30,533 LUTs, 20,273 registers, 74 block RAM
tiles, six DSPs, and 1,009 control sets. Against the Disk II time-tick run,
slices rose by 289, LUTs rose by 135, registers fell by 16, and control sets
rose by one. Setup WNS rose by `0.151 ns`, hold WNS rose by `0.019 ns`, and
bus-skew slack fell by `0.178 ns`.

The prior vTW posted-memory and posted-fill paths left the saved top ten, so
keep this cut. Live Apple-address consumers took over:

| Rank | Path class | Slack | Route share | Levels |
| ---: | --- | ---: | ---: | ---: |
| 1 | Apple address to Disk II recalibration-sound arm state | `+0.121 ns` | 73.6% | 12 |
| 2, 3, 5 | Apple address to SmartPort input-FIFO write enable | `+0.206` to `+0.255 ns` | 73.0-73.2% | 13 |
| 4, 10 | Apple address to Disk II bit-offset enable | `+0.222`, `+0.283 ns` | 75.5-75.7% | 12 |
| 6, 8 | PS AXI interface to egress and Ethernet config state | `+0.259`, `+0.278 ns` | 83.7-83.9% | 0 |
| 7 | Apple reset snapshot to control read data | `+0.264 ns` | 76.5% | 10 |
| 9 | Slot-enable mask to no-slot-clock reset | `+0.280 ns` | 80.5% | 7 |

Do not promote or package this build. Setup margin is still `0.179 ns` below
the `+0.300 ns` gate. For the next measured cut, split the native Disk II raw
read-response path from its controller side effects, then register the native
`$C0Ex` read/write pulse and low address for the stepper and bit-offset
updates. Capture write data at its current data phase. Keep the same-cycle
Apple read response and private vTW request order intact. This targets ranks
1, 4, and 10; remeasure before changing the SmartPort path.

### 2. Group Apple Bus Snapshot Copies When Needed

The broad `MAX_FANOUT` trial is complete and rejected. Use a fresh path report
before adding explicit copies. If one sampled bus source still limits several
blocks, make same-edge copies in `apple_bus_wrapper` for these consumer groups:

- Card and slot logic.
- Cycle and SDD capture.
- vTW and video state.

Capture each copy on the same edge as the completed snapshot register. Do not
add a cycle to the bus event or copy a partly assembled bus word. Keep each
copy near its named consumers. Use `DONT_TOUCH` only on copy registers that
Vivado would merge; do not put it on the full cones.

Measure this as its own commit and full build. Remove the copies if they raise
LUT use or create a worse path without a clear timing gain.

## Phase 2: Reduce Congestion

### 1. Move the SSI263 F2 Table to Block RAM

The two audio back ends use the same 4,096 by 16-bit F2 data. Replace duplicate
LUT ROMs with one inferred true dual-port block RAM when both ports need an
independent read. Register the outputs and add the read cycle to the coefficient
load state. Audio sample work has thousands of fabric clocks available, so one
read cycle should fit. Prove address and coefficient order in simulation.

Measure block RAM, LUT, F7/F8, slice, and timing changes. Keep the edit only if
it frees useful logic or improves placement without an audio fault.

### 2. Move the SmartPort C8 ROM to Block RAM

Measure the SmartPort C8 ROM as a separate change. Its two registered read
ports should fit one true dual-port block RAM without adding a read cycle, but
the synthesis report must prove the mapping and the physical and vTW ROM tests
must prove byte and cycle order.

### 3. Test Control-Set Settings

Run a full build with synthesis control-set optimization threshold `8`.
Change no RTL in that experiment. Compare control sets, slices, LUTs, WNS, and
the top paths. Test `16` later only if `8` helps. Keep a setting only if it
gives repeatable gain.

For new RTL, prefer a shared enable and a synchronous reset when the behavior
allows it. Do not remove a needed reset or merge unrelated enables just to cut
the count.

### 4. Test Synthesis Retiming

Test `STEPS.SYNTH_DESIGN.ARGS.GLOBAL_RETIMING on` as one flow-only commit.
Vivado 2025.2 marks the old `RETIMING` property obsolete. Run all CDC and
source checks. Review every XDC query after retiming; hierarchy-based queries
may no longer bind. Keep retiming only if both timing and function pass.

### 5. Sweep Place Directives

Wait until the Phase 1 and ROM netlists stop changing. Then run a small full
build sweep with one placement directive per build: `ExtraNetDelay_high`,
`ExtraPostPlacementOpt`, then `AltSpreadLogic_high`. Keep synthesis, route, and
physical optimization fixed. Record every result, including failures. Use the
best repeatable result as a candidate; do not select one lucky build by WNS
alone.

Before any flow trial, fix the build record to read `GLOBAL_RETIMING`, record
the post-route enable and route options, and make promotion compare the Vivado
version and every flow field between both builds. Promotion must also require
`rescue_used=0`; an extra rescue pass does not show intrinsic margin.

## Phase 3: Use Only if the Target Still Fails

### 1. Physical Response Stage

Complete in `fbddd819`. The stage removed the final card-response mux from the
data and direction pins but did not raise total WNS. Do not add another stage
unless a new report shows a remaining physical response path.

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
