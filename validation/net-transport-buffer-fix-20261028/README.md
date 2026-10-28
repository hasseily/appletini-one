# Transport reply buffer fix, 2026-10-28

The fixed Apple diagnostic reserves its 512-byte reply and two guard bytes
at MAIN `$9000–$9201`. Code, ordinary BSS, heap and software stack stay
below that range. No ARM source, ROM bytes or FPGA logic changed for this
fix.

The final physical ONE//e run passed 1008 ROM reads at each CPU speed:
two reads of each size from 9 through 512, with correct frame/data/guards
and no timer wrap. Both SmartPort and Disk II scratch files passed their
write/close/open/read/verify cycles. SD readback confirmed their final
patterns for iteration 895.

| 512-byte ROM call | VIA ticks |
| --- | ---: |
| 1 MHz | 10717–10719 |
| TURBO | 9443–9681 |

The existing diagnostic ARM image ran with the F1.3.3b3 FPGA. Its SHA-256
was `f3e5092d6834452bd3d6e4a0e79736bb7d7028b3c390184da3832ba34642d522`.
The live slowdown mask was `$028`, 256 native ticks; Disk II acceleration
was off and `c074=0`. These are complete private STATUS calls, not the
stock READ_BLOCK timing experiment. VIA ticks remain uncalibrated against
an independent clock. This run does not test audio deadlines.

* `board-results.json`: accepted sweeps, live profiles, rendered PASS
  screens, final storage pattern checks and stock restoration results.
* `1mhz.BIN`, `turbo.BIN`: original 9104-byte result files. Each contains
  the 32-byte `NTM1` header and 504 records of 18 bytes: count, call minimum,
  call maximum, IRQ inner minimum, IRQ inner maximum (all little-endian
  u16), followed by call total and IRQ inner total (little-endian u32).
* `1mhz.csv`, `turbo.csv`: those records decoded for reply sizes 9–512.
* `client-build-report.json`, `client.lbl`: linked layout, source and
  program hashes. The reply starts at `$9000`; ordinary BSS ends at
  `$6D45`; heap ends below `$8800`; stack occupies `$8800–$8FFF`.
* `buffer-layout-test.json`: real-client and model reservations, with
  oversized CODE, BSS and NETBUF link attempts all rejected.
* `host-test-summary.json`: separate host checks of actual ARM test source
  and the ROM/raw 65C02 paths. These are not board timing measurements.
* `uart.txt`: captured final run, including live read counts and restore
  checks, with LF line endings and trailing padding removed. Raw captures
  and earlier incomplete attempts remain in ignored build output.
* `SHA256SUMS.json`: evidence and source hashes.

The first UART check ran before the firmware update finished booting. A
later 75-second native sweep deadline also ended too soon. Neither counts
toward acceptance. The final run waited for UART readiness and allowed
the native sweep to finish.

At completion, the runner restored exact stock F1.3.3b3, verified its SD
receipt SHA-256, restored the saved settings by byte-for-byte readback,
removed only `/NETBFIX`, rebooted, and confirmed the original STARFALL
mount open and FTP off. Golden boot stayed unchanged. Private settings
backups and full test disks remain in ignored build output.
