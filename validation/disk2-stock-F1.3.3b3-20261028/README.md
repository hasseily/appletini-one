# Stock F1.3.3b3 Disk II write check — 28 October 2026

The first unthrottled TURBO Disk II write fails verification on the stock
firmware, with Disk II acceleration either on or off. A fresh native-speed
control and both TURBO slot-6 slowdown controls pass all 32 rounds, including
SD readback. This is a physical ONE//e result with ProDOS `.PO` disk images.
External Apple bus pins, WOZ, and DOS 3.3 remain untested.

See [the test and reproduction guide](../../README_DISK2_STOCK_CHECKPOINT.md).

## Matrix

| Case | Actual CPU speed | Disk II acceleration | Live slowdown mask, 256 ticks | Verified Disk II rounds | Result |
| --- | --- | --- | --- | ---: | --- |
| 1 | 1 MHz | On | `$008`, no slot 6 | 32 | Pass |
| 2 | TURBO | On | `$008`, no slot 6 | 0 | First write fails readback |
| 3 | TURBO | Off | `$008`, no slot 6 | 0 | First write fails readback |
| 4 | TURBO | On | `$028`, includes slot 6 | 32 | Pass |
| 5 | TURBO | Off | `$028`, includes slot 6 | 32 | Pass |

Each case first passes eight SmartPort write/close/open/read/verify rounds.
It then attempts 32 equivalent Disk II rounds. Each round writes 512 bytes;
byte `p` in round `r` is `(p * 17 + r * 29 + 0x53) & 255`. Fresh owned images
are uploaded and read back before every case. All cases have `c074=0`; the
client never changes clocks. FTP is off while the client runs. UART screen
polls occur about every seven seconds.

Both failed cases report successful write, close, open, read, and close calls,
then find byte 0 equal to `$00` rather than `$53`. Their SD disk-image hashes
are identical: `c405a29e77886eef5725e3d5af7f5840e4319cc9caed6b8e5bd595e4537505d0`.
Only ProDOS block 2 changes from the pristine image; it becomes all zeros,
destroying the volume header. The scratch data block, 56, remains unchanged.
The volume parser reports `Missing volume header`. This proves persisted
damage to the owned image, beyond the Apple-side readback mismatch.

All three passing Disk II images have hash
`538ab64953e1132555063d3efaae8713ca246e924d31fca983c8440f61d1d453`.
Their volume structures and final scratch patterns validate. The eight-round
SmartPort control files also pass persisted-pattern validation in every case.
`results.json` and `disk-image-observations.json` record these checks. Original
disk images are retained in the ignored `build/disk2-stock-checkpoint` folder.

The Disk II underrun counters are 0, 1, 0, 6, and 2 for cases 1–5. Thus the
write failure occurs even with zero reported underruns, and passing write
verification does not imply zero underruns. These results establish the byte
failure and the compatibility setting; they do not establish its RTL or ARM
cause or full drive timing health.

## Stock identity and client

`prepare.json` records a fresh full readback of `/FIRMWARE.OK` matching the
archived stock package hash:
`cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2`.
The board was already restored to that image before this check. No firmware
or FPGA build, package upload, or flash write took place in this check.

For cases 2–5, the runner also matches eight live ARM words from each of
`disk2_service_poll` at `$00151E60` and `smartport_service_poll` at `$00168120`
to the archived stock ELF. The per-case JSON and UART captures contain these
checks. Each capture also records actual speed, acceleration, slowdown,
screen, SmartPort counters, and Disk II counters.

`client-build.json` records the standalone SYS hash, its source hash, the
local ProDOS input hash, and pristine volume hashes. The client uses only
ProDOS file calls. It performs no private networking calls, timer writes,
audio setup, or clock overrides. The exact same client is used in every case.
`provenance.json` records the source, tool, stock ELF, and stock FPGA hashes.
The FPGA input remains the F1.3.3b3 payload with SHA-256
`94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34`.

## Restore

`restore.json` and `restore.txt` record byte-for-byte SD readback of the
original settings, removal of the two owned test images and their empty
D2CHECK directory, and a normal reboot. UART then confirms stock F1.3.3b3,
Golden B2.0.6, the original STARFALL SP1 mount open, original Disk II media and
disabled slot-6 setting, TURBO, live slowdown mask `$008`, acceleration on,
and FTP off. Serial connections have closed. No user disk image was written.
