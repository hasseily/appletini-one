I ran all 29 disk entries in GSSquared's Enhanced IIe mode with the stock Applesoft ROM and the 65C02, at maximum speed. The results support starting with exact arithmetic helpers, then array indexing and variable lookup. Pixel plotting is a small part of the longest graphics programs' CPU work.

The study contains 62 captures: 61 useful captures covering 28 entries, and one failed program. Twenty-one entries reached their normal end, including unattended game losses and a scripted DESERT ISLAND solution. Seven others have startup, key-response, or bounded-running profiles. GEOMETRY and GEOMETRIC contain identical programs. FIFTEEN PUZZLE fails in the supplied disk and is excluded from performance priorities.

| Program / measured phase | Guest CPU cycles | CPU time at exactly 1 MHz | Host capture duration | Arithmetic | Variable / array lookup | Parser / dispatch |
|---|---:|---:|---:|---:|---:|---:|
| RANDOM PLASMA, complete | 4,347,854,978 | 72.5 minutes | 37.8 s | 88.0% | 1.4% | 8.0% |
| FS PLASMA, complete | 9,339,647,629 | 155.7 minutes | 85.9 s | 63.0% | 17.8% | 15.4% |
| COLOR PLASMA, complete | 4,363,603,356 | 72.7 minutes | 40.6 s | 53.8% | 19.9% | 18.8% |
| TESSELLATOR, draw after input 64,2,0 | 852,249,218 | 14.2 minutes | 7.8 s | 70.4% | 11.3% | 12.1% |
| TRUCHET TILES, complete | 274,721,368 | 4.6 minutes | 2.7 s | 40.2% | 22.3% | 28.8% |
| SIERPINSKI, complete | 38,982,704 | 39.0 seconds | 0.4 s | 36.7% | 25.8% | 23.3% |

Percentages count cycles executing instructions in each ROM region, without counting a callee twice. Arithmetic includes shared floating-point conversion and support routines. The 1 MHz column is cycles divided by one million; it is not a timed physical Apple II run. Host durations include profiler/control overhead, especially noticeable for short captures. None of these columns measures vTW hardware speed.

![Measured CPU cycle shares](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/analysis/workload-shares.png)

The specific changes these profiles support, in order:

1. **Exact floating-point multiplication, division, addition/subtraction, and their shared support paths.** Keep the 65C02 and the existing interpreter. Accelerate guarded ROM entries locally in the FPGA, preserving Applesoft's result format, rounding, errors, scratch memory, registers, and return behavior. Existing SIN/COS and numeric conversion routines will benefit when they call those primitives. RANDOM PLASMA enters SIN 107,520 times, and its arithmetic work dominates the run.
2. **Array index multiplication and variable lookup.** FS PLASMA visits ARRAY 537,601 times and PTRGET 2,419,777 times. The integer array-index multiplier alone consumes 7.59% of FS PLASMA and 8.02% of COLOR PLASMA. Scalar table scanning alone consumes 9.99% of SIERPINSKI and 10.84% of TRUCHET TILES. These are concrete candidates for native operations or guarded caches. Preserve allocation, subscript conversion, bounds checks, and table movement.
3. **Repeated numeric literals and the character/parser path.** FS PLASMA enters FIN 1,344,580 times; COLOR PLASMA enters it 734,726 times. A literal cache could avoid repeated parsing of unchanged constants. FIN's own body accounts for only 3.3% and 3.9%, but its arithmetic callees add work already counted under arithmetic, so those percentages do not bound a full literal-cache saving. CHRGET alone costs 7.66% of SIERPINSKI and 8.74% of TRUCHET TILES. Measure inclusive helper costs before choosing between a literal cache and broader parser changes.
4. **Targeted output and graphics helpers.** FIZZBUZZ spends 6.18% in decimal-output conversion. High-resolution graphics routines account for 13.6% in ISOMETRIC FACTORY and 11.1% in SIERPINSKI, but only 0.23–0.50% in the three plasma programs. Graphics expression evaluation is charged to parser/arithmetic rather than the drawing routines. Native plotting helps some workloads; these measurements give arithmetic a much larger first target.
5. **Retain garbage collection and line-search improvements as separate benchmark work.** No successful capture entered GARBAG, and these short programs barely exercise line search. This disk does not establish the benefit of a new collector or a large-program line index. Add a long-lived string workload and programs with large line/variable tables before ranking those changes.

For scale, a purely hypothetical 10x improvement to the selected add/subtract, multiply, and divide instruction ranges gives an overall modelled gain of **2.69x for RANDOM PLASMA, 1.67x for FS PLASMA, and 1.47x for COLOR PLASMA**. This holds every other cycle fixed and adds no helper-transfer overhead. It is not a measured result or a promise; the broad arithmetic share includes more routines than that selected subset. Gains from overlapping math, literal, and parser changes cannot simply be added.

The [vTW integration study](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/integration-priorities.md) identifies the current ROM-load patch path, CPU state interface, memory ownership, cache invalidation, interrupt handling, and video ordering that a native helper must use. A local PL helper is the first design to test; a request to the ARM side for every short arithmetic operation could cost more than it saves.

The disk also supplies useful compatibility checks. SIERPINSKI and CONCENTRATION deliberately change Applesoft's text pointer with POKE 184, and they ran successfully. Several games change FOR counters. MIND THE GAPS changes LOMEM and shape-table pointers. Optimizations must preserve these behaviors.

FIFTEEN PUZZLE's stored pointer destinations are three bytes too high. Its first jump lands at `$0869`, inside `Z=1`, where the required statement separator is `$0866`; the captured text pointer matches this fault. The later hardcoded destinations have the same discrepancy. The source-derived corrections would be 84 to 81 and 216 to 213, but I did not apply or test them. See the [byte-level diagnosis](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/inventory/FIFTEEN_PUZZLE_DIAGNOSIS.md).

The capture procedure and checks were:

- Boot a working copy of the DOS 3.3 disk on Enhanced IIe platform 3. Select maximum speed with Shift-F9 and verify `clock_mode_at_start = 0` for every capture.
- LOAD each original program and compare its loaded token bytes with the disk extraction. Start ordinary captures at ROM RUN `$D912`, after disk loading. Set all five RND seed bytes to `80 4F C7 52 58` for repeatable random workloads. FIZZBUZZ's earlier capture used the existing seed and does not call RND.
- Stop complete runs before the BASIC prompt at `$D43F`. Stop interactive phases before RDKEY `$FD0C`; resume key-response measurements after RDKEY accepts the key. This excludes time waiting for host input. TESSELLATOR's draw and TARGET PRACTICE's shot start after INPUT accepts their fields.
- Record exactly 100 million instructions for each bounded STACKER and FIREWORKS run. The profiler stops counting at the limit; the guest continues until the host pauses it. Their later memory/register snapshots are labelled separately from the exact profile endpoint.
- Preserve intentional delays: SYS.DIAG uses SPEED=160, STACKER uses SPEED=245, and several games contain explicit delay loops. SYS.DIAG spends 73.9% in monitor/IO routines. PC-only profiles cannot separate every deliberate BASIC loop delay from useful work, so those are not treated as pure throughput tests.
- Check the emulator's `$D000–$FFFF` ROM bytes against the local Enhanced IIe ROM. Verify a 1,000-instruction capture against the independent trace: all per-PC counts and cycles match, totalling 3,661 cycles. Every capture's histogram total matches its guest-cycle span. GEOMETRY and GEOMETRIC both produce exactly 6,425,890 instructions and 20,856,081 cycles.

Across useful captures, the profiler counted **6,151,771,196 instructions and 20,135,577,309 guest CPU cycles**. This sum mixes full programs and explicitly documented phases; it is a validation total, not a comparative benchmark score.

The original disk and working copy both retain SHA-256 `a886567d9400af183cf85dbfca84a95e5ec25ad18d2e4036ce725c95a694fbe6`. GSSquared exited through its debug QUIT command. Its original executable and all temporarily instrumented source files have been restored and hash-checked. No Appletini firmware or HDL was changed, built, flashed, or tested on hardware.

The retained artifacts are:

- [Detailed profiles](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/analysis/PROFILES.md), [machine-readable results](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/analysis/profiles.json), and [narrow target-region costs](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/analysis/target_regions.csv).
- [Disk inventory and controls](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/inventory/PROGRAMS.md), exact tokens, readable listings, per-PC CSV captures, register/text snapshots, and graphics-memory captures.
- [Profiler build and control notes](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/runtime/README.md), saved instrumentation, instrumented executable, original backups, and build/check results. The executable depends on the existing GSSquared runtime; this is not a portable package.
- [Capture harness](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/profile_harness.py), finite/interactive/shot runners, and [rollup script](C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/rollup_profiles.py). The rollup can be rerun without the emulator.
