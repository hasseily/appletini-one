# SmartPort transport checkpoint — 28 October 2026

Step 1 passed on the physical Appletini board in ONE//e mode, with the stock
F1.3.3b3 FPGA. Both client paths passed 10000 reads at each CPU speed, with
SmartPort and Disk II file writes/readbacks between transport reads. A warm
Apple reset during a partial FIFO read also passed recovery in a fresh client.
TURBO acceptance requires the compatibility profile below. External Apple bus
pins remain untested.

This test enables a known byte source on private STATUS `$82`/`$83`, not HTTP
or JSON. See [the implementation and reproduction guide](../../README_NET_TRANSPORT_CHECKPOINT.md).

## Runs

| Client path | Actual CPU speed | Reads | Client version | Result |
| --- | --- | ---: | --- | --- |
| Slot-7 ROM | 1 MHz | 10000 | v1 | Pass; timing values excluded |
| Slot-7 ROM timing sweep | 1 MHz | 1008 | v2 | Pass; two timing samples per size |
| Slot-7 ROM | TURBO | 10000 | v3 | Pass |
| Direct FIFO, libtini-style | TURBO | 10000 | v3 | Pass |
| Direct FIFO, libtini-style | 1 MHz | 10000 | v4 | Pass |

Every stress run covers all 504 requested reply sizes from 9 through 512.
Adjacent calls repeat the same offset and size; headers, bytes, EOF replies,
and untouched buffer tails are checked. Sizes 9–472 have 20 samples each;
473–512 have 18. Each run also completes 79 write/close/open/read/verify cycles
of 512 bytes on each dedicated volume, NETSP and NETD2. The final SD readback
matches the last write round, 9983, on both volumes, and both volume structures
validate. User disk images were never written.

The runner verifies the live ARM function's first word against the selected
ELF, a clean source counter, ONE//e bridge, actual speed, and `c074=0`. It reads
the source counter over UART every 15 seconds. FTP remains off during the run;
the collector uses it only after completion. The per-run JSON and UART files
record the actual speed and slowdown settings, screen, and source count.

## Timing

| Path and speed | Largest complete call | Largest IRQ inner interval |
| --- | ---: | ---: |
| ROM, 1 MHz, later sweep | 11833 | 11694 |
| FIFO, 1 MHz | 11487 | 11313 |
| ROM, TURBO | 14305 | 14148 |
| FIFO, TURBO | 5225 | 5068 |

Units are native VIA1 T1 ticks. The tick rate was not independently calibrated.
Audio was idle and the diagnostic owned VIA1 T1. The IRQ measurement lies
inside the masked window and gives a lower bound; the complete call starts
before SEI and ends after PLP and gives an upper bound. These are observed
maxima for this setup, not future deadlines or an audio acceptance test.

`RS1.LOG`, `L1.BIN.csv`, `RT.BIN.csv`, and `LT.BIN.csv` contain min, max,
and total intervals for every requested size. Their original binary records
are also retained. `board-results.json` validates every row and gives hashes.
The earlier `R1.LOG` proves the native ROM data stress run but its old timer
wrap check is not accepted for timing. The later sweep uses the corrected
check. Its first host watcher ended while the client was still saving its CSV;
the resume capture records the completed pass. That watcher failure is kept
in the record.

## TURBO qualification

Unthrottled Disk II file write verification failed with this diagnostic ARM
image and the stock FPGA, with Disk II acceleration both on and off. One
failed run damaged the owned NETD2 volume header. The failed disk image stays
in the ignored local build directory; its hash and decoded screen samples
are recorded here. Native storage passed after restoring a clean owned volume.

Both accepted TURBO runs use the existing slot-6 slowdown setting: live mask
`$028`, 256 ticks, with Disk II acceleration off. Bit `$020` selects slot 6;
`$008` is the original slot-4 Phasor setting. The native FIFO run uses the same
profile; the earlier native ROM runs use mask `$008`. All have `c074=0`.
The client does not switch clocks around file calls. This checkpoint does not
fix or accept unthrottled TURBO Disk II writes.

## Warm reset

`reset-before.txt` shows the paused marker, FIFO status `$21000000` (256 bytes
unread), generation 4, reset sequence 2, and one data read. Writing 1 to the
ONE//e input bridge control at `$4000017C` applies a virtual warm Apple reset.
It does not reset the ARM, reload the FPGA, or drive the physical Apple RESET
pin.

`reset-after.txt` shows FIFO count zero, generation 5, reset sequence 3, and
zero data reads. ProDOS returns to Bitsy Bye. Down arrow and Return select
NETTEST.SYSTEM and load it afresh, as captured in `reset-fresh-load.txt`.
The P probe compares the saved `RESET.TAG` with its new descriptor, writes
`RESET.OK`, and reports `RESET RECOVERY PASS, GEN=5`. The saved proof and
`reset-results.json` confirm the new generation/sequence and zero read count.

## Provenance and restore

`build-report.json` records the exact selected FPGA, XSA, FSBL, CPU0, CPU1,
embedded CPU1 blob, ARM source hashes, and linked memory reservation audit.
The FPGA was not rebuilt and Golden was not written. `selected-artifacts.json`
records client hashes and original settings hash. The client hashes were also
checked against the SYS files read back from the physical SD volumes. No
private user configuration is included here.

Client v1 produced the native ROM stress log; v2 corrected timer wrap detection
and added the partial-read pause; v3 changed result saving to block-written
binary records; v4 also rejects a recovery-only probe without RESET.TAG and
checks recovery-file close/delete errors. The selected build reports preserve
the source hashes for v1, v3, and v4; the v2 SYS hash comes from SD readback.

The native ARM source test, actual 65C02 ROM/FIFO model, SmartPort service,
SmartPort ROM, AMP startup, and firmware-memory checks pass. The model checks
all sizes, repeat reads, guard corruption, timer wrap, reset, raw timeout,
and poisoned-driver behavior. Model cycles omit real ARM waits and do not
serve as board timing evidence. Logs are beside this file.

The normal local ARM build was restored with the test flag off. The board
was restored through its normal SD updater to the exact stock F1.3.3b3 image,
SHA-256 `cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2`.
The complete upload readback and the resulting FIRMWARE.OK hash both match.
The Apple probe then returns unsupported control, `RC=21`, on the test selector.

The original settings were restored byte for byte and checked again after
reboot. UART confirms STARFALL on SP1, original Disk II media/settings,
TURBO, slowdown mask `$008`, and Disk II acceleration on. Golden still reports
B2.0.6. Only the two owned test disk images and their empty NETTEST directory
were removed. FTP is off and the serial tools have closed their ports.
`restore-settings.json`, `restored-runtime.txt`, `cleanup.json`, and their UART
logs record these checks.

FTP reported media-remount result `-12` when it closed after cleanup. A final
normal reboot cleared that condition. `restored-runtime-final.txt` confirms
SP1 open on STARFALL, the original settings, and FTP off after that reboot;
no FTP session followed it.
