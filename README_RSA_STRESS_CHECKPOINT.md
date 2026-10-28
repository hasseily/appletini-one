# RSA, video, USB and disk-write stress

On 28 October 2026, the board passed two complete load runs with RSA HTTPS,
moving low-resolution Apple video, USB mouse/controller input and actual
Apple-side ProDOS writes through SmartPort and Disk II. The user accepts
stutter under heavy TLS load. Data integrity and recovery are the release
criteria.

| Result | 33 MHz | TURBO |
| --- | ---: | ---: |
| Verified RSA handshakes | 65 | 65 |
| SmartPort write/readback cycles | 86 | 94 |
| Disk II write/readback cycles | 86 | 94 |
| Longest TLS work step | 27.838 ms | 27.858 ms |
| Longest USB1 service gap under combined load | 70.056 ms | 40.396 ms |
| USB1 recovery-phase maximum gap | 16.697 ms | 20.946 ms |
| TLS pool peak / reservation | 96128 / 524288 bytes | 96128 / 524288 bytes |
| Capture gaps, oversized records, resyncs | 0 | 0 |
| Reported HID and Xbox errors | 0 | 0 |
| Video RAM mismatches | 0 | 0 |

Each one-minute TLS phase alternates RSA 2048- and 4096-bit public sites.
Every request completes a handshake, passes certificate checks, returns
HTTP 200 and matches a separately downloaded SHA-256. Three-second baseline
and recovery phases bracket each load phase. The runtime keeps all 26 public
roots; it does not install the controlled test CA or replace seed state.

The Apple program writes 512-byte patterns into each of 64 blocks of a
32 KiB file. Its coprime block stride forces track changes. It closes each
write, reopens the file and checks the readback. After each run, the host
checks every saved scratch block against the program's recorded write tag,
validates the ProDOS volume, and compares every other file byte for byte
with its original. A separate guard file helps detect writes outside the
target. The final frozen moving-video pattern also matches a host check.

The capture stream remains intact while CPU0 pauses for RSA work. A long
CPU0 service gap can still cause visible or input stutter. USB evidence is
the count of received reports and reported transfer errors; it does not
prove every physical movement against a known input sequence. Physical
HDMI frame loss and USB0 transfer throughput are outside this run.

The test uses a normal HTTP engine with test-only CPU1 health and service
counters, the unchanged F1.3.3b3 FPGA payload, and isolated scratch images.
It restores the prior firmware and saved profile afterward. The final
normal F1.3.3 image then passes a separate boot, version, updater-receipt,
public HTTPS and STARFALL-title check. The user also reports a full successful
STARFALL gameplay and leaderboard test on the same application code.

The first load attempt stopped before saved-disk verification because a UART
command echo preceded a result marker. The parser regression now covers
that case. Both accepted runs start from fresh scratch images.

Evidence: `validation/rsa-stress-20261028/`. Build with
`scripts/build_rsa_stress.py` and the test-only HTTP service build. The
`scripts/run_rsa_stress.py` stage/run/restore actions preserve a journal and
use only the owned `/RSASTRESS` volumes. Do not run them during user play.
