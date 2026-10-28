# SmartPort networking transport checkpoint

Step 1 of [the HTTPS/JSON plan](README_HTTPS_JSON_PLAN.md) tests the existing
SmartPort transport with a known byte source. It does not enable HTTP or JSON.
The full network byte contract remains step 2.

The checkpoint passed on the physical board in ONE//e mode on 28 October
2026: 40000 stress reads, a separate 1008-read timing sweep, interleaved
SmartPort and Disk II file writes/readbacks, and warm-reset recovery from a
partly consumed reply. TURBO requires the slot-6 compatibility setting below.
The [validation record](validation/net-transport-checkpoint-20261028/README.md)
holds the UART captures, per-size measurements, source hashes, and restoration
evidence. This does not prove the external Apple bus pins.

## Firmware

`net_transport_test.c` serves STATUS selectors `$82` and `$83` on controller
unit 0, only with `APPLETINI_NET_TRANSPORT_TEST=1`. Normal builds return the
existing unsupported-control result. The module performs no network, storage,
DMA, or allocation work. Each call produces at most 512 bytes in the existing
SmartPort scratch buffer and OUT FIFO. The selected FPGA and platform are the
exact F1.3.3b3 inputs; this checkpoint does not rebuild PL or write Golden.

The dispatcher requires a ten-byte frame: STATUS opcode followed by all nine
parameter bytes. Byte 0 of the list is parameter count 3; byte 1 is unit 0;
bytes 2–3 are the Apple destination; byte 4 is the selector. For `$83`, list
bytes 5–6 contain the little-endian offset, and 7–8 contain the maximum reply
size, from 9 to 512. Invalid sizes and malformed frames return BADCTL.

These are **diagnostic ABI 0.1** bytes, not a production capability claim:

| STATUS `$82` byte | Meaning |
| --- | --- |
| 0–3 | `ATNT` signature |
| 4–7 | Major 0, minor 1, descriptor size 32, nonzero byte generation |
| 8–9 | Capabilities `$8000`: test source only |
| 10–11 | Source READY = 1, result 0 |
| 12–15 | Source length 32768 |
| 16–17, 18–19 | Minimum 9 and maximum 512 reply bytes |
| 20–21, 22–23 | Read header size 8, pattern version 1 |
| 24–27 | Observed Apple reset sequence |
| 28–31 | Accepted data-read count since that reset |

STATUS `$83` returns a byte generation, source state (MORE = 1 or END = 2),
32-bit offset, 16-bit data length, and up to 504 data bytes. Integer fields
are little endian. For source byte `p`:

```text
((p * 73 + (p >> 8) * 19 + 0x5A) XOR (p >> 4)) AND 255
```

Reads have no cursor. Every offset from 0 through 65535 is valid; an offset
at or past 32768 returns an eight-byte END reply with no data. The reply never
exceeds the supplied limit. The Apple reset sequence resets the counter and
advances generation, skipping zero. A reset detected while ARM builds a reply
prevents it from publishing that reply.

## Apple diagnostic

`software/net_transport_checkpoint/main.c` uses the same assembly client in
two modes: the existing slot-7 ROM entry at `$C70D`, or the direct FIFO path
used by libtini AMEM. Both own C8 with IRQs masked, restore INTCXROM and the
caller IRQ policy, and release C8. Identification checks the existing C700
CRC `$95D9` before any private FIFO command. The direct path has AMEM's
240-VBL-transition timeout and stops after an uncertain reply. The ROM itself
still has no timeout.

The test makes 10000 reads per run, using every size from 9 through 512.
Each adjacent pair uses the same offset and size. It checks the frame,
pattern, EOF cases, and bytes beyond the reply. Every 128 reads, it writes,
closes, opens, reads, and verifies a 512-byte scratch file on each of two
dedicated volumes. `NETSP` uses SmartPort; `NETD2` uses Disk II. These files
and disk images belong only to this diagnostic.

VIA1 T1 in slot 4 measures call and IRQ-mask duration. Audio must be idle and
Mockingboard-compatible timer access available. Each call starts with a fresh
65535-tick interval; a timer underflow rejects the measurement rather than
silently accepting a wrapped time. The logs retain raw native VIA ticks;
their rate is the native Apple bus cadence, not the accelerated CPU rate.
Timer reads use high/low/high to avoid a torn counter value. The saved IRQ
interval lies inside the masked window and is a lower bound; the complete
call interval starts before SEI and ends after PLP and is an upper bound for
that window. The collector labels both bounds rather than presenting the
inner interval as an exact masked duration. Model instruction cycles omit real ARM
scheduling waits and cannot replace these board timings.

The four run logs are `R1.BIN`, `RT.BIN`, `L1.BIN`, and `LT.BIN` on NETSP.
Each records min, max, and total ticks for every supplied reply size. Choose
R or L, then 1 or T for the speed label; set the actual speed through Appletini
before starting. The label itself does not set CPU speed. S runs a shorter
1008-read ROM timing sweep, two reads per size, and saves `RS1.BIN`.

Each binary log has a 32-byte header followed by 504 records of 18 bytes:

| Header byte | Meaning |
| --- | --- |
| 0–3, 4 | `NTM1`, log format version 1 |
| 5, 6 | Mode 0 = ROM or 1 = FIFO; ASCII speed label `1` or `T` |
| 7 | Bit 0 = passed, bit 1 = timer wrap checked |
| 8–9, 10–11, 12–13 | Completed reads, row count 504, row size 18 |
| 14–15, 16–17 | Minimum size 9, maximum size 512 |
| 18, 24 | Service generation; unit 1 = native VIA ticks |

All other header bytes are zero. Each row has five little-endian 16-bit
values (sample count, call min/max, IRQ inner min/max), then two 32-bit totals
(call, IRQ inner). The collector also writes a CSV with the size for each row.
Earlier client versions saved CSV files directly with a `.LOG` suffix; the
collector accepts both formats.

X saves the current descriptor to `RESET.TAG`, starts a 512-byte raw FIFO
read, consumes 256 bytes, then pauses with 256 payload bytes still in the
FIFO. The screen says `RESET NOW: PARTIAL FIFO READ PAUSED`. Apply a warm
Control-Reset at that point, load the program afresh, and choose P. It requires
changed generation and reset sequence and zero data reads in the new session,
then writes `RESET.OK`. A board power cycle is not the same test.

## Build and checks

Host tools need Python, a native GCC compiler, and cc65's `cl65`, `ca65`, and
`ld65` on PATH. ARM builds use the existing Vitis workspace for the selected
stock platform. Install the Python dependencies with:

```powershell
python -m pip install -r scripts/requirements-net-transport.txt
```

```powershell
python scripts/test_net_transport.py
python scripts/test_smartport_service.py
python scripts/test_smartport_rom.py
python scripts/build_net_transport_client.py
python scripts/build_net_transport_checkpoint.py
```

The host test compiles the actual ARM test source and runs both actual 65C02
client paths against the stock ROM bytes and a controller model. It checks
all reply sizes, repeated reads, caller bounds, reset recovery, timeout, and
a malformed raw reply. The client builder validates cc65's AppleSingle
metadata, extracts its executable data fork at `$2000`, and makes checked
ProDOS scratch images. It never puts an AppleSingle header into a SYS payload.

`prodos_disk.py` is a self-contained copy of libtini's standard-library disk
packager; its MIT license is beside it. ProDOS boot blocks and kernel come
from the user's local `software/ProDOS_2_4_3.po`. Generated images stay in the
ignored build directory.

The firmware builder verifies the existing generated platform against the
selected stock XSA, rebuilds CPU0/CPU1, audits the linked frontend memory, and
packages the exact selected FSBL and bitstream. It saves source and input
hashes. Deployment requires an SD SHA-256 readback before reboot. After the
board work, restore the saved settings and exact stock firmware, then run:

```powershell
python scripts/build_net_transport_checkpoint.py --restore-default-build
```

`run_net_transport_checkpoint.py` checks the live diagnostic code, ONE//e
bridge, clean read count, actual CPU speed, and `c074=0` before sending keys.
It owns COM13 while the run is active and reads the source counter every 15
seconds. Run `collect_net_transport_checkpoint.py` only after the client has
finished: it starts FTP, reads back both owned volumes, checks their structure
and result rows, then stops FTP. FTP stays off during measurements.

## Status

The exploratory TURBO runs with no slot-6 slowdown failed Disk II file write
verification, both with Disk II acceleration enabled and disabled. They used
the stock F1.3.3b3 FPGA and the diagnostic ARM image. A failed run damaged the
owned NETD2 volume header; its readback is preserved in the ignored build
directory. Native-speed storage checks passed on a clean volume. These failed
runs do not count toward acceptance.

Both 10000-read TURBO runs then passed with the existing per-slot slowdown
enabled for slot 6. The live mask was `$028` (slot 6 plus the existing slot-4
Phasor setting), for 256 native ticks, with Disk II acceleration disabled.
`c074=0`: the Apple client contains no clock override. This is a qualified
TURBO result with that compatibility setting.

A later [standalone stock-firmware check](README_DISK2_STOCK_CHECKPOINT.md)
reproduced the unthrottled write failure on exact F1.3.3b3, without the
transport diagnostic ARM code. Native writes and TURBO with 256-tick slot-6
slowdown passed; the acceleration toggle alone did not fix the failure.
The cause is in the ARM sector decoder, not in this transport. Commit
`ca8cf1f` fixes it ([details](README_DISK2_ADDRESS_EPILOGUE_FIX.md)), and
unthrottled TURBO writes passed on the board with that correction. The
transport runs above still used the slot-6 slowdown and were not repeated
without it.

Source, host model, existing SmartPort/ROM, AMP startup, and firmware memory
checks pass. All four stress runs passed, using every requested reply size
from 9 through 512, repeated offsets, EOF cases, and buffer guards. Each run
performed 79 write/close/open/read/verify cycles per scratch volume. SD
readback also confirmed both final 512-byte file patterns and valid volumes.

| Physical run | Stress reads | Largest complete call, VIA ticks | Largest IRQ inner interval, VIA ticks |
| --- | ---: | ---: | ---: |
| Slot-7 ROM, 1 MHz | 10000 | 11833 | 11694 |
| Raw FIFO, 1 MHz | 10000 | 11487 | 11313 |
| Slot-7 ROM, TURBO with slot-6 slowdown | 10000 | 14305 | 14148 |
| Raw FIFO, TURBO with slot-6 slowdown | 10000 | 5225 | 5068 |

The native ROM timing values come from the later 1008-read sweep, two samples
per size. Its original stress run used an earlier timer-wrap check; those
older times are not accepted. All other table entries come from the full
stress runs with wrap detection. These are observed maxima in this setup,
not deadlines. Those runs did not calibrate the raw tick rate. A later
[ARM-referenced VIA calibration and STARFALL test](README_STARFALL_IRQ_CLOCK.md)
supports 0.9825 microseconds per tick on this PAL ONE//e profile. Each
per-size log records both bounds on the IRQ-masked interval as described above.

The ROM speed reversal now has a [source, RTL and physical-board
diagnosis](README_SMARTPORT_ROM_TIMING.md). The clients put their buffer
in HGR page 2 (`$4733` in the archived ROM TURBO client, `$4751` in the
pre-fix v4 client). In TURBO, the ROM copy loop forces a video
mirror flush after each destination byte. A matched stock-firmware test
moved the buffer to ordinary RAM at `$9000`: the 512-byte ROM read fell
from 12758–13331 to 8954–9592 ticks, below its 1 MHz time. The existing
table keeps the original buffer and measurements. The client now reserves
its reply and guards at MAIN `$9000–$9201`, with its program, heap and
software stack bounded below `$9000`. The planned RAM FIFO driver remains
appropriate.

For the warm-reset test, live FIFO status showed 256 unread bytes. A ONE//e
warm Apple reset cleared them, advanced generation 4 to 5 and reset sequence
2 to 3, and cleared the data-read count. ProDOS then loaded the program
afresh; its recovery probe checked the saved descriptor and wrote `RESET.OK`
with the new generation and zero data reads. ARM and FPGA stayed running
through that test.

The exact stock firmware and original settings are restored, the owned test
media is removed, and FTP is off. A final UART check after reboot confirms
the original STARFALL mount and speed settings. The earlier CPU0 HTTPS
evidence remains separate from this transport test. HTTP, JSON, game audio,
and external Apple bus acceptance are not part of these results.
