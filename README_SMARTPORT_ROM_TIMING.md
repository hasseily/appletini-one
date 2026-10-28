# SmartPort ROM timing diagnosis

The ROM slowdown in TURBO comes from the transfer buffer's address. The
earlier transport diagnostics put `nt_buffer` inside HGR page 2: `$4733`
in the archived ROM TURBO client and `$4751` in the pre-fix v4 client.
TURBO defers video writes, then flushes them before a card ROM or device
access. The ROM copy loop fetches instructions from `$C9C5`, so each byte
stored in the buffer causes another flush. Those flushes cost more than
TURBO saves on this ROM loop.

Moving the buffer to ordinary MAIN RAM at `$9000` removes the reversal on
the physical board. No ARM or FPGA change is needed for that result.

## Physical comparison

On 2026-10-28, the stock F1.3.3b3 firmware ran in PAL ONE//e mode. The test
used standard SmartPort READ_BLOCK, unit 1, block 1599, on an owned ProDOS
image. It read the same 512-byte `$A5` block 32 times for each row and speed.
FTP stayed off during the measurements. No network service or diagnostic
ARM code ran. The raw path used the ROM's five-instruction page-copy loop
in main RAM; its request setup also ran in RAM.

| Code path | Destination | 1 MHz, VIA ticks | TURBO, VIA ticks |
| --- | --- | ---: | ---: |
| Slot-7 ROM | MAIN `$4751`, HGR2 | 10576–11283 | 12758–13331 |
| RAM FIFO loop | MAIN `$4751`, HGR2 | 10204–10888 | 4444–5152 |
| Slot-7 ROM | MAIN `$9000`, ordinary RAM | 10583–11297 | 8954–9592 |
| RAM FIFO loop | MAIN `$9000`, ordinary RAM | 10204–10831 | 1973–2669 |

All 256 reads passed: return code zero, all 512 bytes correct, both guard
bytes intact, and no timer wrap. The native ROM ranges overlap at the two
addresses. Both TURBO paths improve when the destination leaves video RAM.
The ROM path becomes faster than its native result.

These are complete measured SmartPort calls, including ARM service wait
and timer access overhead. VIA1 T1 supplied the ticks. The live Phasor
slowdown mask was `$008`, 256 native ticks, and `c074=0` at both speeds.
The tick rate was not independently calibrated; the address comparison
uses the same clock and does not need a conversion to microseconds.
The earlier private STATUS diagnostic used a different ROM wrapper, so
its absolute times should not equal these READ_BLOCK times.

The benchmark restored the exact saved settings, original STARFALL mount,
and TURBO speed, removed its owned SD test directory, and left FTP off.
It did not rebuild or write ARM firmware, the FPGA, or golden boot.
The SD updater receipt still matches the stock F1.3.3b3 SHA-256:
`cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2`.
Live SmartPort/Disk II function entry words and the stock decoder
instruction also matched before the run.

## Source and RTL evidence

* The pre-fix v4 `client.lbl` places `_nt_buffer` at `$4751`.
  Rebuilding the archived v1 and v3 sources reproduced their saved program
  bytes exactly and recovered buffer addresses `$4945` and `$4733`.
  All three lie in HGR2. `transport-buffer-provenance.json` records the
  program hashes and recovered labels. The matched physical comparison
  above uses that pre-fix client's `$4751` address.
* `hdl/apple/smartport_a2retronet_style.asm`, `RDPAGE`, copies each byte with
  `LDA $CFF0; STA $CFF2; STA ($44),Y; INY; BNE RDPAGE`. The current C800
  image contains those bytes at `$C9C5`.
* `hdl/apple/vtw_core_top.sv`, `xl_is_posted`, routes writes in video address
  ranges to the mirror even when that graphics page is off screen.
  `video_selected` enables the deferred mirror in TURBO.
* `video_exposure_access` treats card ROM and DEVSEL accesses as possible
  device/DMA entry. `video_full_flush` retires pending video bytes before
  allowing those accesses. A ROM instruction fetch after each destination
  store therefore starts another flush.
* Cxxx addresses also miss the TURBO RAM cache. This explains why RAM code
  still wins after moving the buffer. It does not by itself explain the
  reversal: the mirror overhead supplies that extra cost.
* ONE//e keeps the private SmartPort shortcut off. These requests use the
  normal virtual slot bus, including ROM fetches and FIFO accesses.

`tb_smartport_copy_timing.sv` runs the production core, mirror, virtual bus
and SmartPort card, with a preloaded 512-byte FIFO. It executes the actual
ROM loop and a byte-identical copy in RAM. This removes ARM dispatch and
storage wait from the comparison. The tested production modules and ROM
images match the F1.3.3b3 tag.

| RTL copy case | Destination | Native tick equivalents | Mirror flushes |
| --- | --- | ---: | ---: |
| ROM, 1 MHz | `$4751` | 9763.00 | 0 |
| ROM, TURBO | `$4751` | 12013.71 | 512 |
| ROM, TURBO | `$9000` | 8197.71 | 0 |
| ROM, TURBO, mirror record path disabled | `$4751` | 8200.71 | 0 |
| ROM, 1 MHz | `$9000` | 9763.00 | 0 |
| RAM loop, TURBO | `$4751` | 4025.71 | 512 |
| RAM loop, TURBO | `$9000` | 1537.71 | 0 |

The simulation measures only the two page copies and a final FIFO-control
read that drains the last pending byte. It divides fabric clocks by 131
for the PAL native tick equivalent. It does not measure the full board
call. Disabling the record path is a simulation control, not a proposed
firmware setting. Every case checks all destination bytes and exactly 512
FIFO pops. The test also asserts the ROM speed reversal, its removal at
`$9000`, and the flush counts.

## Buffer fix

The transport client now puts its reply and both guard bytes in a dedicated
`NETBUF` segment at MAIN `$9000–$9201`. Its project-local `apple2enh.cfg`
sets `__HIMEM__=$9000`, bounds the ordinary BSS/heap below `$8800`, and
reserves `$8800–$8FFF` for the 2 KiB software stack. The program and heap
cannot allocate the reply range. The assembly model uses the same buffer
reservation.

The builder checks the linked labels before it makes disk images. The
layout test also links the real sources with oversized code, BSS and reply
segments; each must fail. This guards against later growth reaching the
reply or its bounds checks. Every call fills all 514 reserved bytes before
using them; the separate segment does not need the runtime's ordinary BSS
clear.

```powershell
python scripts/test_net_transport_buffer.py
```

The buffer fix changes the Apple diagnostic and its linker layout. It
does not change ARM service code, ROM bytes or FPGA logic. The separate
full host suite also passed both 65C02 paths for every reply size, repeated
reads, guard checks, reset, timeout and timer wrap.

The fixed client then passed two physical 1008-read ROM sweeps, one per
speed, with two reads of every size from 9 through 512. Both scratch
volumes passed the write/close/open/read/verify checks during each sweep.
No timer wrapped and all reply/guard checks passed.

| Fixed client, ROM path | 512-byte complete call, VIA ticks |
| --- | ---: |
| 1 MHz | 10717–10719 |
| TURBO | 9443–9681 |

These sweeps used the existing diagnostic ARM package, SHA-256
`f3e5092d6834452bd3d6e4a0e79736bb7d7028b3c390184da3832ba34642d522`,
with the unchanged F1.3.3b3 FPGA. Disk II used its earlier qualified
256-tick slot-6 slowdown: live mask `$028`, acceleration off. The buffer
was the only client behavior change. The original stock firmware and
saved settings were restored afterward. This private STATUS sweep has
different command overhead from the stock READ_BLOCK comparison above.

The fixed-client evidence is in
`validation/net-transport-buffer-fix-20261028/`. The firmware updater
needed more time than the first UART check allowed; a second short
native sweep hit the harness deadline. Neither incomplete attempt counts.
The accepted run checked UART readiness and completed both sweeps before
collecting and restoring media.

Keep the planned networking copy loop in RAM. It already avoids the large
cost of fetching each loop instruction from card ROM. Where possible,
receive into ordinary RAM and let the application update video separately.
Applications may still choose a video destination and pay the mirror cost.

Do not remove the flush rule to improve this benchmark. It protects video
and memory coherence when a card starts DMA. A general ROM-speed change
would need a separate design, such as a reserved RAM copy routine or a
more precise device handoff rule, plus its own compatibility checks.
The result here does not resolve the separate audio/IRQ latency gate.

## Reproduce

```powershell
python scripts/test_smartport_copy_timing.py
python scripts/build_smartport_copy_timing.py
python scripts/run_smartport_copy_timing.py
```

The first command needs Vivado's simulation tools. The last needs COM13
at 921600 baud and the board at `10.17.1.18`, running stock F1.3.3b3 in
ONE//e mode. It temporarily selects the owned `/SPCOPY/TEST.PO` image,
then restores the saved settings in `finally`. It refuses an existing
restore copy or SD test directory. Outputs and the private settings backup
stay in ignored `build/smartport-copy-board/`. Copy that backup elsewhere
before preparing another run; do not overwrite the only restore copy.

Saved public evidence is in
`validation/smartport-rom-timing-20261028/`: board results, binary result
records, UART trace, client build hashes, RTL results and source hashes.
