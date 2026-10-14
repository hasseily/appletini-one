# Applesoft acceleration: vTW integration priorities

This review combines the unchanged disk described in [inventory/PROGRAMS.md](inventory/PROGRAMS.md), the ROM map in [maps/README.md](maps/README.md), and the [measured profiles](analysis/PROFILES.md). The rollup contains 62 captures and successful phases from 28 filenames; phases include full finite runs and bounded interactive work. No FPGA, firmware, or physical-card test was performed, so these are workload measurements rather than measured vTW speedups.

## Measured priorities

The three completed plasma runs strongly favor math first, followed by parser and lookup work for the two array-heavy programs. The figures below are **exclusive guest instruction-cycle shares**, from [analysis/profiles.json](analysis/profiles.json).

| Program | Arithmetic | Variable/array lookup | Parser/dispatch | Selected FP kernels 10x faster: whole-program what-if |
|---|---:|---:|---:|---:|
| RANDOM PLASMA | 88.0% | 1.4% | 8.0% | 2.69x |
| FS PLASMA | 63.0% | 17.8% | 15.4% | 1.67x |
| COLOR PLASMA | 53.8% | 19.9% | 18.8% | 1.47x |

The what-if accelerates only the selected add/subtract, multiply, and divide instruction ranges by exactly 10x, holds all other cycles fixed, and adds no service overhead. It is neither an achieved gain nor a vTW prediction; the selected ranges cover only part of the arithmetic category.

FS PLASMA visits the FIN entry **1,344,580** times, PTRGET **2,419,777** times, and ARRAY **537,601** times. This supports investigating exact literal reuse and variable/array address work after math. These are entry-instruction visits, not inclusive durations or independent call totals. FIN also calls math routines, so its full potential saving overlaps arithmetic acceleration.

Graphics plus monitor/IO account for under 0.6% of each plasma run's guest cycles. They are lower priorities for these workloads, although vTW video posting and physical I/O costs need separate measurement. No successful capture visits GARBAG: string handling appears in the corpus, but faster garbage collection needs a separate memory-pressure test before claiming priority. FIFTEEN PUZZLE reaches an error and is excluded from performance aggregates; [raw-byte inspection](inventory/FIFTEEN_PUZZLE_DIAGNOSIS.md) confirmed its supplied self-pointer destination is three bytes too high. Retain it as a compatibility case, not a successful timing result; no correction was applied to the disk.

## Recommended order

1. **Keep the 65C02 and add a small, synchronous ROM-helper service interface.** Establish safe entry, memory ownership, completion, interrupt handling, reset cancellation, and fallback before adding arithmetic. A first helper should perform one operation with a complete, testable Applesoft contract.
2. **Implement exact Applesoft multiply, divide, and add/subtract helpers, then assess conversion and transfer helpers.** Preserve the ROM's representation, normalization, rounding, overflow/underflow, error behavior, and scratch/register results. Leave the existing SIN/COS/power routines calling these faster primitives initially. A standard host floating-point result is not automatically a compatible replacement.
3. **Investigate literal parsing and variable/array lookup as separate candidates.** The FS PLASMA counts and FS/COLOR lookup shares support this next step, even with small variable tables. They need different guards and invalidation rules from the existing byte cache. Measure their inclusive costs and overlap with faster math before choosing between them.
4. **Optimize graphics only where its measured work warrants it.** Native line drawing or block fills must still update shadow RAM and the existing video/capture/motherboard paths. Preserve I/O pacing and intentional game/output delays.

The plasma programs provide repeated math, numeric literals, and variable references; FS PLASMA and COLOR PLASMA add repeated two-dimensional array indexing. GUMBALLS, GEOMETRY, and ISOMETRIC FACTORY add line drawing. SPRITE, IDENTITY CRISIS, and the text games expose string work that math helpers cannot eliminate. This corpus does not establish lookup-cache gains for large variable or line tables.

## Concrete PL interface

The existing takeover loader already patches selected bytes while loading the fixed Enhanced IIe ROM (`ps_sources/frontend/vtw_service.c:339-367,1060-1065`). The shadow contains separate main, aux, and ROM stores; ARM port B can write ROM, while CPU port A cannot (`hdl/apple/vtw_shadow.sv:73-103,119-150`). Either a guarded entry trap or a patched ROM stub can therefore invoke an assist without replacing the whole interpreter.

For a trap, recognize an instruction fetch at a supported entry **and** the active translated ROM mapping and verified ROM image. A PC match alone could intercept language-card RAM. Before servicing, settle the wrapper's outstanding access, capture A/X/Y/S/P and relevant mapping, and give the helper exclusive access to the required memory port. Complete all memory and register effects before resuming at the defined exit or a 65C02 return stub. Unsupported state must execute the original routine.

The CPU already exposes architectural state, but vTW currently wires only its diagnostic PC and builds with `DEBUG_STATE_LOAD=0` (`hdl/apple/vtw_core_top.sv:354-386`). The simulation loader clears pending NMI (`hdl/apple/w65c02_core.sv:1189-1217`); it is unsuitable as an unchanged production return path. A new interface must preserve pending interrupt events and define interrupt precedence around a service. Existing execution latches NMI edges even while stalled (`hdl/apple/w65c02_core.sv:1220-1224`).

Freezing `core_en` alone does not grant memory ownership: the wrapper runs an access ahead and may still reach a cache (`hdl/apple/vtw_core_top.sv:1498-1509`). ARM shadow writes currently invalidate the TURBO caches (`hdl/apple/vtw_core_top.sv:1160-1169`); a new PL helper's writes need the same coherence guarantee. A parked cached response may require reissue (`hdl/apple/vtw_core_top.sv:1856-1863`). Honor the current zero-page/aux mapping when reading FAC, ARG, scratch state, and caller operands.

Prefer local PL math over a CPU0 request for every short operation. SmartPort provides a useful ownership pattern, but its ISR queues work for the shared main loop (`ps_sources/frontend/smartport_service.c:1787-1801`). Even packed ARM shadow reads use MMIO commands, readiness/completion polls, and data reads (`ps_sources/frontend/smartport_service.c:859-893`). This transfer and scheduling cost needs measurement before ARM offload; it can exceed the work saved by short helpers. Larger self-contained routines may amortize it.

Native calls also need explicit virtual-time behavior: virtual Disk II advances from accepted guest-cycle events (`hdl/apple/vtw_core_top.sv:1484-1496`). Keep native BASIC acceleration an explicit option until its timing and compatibility policy is tested.

## Cache boundaries and corpus hazards

| Candidate | What it can reuse | Required compatibility boundary |
|---|---|---|
| Numeric literal cache | Exact parsed numeric result and the validated parser continuation | Match unchanged source bytes, parser context, and active program mapping; reproduce the text-pointer and helper effects. Invalidate on program writes, edits, loads, moves, and lifecycle changes. |
| Scalar lookup cache | Address of a variable, not its value | Match Applesoft name/type semantics and the current variable-table generation. New variables, table movement, CLR/RUN/NEW, loads, and direct memory changes can make old addresses stale. |
| Array descriptor/offset assist | Validated descriptor, dimensions, bounds, and element-address arithmetic | Preserve default allocation, bounds errors, subscript conversion, element size, and relocation. The scalar and array tables can move; a cached array pointer is not permanent. |
| Expression or loop cache | Only a separately specified, guarded execution contract | This is broader work. Preserve RND and other side effects, input-dependent values, modified loop variables, and direct interpreter-pointer changes. Do not assume a fixed FOR trip count. |

SIERPINSKI, CONCENTRATION, and FIFTEEN PUZZLE directly POKE `$B8`, the low byte of the interpreter text pointer. A cache must observe this even when source bytes have not changed. MIND THE GAPS changes LOMEM and shape pointers. Several programs allocate temporary strings and need correct relocation/garbage collection. These are useful compatibility tests, not unusual cases to remove from the benchmark.

## I/O and performance interpretation

Graphics writes pass through posted/coalesced video machinery, and pending video state can delay subsequent I/O (`hdl/apple/vtw_core_top.sv:1432-1463,1841-1853`). Bulk PL drawing must participate in that ordering. Keep keyboard, paddle, speaker, and card accesses on their defined paths. Report SYS.DIAG's SPEED=160, STACKER's SPEED=245, explicit FOR delays, and GET/INPUT waits separately rather than silently deleting them.

The histogram records **exclusive cycles at each executed PC**. Arithmetic called by SIN or FIN is charged to arithmetic, not again to SIN or FIN. Entry counts show execution frequency, not inclusive routine cost; alternate internal entries and shared helpers matter. Do not add an entry count, an exclusive category percentage, and a putative inclusive saving.

GSSquared guest cycles are a workload profile, not vTW fabric clocks or wall-clock speed. vTW has different cache, bus, pacing, and video costs. For a disjoint fraction `f` improved by factor `s`, `1 / ((1-f) + f/s)` is only a model with the remaining work held fixed. Service overhead, cache effects, I/O, and overlap between candidates must be included before reporting a predicted gain. Capture wall seconds include host control and pauses and are not a clean emulator throughput benchmark.

Qualify each helper with differential execution against the exact ROM: normal and boundary values, error paths, multiple valid entry points, registers/flags/stack, all changed memory, interrupts/reset, mapping changes, and cache coherence. Then replay the disk workloads with matching random state and compare final screen/output and relevant guest state. Only a vTW measurement after integration can establish the hardware speedup.
