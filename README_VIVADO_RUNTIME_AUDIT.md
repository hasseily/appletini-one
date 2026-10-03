# Vivado runtime and timing audit

As of 2026-10-03, `codex/turbo-paging-dma` requires at least **+0.050 ns**
final setup slack for build export, packaging, and promotion. The temporary
implementation setup uncertainty stays at **0.200 ns**; clocks, hold checks,
and pulse-width checks stay unchanged. The results and targets below describe
the earlier audit runs.

Audit date: 2026-09-27. This report describes the completed run
`.timing_runs/20260926T201645Z-f1f4919d-full`, made with Vivado 2025.2 for
`xc7z020clg484-2`. The later sections record measured simplification trials
and constraint corrections. A fresh full normal build now confirms
**+0.163 ns nominal setup slack** and exports the bitstream/XSA in
**17m33s**, about **31.8% less time** than the original 25m44s attempt.
It meets the requested +0.150 ns floor. Hardware validation and known-good
promotion remain pending.

The original run took **25m44s** and ended at **+0.017 ns nominal setup slack**, below
both its configured +0.200 ns release requirement and the requested +0.150 ns
target. Routing and physical optimization took most of the time. The slowest
paths after routing were audio mixing and Apple-bus control paths, rather than
the new pixel-clock controls.

Source references below name the report and its line at audit time. The
`.timing_runs` reports are the saved record; the `project` run logs can change
when those run names are reused. The manifest records `git_dirty=1`, so its Git
SHA alone does not identify all the sources used.

The original synthesis and routed checkpoints are also archived in that run
folder as `original_synth.dcp` and `original_postroute_physopt.dcp`. The routed
checkpoint SHA-256 is
`0091bce27a9ed3adc12e103c9a4b8cc3f7b02cdf7edc03121aa22661555eff7b`.

## Where the time went

The manifest records 20:16:45–20:42:29 UTC. The driver waited **4m22s for
synthesis** and **20m19s for implementation through bitstream generation**.
These waits contain the command times below; they must not be added to them.

| Command | Elapsed | Share of total | Evidence |
| --- | ---: | ---: | --- |
| `synth_design` | 3m57s | 15.3% | `build/display_modes_rtl/full_build.log:4723` |
| `opt_design` | 11s | 0.7% | same log:4967 |
| `place_design` | 3m48s | 14.8% | same log:5255 |
| Pre-route `phys_opt_design` | 4m12s | 16.3% | same log:7246 |
| `route_design` | 6m55s | 26.9% | same log:7900 |
| Post-route `phys_opt_design` | 2m26s | 9.5% | same log:8039 |
| `write_bitstream` | 21s | 1.4% | same log:8085 |
| Other time: run launch, design load, checkpoints and reports | 3m54s | 15.2% | Difference from manifest elapsed time |

Wait totals appear at `full_build.log:4730` and `:8088`. Command elapsed time
is wall time, not CPU time. Peak memory reached 2,786.5 MB during bitstream
generation; the log supplies no evidence that paging caused the delay.

Although the manifest says `jobs=8`, the actual placer, router and physical
optimizer each logged a maximum of **two CPUs**. Synthesis logged two
processes. See `full_build.log:2483`, `:5002`, `:5291`, and
`project/appletini_yarz.runs/impl_1/runme.log:2537`. An eight-job launch did not
make this implementation run with eight worker threads.

A direct query of the installed **2025.2** tool also returned
`general.maxThreads=2` (`build/vivado_simplify/query_profiles.log:42`).
[AMD UG904's multithreading documentation](https://docs.amd.com/r/en-US/ug904-vivado-implementation/Multithreading-with-the-Vivado-Tools)
describes the Windows default of two and the `general.maxThreads` setting,
with a limit of eight. The linked current manual is 2026.1; the local query
and run logs establish the installed version's behavior.

## Which optimization work was expensive

The active profile was `Performance_ExplorePostRoutePhysOpt`: Explore placement,
pre-route physical optimization, Explore routing with `-tns_cleanup`, and
Explore post-route physical optimization. A full build disabled incremental
reuse; it did not select default implementation settings. Synthesis retiming
and the control-set threshold remained `auto`.

The router completed its first pass by 3m14s, then performed incremental
placement and another route. The whole command took 6m55s. The first routed
timing check was −0.585 ns; the final routed check was −0.282 ns, both while
the temporary setup margin remained applied. The extra route work therefore
improved WNS by about 0.303 ns in this run. Its 3m41s cost is substantial, but
removing it has an unmeasured timing cost. See `impl_1/runme.log:2745`, `:2823`,
`:2825`, `:2834`, and `:3143`.

The forced TNS cleanup phases themselves occupied only about **18 seconds**:
2m49s→2m59s and 6m19s→6m27s on the router's cumulative clock. They are not the
main reason routing took nearly seven minutes (`impl_1/runme.log:2650`,
`:2717`, `:2972`, `:3031`).

Pre-route physical optimization reported **+0.888 ns WNS gain** across 33
iterations. Its largest time entries were multi-cell placement (71s),
single-cell placement (57s), and critical-cell work (48s). The multi-cell
entry improved WNS by 0.050 ns while worsening TNS by 9.143 ns; the complete
pass still improved both metrics. Post-route physical optimization improved
WNS by **0.098 ns** and optimized six cells/nets, taking 2m26s overall. These
were useful timing passes, not proven no-ops (`impl_1/runme.log:2485` and
`:3292`). The optional AggressiveExplore rescue did **not** run
(`manifest.txt:41`, `rescue_used=0`).

## Density and routing pressure

| Resource | Used | Available / utilization |
| --- | ---: | ---: |
| LUTs | 36,086 | 53,200 / 67.83% |
| Occupied slices | 11,166 | 13,300 / 83.95% |
| Registers | 23,470 | 106,400 / 22.06% |
| Block RAM tiles | 110 | 140 / 78.57% |
| DSPs | 7 | 220 / 3.18% |
| CARRY4 cells | 3,267 | Count reported |
| Control sets | 1,205 | 993 minimum plus 212 from physical replication |

See the saved `utilization.rpt:35`, `:76`, `:106`, `:121`, `:203`, and
`control_sets.rpt:26`. Low register use does not imply much placement space:
occupied slices are already near 84%, and control sets limit packing.

Final local congestion reached 97.06% westbound, 91.18% eastbound, 89.19%
southbound, and 86.49% northbound. The reported effective congestion levels
were 1, 1, 2, and 0 respectively. These are local hot spots, not proof that the
whole device is unroutable. All **54,260 routable nets completed with zero
route errors** (`impl_1/runme.log:3083`; saved `route_status.rpt:8`).

## Timing margin versus final timing

The pre-placement hook added **0.200 ns** fabric setup uncertainty. The
post-route hook removed it before the design was reopened for final reports.
The saved manifest confirms final user uncertainty **0.000 ns** and the
original 10 ns direction-output / 8 ns PHI0-release bounds.

| Point in run | Setup WNS | Interpretation |
| --- | ---: | --- |
| After pre-route physical optimization | −0.034 ns | Estimated routes, temporary margin applied |
| After routing | −0.282 ns | Routed, temporary margin applied |
| After post-route physical optimization | −0.183 ns | Routed, temporary margin applied |
| Saved final signoff report | **+0.017 ns** | Routed, temporary margin removed |

The last two values differ by the removed 0.200 ns requirement; this was not
an additional physical improvement. See `full_build.log:4988`, `:7212`,
`:7877`, `:8019`, `:8047`; saved `timing_summary.rpt:155`. Final hold slack was
+0.060 ns, pulse-width slack +0.265 ns, and bus-skew slack +5.430 ns. There were
no failing setup/hold endpoints, missing constraint objects, or unconstrained
internal endpoints. The exporter nevertheless correctly rejected the +0.200 ns
release requirement. It did not export an XSA for this run.

External I/O warnings remain: 17 inputs without full input delays, 48 outputs
without output delays, and 36 partially constrained inputs. Their intended
exceptions need a separate board/CDC review; the absence of internal
unconstrained endpoints does not validate every external interface.

## Actual paths to improve

The saved final `timing_summary.rpt` reports these path families:

| Path | Slack | Data-path evidence | Report line |
| --- | ---: | --- | ---: |
| Disk II audio sample → mixed audio sample register | +0.017 ns | 7.383 ns; 15 levels, including 11 CARRY4; 58% logic delay | 2300 |
| VTW `wr_data_en_q` → boot-menu timer | +0.017 ns | 7.001 ns; 11 LUT levels; 78% route delay | 2402 |
| `onee_run` → SmartPort output-word enable | +0.031 ns | 7.024 ns; 12 LUT levels; 77% route delay | 2587 |
| `onee_run` → linear-text staged-row enable | +0.042 ns | 7.009 ns; 13 LUT levels; 75% route delay | 3012 |
| Pixel FIFO → DVI red register | +0.369 ns | Pixel-domain worst setup path | 3901 |

DVI output setup slack was +0.872 ns and hold slack +1.618 ns
(`timing_summary.rpt:210`). The pixel-clock mode sequencer and preset
arithmetic do not occur in the reported ten worst fabric paths. That is not
a full proof about every video path, but it gives no reason to optimize that
arithmetic first.

The placement-only worst paths had led to VTW shadow-RAM write enables.
Routing and optimization moved the final limit. RTL work should use the final
families above, not the early placement estimate. Reaching +0.150 ns needs at
least 0.133 ns more on both paths now tied at +0.017 ns, with the other families
close behind.

## Comparison and first follow-up experiment

Nearby saved full builds took 24m11s, 24m24s, 29m50s, 28m47s, and 28m45s on
September 25. September 26 runs at 11:03 and 12:11 UTC took 57m20s and 56m44s.
The audited 25m44s run is not the slowest recent full build. Those records use
different dirty source states and do not isolate a cause for the variation.
See each run's `manifest.txt` under `.timing_runs`.

The first experiment is
`.timing_runs/20260927T085536Z-f1f4919d-simplify-baseline`. Its manifest records
fresh synthesis (`synthesis_reused=0`) and
`opt Default place Default phys None route Default post None threads 8 margin 0.0 unpin 1`.
The script disables incremental checkpoints, added physical-optimization
passes, TNS cleanup and rescue, and applies no temporary timing margin. New
run names leave the previous `synth_1`/`impl_1` artifacts intact.

This baseline also removes **LOC/BEL only** from the two historical LUT
placements: `apple_data_enable_lut` at `SLICE_X104Y23/A6LUT`
(`hdl/apple/apple_bus_wrapper.sv:467`) and `apple_addr_enable_lut` at
`SLICE_X112Y23/A6LUT` (`hdl/apple/apple_bus_write_arbiter.sv:120`). It retains
their truth tables and `DONT_TOUCH` protection. Thus it combines **directive,
thread-count and placement-hint changes**; differences from the previous
build cannot be attributed to any one of those changes. Hold these settings
fixed when testing one additional optimization pass.

## Constraint and protection inventory

The baseline keeps these board and CDC requirements. XDC references below
refer to `hdl/constraints/appletini_yarz.xdc`.

| Keep | Exact requirement and source |
| --- | --- |
| Board pins and electrical settings | Package pins, I/O standards, drive strength, slew and pulls; XDC from line 20 |
| Clock definitions and domain separation | PS/IP clocks; audio MCLK 81.380 ns and BCLK divide-by-four; replace the broad fabric/pixel group with the verified narrow exceptions below |
| PSRAM interface | Forwarded clock; IOB packing; data output max +2.01/min −1.99 ns; CE max +2.51/min −2.49 ns; calibrated-input false paths; XDC:471, 489, 497, 513, 517 |
| DVI receiver timing | Forwarded clock edges `{2 3 4}`; data/control output max +2.0/min −1.4 ns; XDC:530, 540 |
| Apple bus paths | Input datapath bound 5 ns; direction outputs 10 ns; raw PHI0→DIR_D, IRQ re-arm and DMA re-arm bounds 8 ns; XDC:583, 613, 620, 626, 634 |
| Narrow CDC exceptions | Audio-menu-mute crossing bound 10 ns and reset synchronizer CLR exception; remove the conflicting capture PRE false path as verified below |
| CDC and calibration structure | `ASYNC_REG` in `hdl/cdc_bit_sync.sv:14` and `hdl/reset_sync.sv:15`; IDELAYCTRL `DONT_TOUCH` and RDY `KEEP` in `hdl/appletini_yarz_top.sv:948,968` |
| Required implementation structure | Memory styles, functional pipelines, reset/handshake logic; IRQ/DMA register `KEEP` names used by the 8 ns exceptions (`hdl/apple/apple_bus_wrapper.sv:259`) |

Other historical hints remain in place and need individual evidence before
removal: arbiter reduction `KEEP` (`apple_bus_write_arbiter.sv:74,110`),
`machine_inh_allowed_wrapper_q` protection (`apple_top.sv:1075`),
`onee_run_q` fanout 32 (`onee_mode_safety_guard.sv:234`), virtual-bus data
`KEEP` (`apple_virtual_bus.sv:65`), SSI start/speaker history `KEEP`
(`ssi263_bus_wrapper.sv:82`, `onee_speaker_audio.sv:28`), VTW response/tag
protection and slow-update `KEEP` (`vtw_core_top.sv:328,1092,1178`), and CPU
decimal-result protection/one-hot state encoding (`w65c02_core.sv:206,227`).
These paths are under `hdl/apple/`. A hint being historical does not prove
that removing it preserves timing, binding of exceptions, or behavior.

Check exception binding and ignored exceptions independently. In particular,
the capture PRE false path may override the nearby 5 ns bound despite the
comment's stated intent; do not loosen either rule just to change slack.

The XDC header named speed grade −1 while the actual run used −2. The header
now says −2. The implementation device and timing model have not changed.

Measure final routed slack against **+0.150 ns with nominal board constraints**,
along with hold, pulse width, bus skew, route errors and missing objects. A
profile simplification counts as a useful result only if those checks pass.
If the default baseline misses the target, inspect its new routed paths before
adding one optimization pass at a time. If the same families remain critical,
reduce serial audio arithmetic and long shared bus-enable paths with focused
RTL work and functional tests; do not hide those paths with looser constraints.

## Measured simplification trials

The first completed baseline is
`.timing_runs/20260927T090108Z-f1f4919d-simplify-baseline`. It reuses the fresh
synthesis from the initial attempt; that synthesis took 3m39s inside
`synth_design` (4m01s including launch/checkpoint/report work).

| Result | Previous Explore build | Default, eight threads, unpinned LUTs |
| --- | ---: | ---: |
| Placement command | 3m48s | 3m02s |
| Routing command | 6m55s | 2m20s |
| Nominal setup WNS | +0.017 ns | **−0.786 ns** |
| Hold WNS | +0.060 ns | +0.058 ns |
| Failing setup endpoints | 0 | 225 |
| Occupied slices | 11,166 | 11,418 |
| Control sets | 1,205 | 1,122 |

The baseline completed implementation through routing in 7m22s. It does not
include bitstream generation and cannot be compared directly with the old
20m19s implementation-through-bitstream time. Two driver issues required a
retry/report recovery: the generated address-LUT name includes a dot before
the primitive name, and the completed-run status check rejected the first
status read. The saved reports come from the completed routed checkpoint;
the manifest also retains the earlier driver status record.

The baseline meets hold, pulse width and bus-skew checks, routes every net,
and reports no missing constraint objects. It **fails setup timing**. Its
worst path runs from the machine-policy register to `a2fpga_dir_a` at
−0.786 ns; the worst fabric path runs from `onee_run` to SmartPort's output
counter at −0.466 ns. Removing the whole profile is faster, but this result
does not support using it for firmware. Fewer control sets also did not
reduce occupied slices in this run.

The second completed trial,
`.timing_runs/20260927T091206Z-f1f4919d-simplify-phys_explore`, changes only
the pre-route physical-optimization directive from disabled to `Explore`.
It retains default opt/place/route, eight threads, unfixed LUT locations,
no temporary margin and no post-route pass.

The extra pass takes 1m51s. Its placement estimate improves from −0.600 ns
to +0.024 ns, but final routed WNS is **−0.437 ns** with 122 failing setup
endpoints. Routing takes 2m45s; implementation through routing takes 9m21s.
Hold remains +0.058 ns; pulse width, bus skew, all-net routing, internal
constraint coverage and missing-object checks pass. Control sets rise to
1,148 and occupied slices to 11,422. This profile still cannot ship.

The worst path is now `onee_selected` to VTW auxiliary shadow-RAM write
enable (nine logic levels, 5.970 ns routing out of 7.400 ns). `onee_run` to
linear-text configuration enable follows at −0.289 ns. The worst direction
output improves to −0.247 ns, but its moved final gate has 1.923 ns of
pad-net route delay versus 1.407 ns in the original fixed placement.

The driver originally rejected Vivado's valid completed-but-failing status,
`route_design Complete, Failed Timing!`. Reports were recovered from the
completed run without repeating implementation. The driver now accepts that
exact status as well as `route_design Complete!`, and judges timing from the
reports. Recovery adds time to the manifest's UTC interval; use the measured
command and implementation-wait times above for runtime comparisons.

The third completed trial,
`.timing_runs/20260927T092638Z-f1f4919d-simplify-restore_pins`, restores only
the two original LOC/BEL hints while retaining the second trial's directives.
It uses a fresh synthesis because a reverted audio-helper experiment marked
the earlier synthesis stale. The restored HDL matches the baseline source
hashes. The audio helper has not changed in these three trials.

Restoring the hints improves routed WNS to **−0.180 ns**, with 52 failing
setup endpoints. Hold is +0.014 ns; the other signoff checks pass. Placement
takes 2m22s, the pre-route pass 2m12s, and implementation through routing
8m59s. Occupied slices fall to 11,316 and control sets to 1,137. Fresh
synthesis takes 3m11s including launch and reports. This is useful evidence
for keeping the two hints, although this profile still fails setup timing.

The new worst paths run from VTW bus-cycle state to mouse position registers
(−0.180 ns), slot-enable policy to DIR_A (−0.169 ns), and Disk II track mode
to stream-counter/cache enables (−0.093 ns). The earlier VTW auxiliary-RAM
and linear-text paths no longer head the report. Placement changes move the
limit among several shared-control paths; fixing only the last run's worst
endpoint would not prove that the design as a whole meets the target.

The fourth trial,
`.timing_runs/20260927T094027Z-f1f4919d-simplify-margin`, retains this profile
and requests 0.200 ns of extra setup margin during implementation. It reuses
the checked margin hooks for fabric setup and Apple direction outputs, then
restores exact nominal constraints before final reporting.

Its nominal WNS is **−0.166 ns**, with 22 failing setup endpoints and hold
+0.037 ns. Placement takes 2m33s, pre-route optimization 3m26s and routing
2m41s; implementation through routing takes 10m44s. Slices fall to 11,015,
while control sets rise to 1,250. Extra requested margin alone does not close
the design. The main limits are now ONE//e control to the linear-overlay
state (−0.166 ns) and audio mixing (−0.116 ns, 11 CARRY4 cells).

The fifth trial simplifies the saturating audio adder: a 16-bit sum and
sign-overflow check replace the extended sum and range comparisons. It keeps
the existing nested saturation order and adds no cycle. Xsim passes 722,232
pair/triple arithmetic cases, and the ONE//e top-level integration test passes.
The build also applies the independently verified constraint corrections
below. Neither the proposed address-enable factoring nor the VTW write-gate
change has been applied. The build uses the same fourth-trial profile.

This run, `.timing_runs/20260927T095832Z-f1f4919d-simplify-audio_constraints`,
finishes at **−0.206 ns**, with ten failing setup endpoints, hold +0.056 ns
and bus-skew slack +5.690 ns. Synthesis takes 3m34s including launch/reports;
implementation through routing takes 11m27s. Command times are 2m47s for
placement, 3m19s for pre-route physical optimization and 3m09s for routing.
Occupied slices are 11,018 and control sets 1,361. Synthesis uses 3,227 CARRY4
cells versus 3,239 before the audio change; audio no longer appears among
the worst reported paths. The new worst path is AXI write-valid to VTW
shadow-RAM write enable: ten LUT levels and 80% route delay. The next bound
is the machine-policy path to DIR_A at −0.156 ns.

All six raw Apple capture paths retain their 5 ns bound, and all 26 FIFO
Gray-pointer paths retain their generated bounds and pass. However, the
synthesis log reports two missing generated-clock names and unsupported
`if` commands in the newly added XDC checks. The named clocks exist during
implementation but not inside the synthesis flow's black-box IP view. This
trial therefore fails both setup and clean constraint loading. The seventh
trial puts the video CDC rules in a late, implementation-only XDC and keeps
explicit object validation in Tcl. No warning waiver is used.

The sixth trial, `.timing_runs/20260927T101458Z-f1f4919d-simplify-opt_explore`,
reuses that synthesis and changes only `opt_design` from Default to Explore.
It measures the effect of logic optimization before changing RTL again;
it does not resolve the synthesis warning on the reused input.

Explore logic optimization improves WNS to **−0.111 ns**, with ten failing
endpoints, hold +0.027 ns and bus-skew slack +5.831 ns. It takes nine seconds;
placement takes 2m52s, pre-route optimization 2m46s and routing 2m40s.
Implementation through routing takes 10m14s, 1m13s less than the preceding
trial. Occupied slices rise to 11,391; control sets fall to 1,261. The final
worst family is VTW bus-cycle state to mouse and linear-overlay register
write enables. The AXI-to-shadow-RAM path no longer heads the report.

The seventh trial, `.timing_runs/20260927T102736Z-f1f4919d-simplify-route_explore`,
adds Explore routing, retains Default placement, and still has no post-route
pass or TNS-cleanup option. It also uses fresh synthesis to validate the
constraint-loading correction: `video_cdc_impl.xdc` has synthesis disabled
and late implementation order, while ordinary Tcl performs strict object
checks. The RTL matches the sixth trial. Both proposed bus-enable RTL
changes remain unapplied.

Fresh synthesis takes **3m19s including launch and reports** (3m00s inside
`synth_design`). The new XDC loads during implementation without missing
clocks or unsupported Tcl commands. Placement reproduces the sixth trial's
estimated WNS −0.961 ns and TNS −48.146 ns; pre-route physical optimization
also reproduces WNS +0.026 ns and zero TNS. These stage results match before
the changed routing directive runs.

Final nominal setup WNS is **+0.100 ns**, with **+0.163 ns in the fabric
domain**. This closes setup violations, but misses the requested global
+0.150 ns target by 0.050 ns. Hold is +0.027 ns, pulse-width slack +0.265 ns
and bus-skew slack +5.831 ns. All nets route, with zero route errors, missing
constraint objects or unconstrained internal endpoints. All **32 checked
bounds pass**: six Apple capture paths and 26 FIFO Gray-pointer paths.
The saved `manifest.txt` records `constraint_bounds_status=PASS` and
`status=measured`, not `target_met`.

| Measured stage | Sixth trial: Default route | Seventh trial: Explore route |
| --- | ---: | ---: |
| Logic optimization | 9s | 8s |
| Placement | 2m52s | 2m34s |
| Pre-route physical optimization | 2m46s | 2m24s |
| Routing | 2m40s | 5m34s |
| Implementation through routed reports | 10m14s | **12m20s** |
| Nominal setup WNS | −0.111 ns | **+0.100 ns** |

Explore routing costs 2m54s more inside the router and 2m06s more for the
implementation run. It improves final WNS by 0.211 ns in this comparison.
Both experiments stop after routing and its reports; the original 20m19s
implementation time also includes post-route physical optimization and
bitstream generation. Do not present that difference as a measured saving
for a complete firmware build. The fresh seventh trial's manifest records
199 seconds for synthesis and 740 seconds for implementation.

The remaining worst path is in the pixel domain: framebuffer FIFO
`rdp_inst/count_value_i_reg[4]` to the RAMB36 read-address pin
`ADDRBWRADDR[4]`. It has **zero logic levels**, 0.433 ns of register delay,
and **5.505 ns of route delay**, or 92.7% of its 5.938 ns data delay. Its net
drives 20 loads. See the saved `timing_summary.rpt:6787`; the fabric and pixel
domain results appear in its intra-clock table. This path gives no reason
to add Boolean factoring or another pipeline stage.

The eighth trial,
`.timing_runs/20260927T105314Z-f1f4919d-simplify-pixel_margin`, reuses the
seventh trial's synthesis and keeps the same implementation stages. It adds
the temporary **0.200 ns pixel setup margin before logic optimization**, as
already done for fabric setup, then restores both before final reports.
The Apple output limits and all CDC bounds remain in force.

Pixel WNS improves from +0.100 ns to **+0.423 ns**, but fabric and global
WNS fall to **−0.049 ns**, with four failing setup endpoints and TNS
−0.139 ns. All four failures run from the ONE//e `onee_run_q` replica to
VTW main shadow-RAM write enables. Hold remains +0.059 ns, pulse-width slack
+0.265 ns and bus-skew slack +5.880 ns. All 32 checked Apple/Gray bounds
pass; no constraint object is missing, no internal endpoint is unconstrained,
and routing has no errors. Both final user uncertainties are zero.

Placement takes 2m30s, pre-route physical optimization 3m51s and routing
**4m42s**. Implementation through routed reports takes **12m55s (775s)**,
35 seconds more than the prior trial. This early pixel-margin profile is
rejected: improving the pixel path does not justify failing fabric setup.

The ninth probe, `.timing_runs/20260927T110741Z-f1f4919d-simplify-pixel_repair`,
starts from the seventh trial's clean nominal **+0.100 ns** routed checkpoint.
`.codex_tmp/pixel_route_repair.tcl` limits post-route physical optimization
to pixel-domain routing, critical cells and critical pins. It applies the
temporary margins for that pass, restores both, and checks the full design
and all 32 bounds afterward. It changes no RTL.

The pass takes **30 seconds** and changes **two nets**. Final global and
fabric WNS are **+0.163 ns**; pixel WNS rises from +0.100 ns to **+0.261 ns**.
Hold remains +0.027 ns, pulse-width slack +0.265 ns and bus-skew slack
+5.831 ns. There are no failing setup, hold or pulse-width endpoints, no
internal unconstrained endpoints and no route errors. All 32 Apple/Gray
bounds pass, and both clock domains have zero final user uncertainty.
The manifest records `status=target_met`. See `build/vivado_pixel_repair.log`
and the probe's saved timing, route and bus-skew reports.

This is the first measured result above the requested **+0.150 ns global
floor**, with 0.013 ns to spare. It is a repair of an existing routed
checkpoint, not a fresh full build or a hardware-validated reference.
Its 30-second pass also excludes checkpoint loading, final reports and
bitstream generation, so it must not stand in for a full-build runtime.

The normal-flow integration preserves the seventh trial's placement:
it applies the **0.200 ns fabric margin before `opt_design`**, and adds the
**0.200 ns pixel margin only after `route_design`**. It then runs
`phys_opt_design -routing_opt -critical_cell_opt -critical_pin_opt` with
`-path_groups clk_out1_zynq_ps_bd_clk_wiz_0_0`, and restores both margins
before final reports and export. This fixed, limited pass replaces an
automatic rescue or full post-route Explore pass. The fresh full build below
confirms its timing result. None of these timing results confirms IIGS border
behavior on hardware.

## Confirmed normal build and current profile

The fresh, non-incremental build
`.timing_runs/20260927T111410Z-f1f4919d-full` completes normally and exports
its checkpoint, bitstream and XSA. Its manifest covers **11:14:10–11:31:43 UTC**.
Final setup WNS is **+0.163 ns**, hold +0.027 ns, pulse-width slack +0.265 ns
and bus-skew slack +5.831 ns. Every setup, hold and pulse-width failing count
and total violation is zero. All nets route; missing constraint objects,
route errors and unconstrained internal endpoints are zero. All **32
Apple/Gray bounds pass**. Both clock user uncertainties are zero and the
Apple output requirements are back to **10/10/8 ns**.

| Whole-run measure | Original full attempt | Simplified full build |
| --- | ---: | ---: |
| Total PL build and final checks | 25m44s | **17m33s** |
| Synthesis wait, including launch/reports | 4m22s | **3m15s (195s)** |
| Implementation wait through bitstream | 20m19s | **13m03s (783s)** |
| Nominal global setup WNS | +0.017 ns | **+0.163 ns** |
| Nominal hold WNS | +0.060 ns | **+0.027 ns** |

The full attempt saves **8m11s, or 31.8%**. Both totals include bitstream
generation and final timing checks; the new build also exports the XSA,
where the original attempt stopped at its failed timing gate. This is the
combined result of the measured source, constraint and flow changes, not
an isolated claim about thread count or one directive. Unlike the earlier
routed-only experiments, this comparison includes the complete normal PL
build. It does not include Vitis or firmware packaging.

The new command times are synthesis **2m54s**, placement **2m32s**,
pre-route physical optimization **2m23s**, routing **5m33s**, limited pixel
repair **29s**, and bitstream generation **12s**. These are parts of the
wait totals above, not extra time. Fresh synthesis has the same checksum,
`c75d21d3`, as the successful `route_explore` trial, and all **32 pre-route
WNS/TNS estimates match** that trial exactly.

The profile used for this measurement has eight worker threads in each child run, synthesis
defaults, Explore logic optimization, Default placement, Explore pre-route
physical optimization and Explore routing. It has no TNS-cleanup option,
automatic incremental reference, full post-route Explore pass or automatic
rescue. The fixed route-post hook performs the pixel-only repair described
above. The saved project's actual properties pass the read-only check in
`build/timing_firmware/test_timing_run_properties.log`.

The `four-play` branch later replaces this pixel-only repair with one routed
`AggressiveExplore` pass across all paths. See [the build workflow](scripts/SCRIPTS_README.md)
for the current profile; the timings above describe the earlier F1.2.0 build.

Vivado's generated implementation script orders the work as routing,
built-in route reports, `finish_video_timing.tcl`, routed checkpoint, then
bitstream. Thus, **the built-in project routed reports precede the repair
and margin removal**. The immutable run's `timing_summary.rpt` is the
authoritative final nominal report: the parent reopens the repaired checkpoint
and checks both margins, board limits and CDC bounds before export. No
netlist edit occurs after bitstream generation. Full-run evidence is in
`build/vivado_simplified_full.log` and the immutable run directory.

This is a dirty-tree user test build (`git_dirty=1`), not a promoted timing
reference. No hardware validation or known-good promotion has occurred.
The full Vitis rebuild and firmware verification also pass; their times are
outside this PL measurement. The test image is
`firmwares/F1.2.0-timing/FIRMWARE.BIN` (4,391,788 bytes), with SHA-256
`ee9a4a5fa55a2144fd0fd69cc4882022f994f63b39918bd7f09fd4e9f5fc4c07`.
Its folder contains the source inputs, build logs, timing reports, bitstream,
XSA, ELFs and firmware verification record. The root `FIRMWARE.BIN` matches.

## Why the protected CPU and cache registers exist

The 41 protected VTW/CPU registers are timing stages, not CDC synchronizers.
They comprise eight cached data bits, nine read-tag bits, eight write-tag
bits, and sixteen decimal-result bits. The source and history give concrete
reasons for their protection:

- `f382eb1` split the cache lookup from the tag comparison on September 12.
  `1197f28` then reduced the byte-cache index width, leaving a nine-bit read
  tag. These registers keep the lookup and comparison on separate edges.
- `7cbf7ef` selected one-hot CPU state encoding to shorten address selection
  ahead of the cache lookup.
- `65e66ff4` placed decimal arithmetic on the CPU's existing extra cycle;
  `c3c75be` protected that register because stable operands let synthesis
  rebuild the arithmetic at the later commit edge.

Removing a protection attribute is a valid separate timing experiment, but
removing these stages or changing their cycle boundaries is a functional
change. None of these register groups heads the latest restored-placement
report. There is no current evidence that dropping their protection would
improve the limiting paths. Keep them while testing simpler expressions on
the actual critical paths.

## Routed checkpoint constraint audit

A separate Vivado 2025.2 session opened the original
`impl_1/appletini_yarz_top_postroute_physopt.dcp` with two worker threads.
It changed no source or checkpoint. The reports are in
`build/vivado_constraint_audit/`; `effective_timing.xdc` records the resolved
constraint scopes, including generated IP constraints.

| Property in the routed checkpoint | Count |
| --- | ---: |
| Cells with `KEEP` | 344 |
| Cells with `DONT_TOUCH` | 59 |
| Nets with `DONT_TOUCH` | 54 |
| Cells with fixed `LOC` | 403 |
| Cells with fixed `BEL` | 2 |
| Pblocks | 0 |

These are object counts, not independent hand-written constraints. The 403
fixed locations include board I/O, PS pads, registers packed into I/O sites,
and I/O delay/DDR primitives. Only two fixed cells are LUTs: the historical
address- and data-enable gates already under test. Both also have fixed BELs.
Thus, there is no large fabric floorplan to discard. Of the 59 protected
cells, 41 are VTW response/tag or CPU decimal-result registers, eight are
AXI skid-buffer copies, four are generated clock-reconfiguration state
registers, and the rest comprise IDELAYCTRL, machine-inhibit state, three
bus-enable LUTs including a replica, and the framebuffer FIFO hierarchy.
Many of the `KEEP` cells are CDC or interface registers. Removing all these
properties would also remove functional and timing safeguards.

The exception report identifies a real mismatch between the Apple input
comment and the effective constraints. The 5 ns input bound is **partly
overridden** by the false path to the six raw-transition capture PRE pins.
All six reported paths have infinite slack and `Timing Exception: False Path`;
they do not retain an enforced 5 ns bound. Their measured data delays in this
checkpoint are 2.996-3.572 ns. The normal synchronized input paths retain the
bound. Correcting this gap would strengthen constraint coverage; removing
the input requirement would hide it.

The fabric/pixel asynchronous clock group also overrides the two generated
XPM FIFO Gray-pointer maximum delays. These are the framebuffer FIFO's
`wr_pntr_cdc_inst` (7.499 ns) and `rd_pntr_cdc_inst` (6.733 ns), each covering
13 source bits. The generated 6.733 ns bus-skew checks remain present and
passed the original build, but that does not reinstate the ignored maximum
delay checks. A later CDC cleanup should retain these per-crossing bounds
while cutting only intended asynchronous checks; it must not simply remove
the clock-domain restrictions. AMD describes this priority rule and calls
for point-to-point exceptions on the other crossings when a maximum delay
must remain active. See [UG903: Constraining Asynchronous Signals](https://docs.amd.com/r/2025.1-English/ug903-vivado-using-constraints/Constraining-Asynchronous-Signals)
and [UG906: TIMING-24](https://docs.amd.com/r/2024.1-English/ug906-vivado-design-analysis/TIMING-24-Overridden-Max-Delay-Datapath-Only?contentId=T5gcz0kD7nYP1qLPhQ_xcA).

The 13 rows in `exceptions_ignored.rpt` also include five generated single-bit
or reset false paths already covered by that clock group, four generated
reset exceptions with no timing path, and two superseded direction-output
bounds from the temporary margin/restore sequence. Those duplicate rows do
not prove a build-time problem. The all-exceptions report additionally flags
an invalid endpoint within the clock-reconfiguration IP's RAM exception;
that generated scope merits a separate IP check, not a broad exception on
user logic.

A bounded in-memory test then exported and reloaded the exact effective
constraints. The round trip preserved the complete clock signature, setup
WNS +0.017 ns, hold WNS +0.060 ns, pulse margin +0.265 ns, zero failing or
unconstrained internal endpoints, and Apple output requirements 10/10/8 ns.
Only after that match, a second reload omitted the one PRE false-path line.
All six capture paths then had a finite 5.000 ns datapath requirement:

| Input | Data path delay | Slack with the 5 ns bound |
| --- | ---: | ---: |
| M2SEL | 3.572 ns | +1.136 ns |
| M2B0 | 3.294 ns | +1.251 ns |
| PHI0 | 3.224 ns | +1.321 ns |
| Q3 | 3.110 ns | +1.420 ns |
| DEVSEL# | 3.155 ns | +1.553 ns |
| 7M | 2.996 ns | +1.712 ns |

Global setup/hold/pulse results and board limits remained unchanged. The
`-datapath_only` requirement still exempted the removal checks, so the
separate false path is unnecessary and defeats the intended maximum delay.
This test supports deleting that one exception. It changed no production
XDC or checkpoint. Evidence: `roundtrip_verification.txt`,
`pre_fix_verification.txt`, `after_pre_fix_paths.rpt`, and
`after_pre_fix_hold.rpt` in the audit folder. Installed Vivado has no
standalone `reset_path` command; `reset_timing` clears all timing constraints,
which is why the verified XDC round trip was used.

The unchanged checkpoint also reports the following fabric/pixel crossing
paths (`crossings.txt`, up to one worst path per endpoint/check):

| Direction and family | Reported paths |
| --- | ---: |
| Fabric to pixel: stable `active_mode` bus | 71 |
| Fabric to pixel: FIFO write Gray pointer | 13 |
| Fabric to pixel: video reset synchronizer CLR | 2 |
| Fabric to pixel: Apple vblank XPM pulse | 1 |
| Fabric to pixel: FIFO reset XPM synchronizer | 1 |
| Fabric to pixel: Apple 50 Hz mode synchronizer | 1 |
| Pixel to fabric: FIFO read Gray pointer | 13 |
| Pixel to fabric: vblank XPM pulse | 1 |
| Pixel to fabric: underrun XPM synchronizer | 1 |
| Pixel to fabric: FIFO reset XPM synchronizer | 1 |

The existing generated XPM exceptions already cover the pulse, underrun and
FIFO reset entries. The existing reset-sync CLR exception covers the video
reset entry. Removing the broad clock group would still require explicit
constraints for the stable mode bus and the first 50 Hz synchronizer stage,
while retaining the generated Gray-pointer maximum-delay and skew limits.
The mode controller changes `active_mode` while video is held in reset;
reset release passes through two pixel-clock flops. That transfer contract
must remain explicit when choosing the mode-bus exception. This census is a
plan for a separate CDC constraint correction, not evidence that deleting
the clock group alone is safe.

A second in-memory test applied a specific CDC replacement after the verified
PRE correction. The unapplied patch is
`build/vivado_constraint_audit/targeted_cdc_unapplied.patch`. It removes only
the fabric/pixel clock group, keeps the generated and reset-sync exceptions,
and adds narrow false paths from `active_mode` registers to the pixel domain
and from the fabric clock to the first 50 Hz flag synchronizer's D pin.
It adds no new timing numbers.

All 26 FIFO pointer paths then had finite generated requirements. The 13
write-pointer paths met 7.499 ns with worst slack +5.883 ns; the 13 read-pointer
paths met 6.733 ns with worst slack +5.230 ns. Clock-to-clock timing queries
returned exactly those 26 bounded paths; the other mapped crossings remained
cut by the narrow or generated exceptions. Global WNS +0.017 ns, hold
+0.060 ns, pulse +0.265 ns, zero failing/unconstrained internal endpoints,
Apple output limits 10/10/8 ns and bus-skew slack +5.430 ns all stayed unchanged.
The ignored Gray maximum-delay entries disappeared. Six ignored rows remained:
four unused generated reset paths and two superseded direction-output limits.

These results validate the exceptions on the existing placement; they do not
measure a fresh build's speed or timing. The audit left its source and
checkpoint unchanged. The subsequent audio/constraints trial applies both
corrections to the production XDC and checks their effective coverage with
`scripts/check_video_bus_constraints.tcl`. Reports: `targeted_cdc_verification.txt`,
`targeted_cdc_summary.rpt`, `targeted_cdc_bus_skew.rpt`, and
`targeted_cdc_ignored.rpt` in the audit folder.

## 1360x768 incremental build

The first 1360x768 run kept the full-build setting from the timing reset.
Its nominal setup slack was -0.031 ns, so it did not export or replace the
firmware. The small preset change was suitable for an explicit incremental
implementation from the prior passing routed checkpoint.

Build `20260927T203823Z-f1f4919d-incremental` used fresh synthesis and the
checkpoint from `20260927T111410Z-f1f4919d-full`. It reused 99.73% of cell
placement and 98.74% of routed nets. Final setup slack is **+0.166 ns**, up
from +0.163 ns. Hold is +0.027 ns, pulse width is +0.265 ns, and bus skew is
+5.831 ns. All 32 Apple/Gray bounds pass, with no failing endpoints, missing
constraint objects or route errors. The build kept the existing constraints
and removed the temporary setup margins before signoff.

| Measurement | Prior full build | Incremental build |
| --- | ---: | ---: |
| Total recorded time | 17m33s | 33m56s |
| Synthesis | 3m15s | 7m44s |
| Implementation | 13m03s | 22m59s |
| Checkpoint import | None | 10m46s |
| Physical optimization replay, within import | None | 7m02s |
| Placement | 2m32s | 3m27s |
| Routing | 5m33s | 2m30s |

Incremental reuse preserved timing and reduced routing time, but checkpoint
import and replay outweighed that saving. These are observed runs, not a
controlled benchmark; synthesis also took longer. Use the archived routed
checkpoint explicitly for small changes when preserving placement matters,
and measure the total time rather than assuming incremental is faster.
The timing folder includes the final incremental reuse report and manifest.
