# vTW TURBO

TURBO is an execution mode separate from the fixed 1 MHz through 33 MHz presets.
Check **Enable TURBO speed** below Speed in the TransWarp tab to make
**TURBO** appear after 33 MHz in the speed selector. The checkbox defaults to
off, and profiles store the choice as `vtw.turbo.enabled`. With it checked,
`vtw speed turbo` over UART selects TURBO, and USB speed-up moves from 33 MHz
to TURBO; speed-down returns to 33 MHz. With it unchecked, the speed selector
omits TURBO, USB speed-up stops at 33 MHz, and UART cannot select TURBO. Unchecking
it while TURBO is selected changes the speed to 33 MHz.

The 1 MHz toggle and optional slug toggle return to the selected TURBO
setting when released. Profiles store `vtw.speed.mode=3`; existing mode
values keep their meanings. The checkbox rows pair Enable TURBO speed with
the slug debug key, Ignore $C074 with Disable DiskII Acceleration, and Slow
Floating bus with Slow Paddles/joystick.

USB joystick and paddle setup is under **USB > Joystick / Paddles**; see
[the joystick guide](README_USB_JOYSTICK.md).

## Execution and memory

The existing four-clock memory path remains in place for modes 0–2.
A TURBO shadow-read miss issues RAM from the lookup stage, avoiding a
second route stage. The shared fabric clock stays at about 133 MHz.

`vtw_turbo_cache.sv` adds two independent caches:

- A 64-entry address table holds 32 read mappings and 32 write mappings
  for 256-byte pages. Page tags distinguish entries that share an index.
  Only shadow-backed pages outside `$C000–$CFFF` enter the table. A store
  can hit this table without a byte-cache hit.
- A 128-byte distributed-RAM cache holds 32 aligned four-byte words.
  A shadow read fills all four bytes through a real 32-bit BRAM port.
  Its index folds high address bits into low bits to reduce collisions
  between code and buffers. Logical tags identify words; physical-page
  tags make byte-write snoops independent of address-table eviction.

A cache hit takes two fabric clocks: lookup captures the byte or write
mapping and its tag, then execute checks the tag and completes the CPU
cycle. A writable address hit commits its byte to shadow BRAM on that edge.
Video and overlay writes use the direct renderer path described below.
The word cache snoops every CPU shadow write. It updates a byte only
when its physical read page matches the write page; this preserves distinct
RAMRD and RAMWRT banks and self-modifying code.

Every `$Cxxx` access takes the original path. Changes to the full translation
state invalidate both caches; harmless I/O polling retains cached RAM.
This preserves soft-switch ordering, language-card double-access rules,
and C8 ROM ownership. ARM shadow writes, RAMWorks DMA hold requests, resets,
mode changes, and changes to write-through policy also invalidate caches.
PSRAM and private-card responses never enter the word cache.

### Extended-memory transfers

While vTW owns the Apple bus, PSRAM now admits each fixed eight-byte request
as soon as the service can accept it. Native bus operation keeps its bounded
admission window. Bus handback drains accepted work and the driver's CE-high
rest during the existing release guard before native reads resume.

The [AMEM copy/fill API](README_MEMORY_API.md) now submits each descriptor to
an FPGA engine. Copies and fills use shadow BRAM directly and retain PSRAM
source and destination lines in the engine. ARM no longer moves each word
through registers or a DDR buffer. Aligned shadow transfers use four-byte
accesses; partial PSRAM writes preserve bytes outside the destination range.
The API still holds the CPU for the batch and invalidates its caches.

In real-driver simulation, 32 PSRAM reads took 1,022 fabric clocks with vTW
ownership versus 4,042 clocks under native admission. This measures the
scheduler alone; it is not an application or hardware speed rating. The
largest observed handback drain was 45 clocks, within the 130-clock release
guard used by the test.

Run `python scripts/test_psram_owned.py`,
`python scripts/test_vtw_copy_engine.py`, `python scripts/test_memory_api.py`,
`python scripts/test_memory_api_hw.py`, and `python scripts/test_vtw.py` for
the transfer, reset, ownership, and CPU regressions.

The CPU keeps one set of architectural registers when modes change. TURBO
can retire ordinary implied and accumulator instructions at opcode fetch,
fold indexing into operand handling, and omit safe branch, indexed-access,
read-modify-write, and stack dummy cycles outside I/O. Actual stack accesses,
decimal correction, interrupts, WAI, and STP keep their required semantics.
JSR still fetches its high operand after the stack writes, including when
those writes modify the instruction itself. A false or unconnected CPU `turbo`
input selects the original cycle-exact behavior. Each instruction samples
TURBO permission at fetch; a slow request can cancel later shortcuts.

Both shadow ports now read aligned 32-bit words while retaining scalar byte
accesses. Aligned ARM packed reads/writes use one RAM access per word;
unaligned transfers retain the byte fallback. No decoded instruction/block
cache or ARM CPU translator is included.

## Timed I/O

Real bus transactions keep their electrical timing. `$C074`, per-region slowdowns, native Disk II accesses,
and Disk II write mode still take priority over the selected speed.

The private Disk II no longer forces 33 MHz while its motor spins. Each accepted
CPU step reports the number of classic cycles it represents, including
omitted cycles. The controller replays those ticks before the next access;
WOZ track data and weak-bit state must be ready before replay advances.
This retains the virtual bit-cell ratio without restoring idle CPU work.
Native accesses and Q7 write mode still force 1 MHz.

## Video writes

TURBO sends each video/overlay write directly into the ordered capture stream
at fabric speed. Renderer backpressure remains lossless. A separate BRAM
mirror retains the latest byte for each dirty motherboard address and drains
through the existing physical bus engine. Repeated writes may coalesce on
the motherboard; Appletini's renderer receives every accepted write.

On ONE//e and physical //e hosts, TURBO now separates renderer capture from
physical mirror urgency. Text/lores and MAIN HGR pages that the current
display mode does not use remain dirty in bank-tagged mirror RAM. Ordinary
I/O and RAMWRT changes can proceed without draining those inactive pages.
Every write still reaches the renderer, including hidden-page drawing.
Mixed mode retains text, and 80STORE keeps display page selection on page 1.
AUX graphics/SHR, paged MAIN SHR, armed overlays and the legacy Li control
holes retain conservative immediate mirroring. Physical II/II+ hosts retain
the immediate policy because their motherboard cannot steer AUX with //e
soft switches. All classic speed presets retain their existing write path.

Display-mode changes, card ROM/DEVSEL entry, RamWorks bank changes, speed
changes, ARM memory holds and handback drain the deferred banks first.
The flush saves the actual physical bank switches, writes each dirty bank,
and restores RAMWRT/PAGE2 before the waiting access resumes. It never changes
80STORE, HIRES or RamWorks switches. Renderer frame records retain the saved
switch values during this temporary steering. Apple RESET retains the existing
behavior of discarding queued mirror data.

When a mirror is pending, Cxxx accesses check video exposure after capturing
the CPU address and before applying any I/O side effect. This keeps the low
address-bit decoder off the wide capture enable. Only pending-video I/O gains
one fabric wait state; RAM and cache accesses keep their existing latency.

The integrated test writes 1,024 bytes before the physical copy has finished.
The bus remains owned until the final physical write and bank restoration finish.
Mirror cycles do not overwrite newer direct records in the renderer.
Motherboard video can lag the local display and omit intermediate values;
use a classic preset when those physical raster effects matter. The same
direct path removes the synthetic 1 MHz video bottleneck in ONE//e mode.

The mode-aware regression writes MAIN and AUX `$08DF` in full-screen DHGR,
switches RAMWRT and reads `$C020` without issuing a physical mirror write;
the renderer already holds both updated bytes. A later display-page switch
must flush both banks before it reaches the bus. Other cases cover hidden
graphics, mixed text, renderer backpressure, all classic speed codes, TURBO
exit/re-entry during a stalled write, and ARM holds or aborts between private
and physical switch updates. The focused
policy and bank-sync benches run through `scripts/test_vtw_video_policy.py`
and `scripts/test_vtw_video_bank_sync.py`. These are simulation checks;
physical-board validation is still pending.

`vtw status` reports fabric clocks, accepted CPU steps, represented classic
cycles, cache read hits/misses, invalidations, Disk II waits and video waits.
The 32-bit counters wrap and clear with `busdbg clear`. They provide interval
measurements; live reads are not an atomic snapshot of all counters.

Direct TURBO video writes now enter through a registered admission state.
Address classification and shadow writes finish before the renderer and
deferred mirror accept the saved byte together. Display policy uses the
switch state captured with that byte. An ARM hold waits for the staged write
and its mirror to drain. This adds one fabric clock (about 7.5 ns) when a
posted write could previously enter both consumers at once. A write that
already needed to wait gains no extra cycle. The video-wait counter includes
this admission cycle; classic posted writes keep their existing timing.

TURBO does not preserve cycle-counted RAM loops or dummy memory-read timing.
Use a 1 MHz through 33 MHz preset for software that depends on those details. Selecting
1 MHz restores the classic path; throttling TURBO instructions alone would
not restore omitted cycles. An instruction already shortened before a live
mode change cannot recover its elapsed cycles.

## Validation and build

Run the instruction tests and integrated vTW tests before building:

```powershell
python scripts/test_w65c02_turbo.py --limit 0
python scripts/test_vtw.py
python scripts/test_vtw_turbo_frontend.py
python scripts/test_onee_vtw_runtime.py
python scripts/test_onee_bus_integration.py
python scripts/test_onee_video_path.py
python scripts/test_onee_disk2_boot.py
python scripts/test_disk2_turbo_time.py
python scripts/test_vtw_shadow_wide.py
python scripts/test_vtw_video_path.py
python scripts/test_vtw_video_integrated.py
```

The CPU tests compare both modes with the independent SingleStepTests
architectural results, retain exact bus-cycle checks for the classic mode,
run Klaus functional/extended/decimal/interrupt programs, and compare CPU
writes and I/O reads across modes. The integrated TURBO bench checks actual
program output, bank selection, CPU/ARM cache coherence, pause and DMA hold,
mode changes, reset, posted writes, and real I/O stalls. Disk II benches
include TURBO in their speed and WOZ read matrices.

The outbound Disk II time-ready logic keeps the live session bypass at the
last gate, shortening the path into TURBO shadow-RAM write enable without
adding a clock or changing local replay. The Disk II launcher compares the
production expression with the previous readiness rules across all
1,048,576 combinations of its input predicates before running the controller
benches.

Build a new bitstream and its matching XSA, then rebuild the PS applications:

```powershell
vivado -mode batch -source scripts/create_project.tcl
$env:APPLETINI_FULL_BUILD = '1'
$env:APPLETINI_TIMING_DIAGNOSTICS = '1'
Remove-Item Env:APPLETINI_POSITIVE_SLACK_ONLY -ErrorAction SilentlyContinue
vivado -mode batch -source scripts/build_and_export_xsa.tcl
vitis -s scripts/create_vitis_workspace.py
```

Follow [the Vivado build guide](README_VIVADO.md) for current timing gates and
image packaging.
