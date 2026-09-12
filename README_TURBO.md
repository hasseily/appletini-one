# vTW TURBO

TURBO is an execution mode separate from the 1 MHz through MAX presets.
Select **TURBO** after MAX in the TransWarp speed selector, or send
`vtw speed turbo` over UART. USB speed-up moves from MAX to TURBO;
speed-down returns to MAX. The 1 MHz toggle and optional slug toggle return
to the selected TURBO setting when released. Profiles store
`vtw.speed.mode=3`; existing mode values keep their meanings.

This firmware uses version **F1.0.9-turbo1** on branch `turbo`.
TURBO has no fixed MHz rating: cache misses and the instruction mix change
its rate. Simulation measurements and routed timing accompany the built
image in `firmwares/TURBO_BUILD.json`. They do not establish operation on a
physical Apple II. The build does not flash the card.

The integrated 16-byte indexed-copy benchmark, including loop control,
measured the following fabric-clock counts. Code ran at `$F000`; both source
layouts (`$9000` and `$9080`) produced the same counts:

| Pass | MAX | TURBO | Speedup |
|---|---:|---:|---:|
| First pass, cold cache | 1,080 | 649 | 1.66 times |
| Second pass, warm cache | 1,064 | 476 | 2.24 times |

At 133.333 MHz fabric, the warm result equals about 74.5 MHz of classic
65C02 work for this program. It is a simulation result for one small
workload, not a general application-speed rating.

## Execution and memory

The existing four-clock memory path remains in place for modes 0–2.
A TURBO cache miss adds one lookup clock before that path. The shared
fabric clock stays at about 133 MHz.

`vtw_turbo_cache.sv` adds two independent caches:

- A 64-entry address table holds 32 read mappings and 32 write mappings
  for 256-byte pages. Page tags distinguish entries that share an index.
  Only shadow-backed pages outside `$C000–$CFFF` enter the table. A store
  can hit this table without a byte-cache hit.
- A 256-byte distributed-RAM cache serves reads without the shadow BRAM's
  response pipeline. Its index folds high address bits into low bits to
  reduce collisions between code and buffers; address tags distinguish
  every byte. Read hits do not need an address-table hit, so replacing a
  mapping does not discard a valid cached byte.

A cache hit takes two fabric clocks: lookup captures the byte or write
mapping and its tag, then execute checks the tag and completes the CPU
cycle. A writable address hit commits its byte to shadow BRAM on that edge.
Video and overlay writes retain the existing posted-write path.
The byte cache snoops every CPU shadow write. It updates a read entry only
when its physical read page matches the write page; this preserves distinct
RAMRD and RAMWRT banks and self-modifying code.

Every `$Cxxx` access takes the original path and invalidates both caches.
That preserves soft-switch ordering, language-card double-access rules,
and C8 ROM ownership. ARM shadow writes, RAMWorks DMA hold requests, resets,
mode changes, and changes to write-through policy also invalidate caches.
PSRAM and private-card responses never enter the byte cache.

The CPU keeps one set of architectural registers when modes change. TURBO
can retire ordinary implied and accumulator instructions at opcode fetch,
fold zero-page indexing into operand handling, and omit taken-branch dummy
cycles outside the I/O region. It keeps decimal correction, read-modify-write,
stack, interrupt, WAI, and STP sequencing. A false or unconnected CPU `turbo`
input selects the original cycle-exact behavior. Each instruction samples
TURBO permission at fetch; a slow request can cancel later shortcuts.

## Timed I/O

Real bus transactions and posted motherboard writes keep their existing
timing and ordering. `$C074`, per-region slowdowns, native Disk II accesses,
and Disk II write mode still take priority over the selected speed.

While the private Disk II read path is active and its motor spins, TURBO
uses the existing MAX path and classic CPU instruction cycles. This keeps
one virtual Disk II tick per classic CPU cycle. Native accesses and Q7 write
mode still force 1 MHz. Once the motor's spin-down interval ends, TURBO
resumes. Merely enabling the Disk II card does not disable TURBO.

TURBO does not preserve cycle-counted RAM loops or dummy memory-read timing.
Use a 1–MAX preset for software that depends on those details. Selecting
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
```

The CPU tests compare both modes with the independent SingleStepTests
architectural results, retain exact bus-cycle checks for the classic mode,
run Klaus functional/extended/decimal/interrupt programs, and compare CPU
writes and I/O reads across modes. The integrated TURBO bench checks actual
program output, bank selection, CPU/ARM cache coherence, pause and DMA hold,
mode changes, reset, posted writes, and real I/O stalls. Disk II benches
include TURBO in their speed and WOZ read matrices.

Build a new bitstream and its matching XSA, then rebuild the PS applications:

```powershell
vivado -mode batch -source scripts/create_project.tcl
$env:APPLETINI_FULL_BUILD = '1'
$env:APPLETINI_TIMING_DIAGNOSTICS = '1'
vivado -mode batch -source scripts/build_and_export_xsa.tcl
vitis -s scripts/create_vitis_workspace.py
```

Package with explicit FSBL, passing bitstream, and frontend ELF paths using
`scripts/make_firmware_bin.bat`. Verify the image with
`python scripts/image_manifest.py verify firmwares/FIRMWARE_TURBO.BIN --role firmware --require-recovery-capable`.
The companion build record identifies inputs, source hashes, validation
results, timing, image size, and SHA-256. Generated images and logs remain
outside source commits.
