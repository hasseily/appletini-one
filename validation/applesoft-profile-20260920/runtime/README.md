# Temporary GSSquared guest profiler

Source revision: `695e627acb8154adfe7ea5ac972b32fbd50e2c3a`.

`GSSquared-profile.exe` is the measured build. It contains the temporary
`gssquared-instrumentation.patch` and `GuestProfiler.hpp`. The tracked sources in
`C:/Users/hasse/source/repos/gssquared` were restored byte-for-byte after building.
Their mtimes were advanced so the next ordinary build recompiles uninstrumented code.

The study is complete. The emulator exited through protocol QUIT, and
`original/GSSquared.exe` has been restored to the build directory and hash-checked.
The instrumented executable remains here as `GSSquared-profile.exe` for replay.
Do not rebuild restored source while using a profiling binary; a normal rebuild
removes instrumentation. A replay must back up and restore its runtime executable.

Set `GS2_GUEST_PROFILE_DIR` to an existing directory before launching. Use
Enhanced IIe (`-p 3`) for the 65C02. Mount a working copy of the input disk with
`-ds6d1=PATH`. Shutdown with protocol QUIT.

Write a complete temporary command file, then atomically replace
`<profile_dir>/control.txt`:

- `1 START run_name 100000000`: count exactly 100 million completed instructions.
- `2 START run_name 0`: count without an instruction limit.
- `3 STOP run_name`: stop the active capture.

Sequence numbers must increase. Names may contain ASCII letters, digits,
underscores, and hyphens. Each process starts with sequence zero. START refuses to
replace an active capture. Keep all command operations serialized.

Pause the emulator before START, wait for `ack.json` to show its sequence and
`state: active`, then continue. For a complete program, an execution breakpoint
can stop at the selected end boundary; issue STOP after the stop. For an endless
program, the instruction limit disables recording at the exact boundary while
guest execution continues. The next frame writes CSV, JSON, and a complete ACK.
The host can then pause the emulator. STOP on an already complete run is harmless.

CSV rows contain decimal PC, instruction count, and guest cycles. Counts and
cycles are exact over the captured interval, not sampled. Hardware IRQ entry
events/cycles are separate from instruction totals. The profile does not count
unexecuted reset, RDY, or stopped-clock intervals as instructions. Compare
`guest_cycle_span` against `accounted_cycles` to detect such gaps.

The capture uses instruction-entry PC and cycle count, followed by the completed
instruction's cycle count; it does not depend on trace-ring retention. It is
intended for the unbanked 65C02; 65816 banked PCs would be merged.

Clock mode zero is GSSquared maximum speed. Shift-F9 from initial 1 MHz selects
mode zero. SDL scancode 66 and modifier 1 encode Shift-F9 in the debug protocol.
Allow a running frame after switching speed, since paused execution does not
apply pending speed changes. JSON records `clock_mode_at_start`.

`capture_wall_seconds` includes host pauses/control latency and must not be
treated as a clean throughput benchmark. These are standard guest CPU cycle
profiles; they do not model vTW TURBO cache stalls or physical Apple bus timing.

`build-validation.json` records hashes and the passed standalone profiler tests.
`profiler-test/` proves exact instruction-budget stopping, IRQ accounting,
per-PC sums, host STOP, and separate consecutive captures.
