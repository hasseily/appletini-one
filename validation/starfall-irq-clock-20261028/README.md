# STARFALL IRQ and VIA clock evidence, 2026-10-28

See [the historical clock study](https://github.com/hasseily/appletini-one/blob/97a678cf27d083e30f237435822b7f9bfd9f8174/README_STARFALL_IRQ_CLOCK.md)
for results and limits. The accepted run contains a timer-only 1 MHz calibration and
complete audio workloads at the 26 MHz menu preset and TURBO.

* `board-results.json`: ARM/VIA sample brackets, all phase metrics, live
  profiles and stock/settings restoration checks.
* `phase-results.csv`: flat comparison of all twelve phases, including
  the deliberately failing 25 ms controls and instrumented frame rates.
* `<speed>-<phase>.bin`: original frozen 128-byte records. Key fields:
  elapsed 256-tick units at 26/30 (u32), audio counts at 34/38 (u32),
  rendered frames at 42/46 (u32), calls at 50 (u16), maximum masked units
  at 52 (u16), sticky error at 9 (u8). All integers are little-endian.
* `summary.json`: calibration conversion and acceptance limits.
* `probe.bin`, `client-build.json`: exact runtime probe and its linked
  layout, original callback prefixes and copied game identity.
* `input-provenance.json`: copied SDK inputs matched their build manifest.
* `probe-host-test.json`: actual 65C02 probe/stock-ROM checks, original
  IRQ acknowledgement, payload freeze and tight calibration tests.
* `arm-packet-host-test.json`: timestamp width, frequency and bounds checks.
* `arm-build-report.json`: selected ARM/XSA/FPGA input hashes and memory
  audit. The private timestamp packet was observed on this image.
* `default-build-restored.json`: local default build, with both test flags
  off. This build did not replace the separately selected stock image.
* `uart.txt`: accepted run, normalized to LF without trailing padding.
  The raw trace and private settings backup remain in ignored build output.
* `SHA256SUMS.json`: evidence and source hashes.

The accepted data detects tick loss in the 26 MHz ROM case and both long
controls. No loss appeared in the raw-read cases. All twelve phases kept
active play without a CLOCK GAP flag, and every phase's extended timer
span agreed with ARM time. The instrumented 26 MHz frame rates do not
establish stock 30 fps performance. The user reported audible slowdown
and CLOCK GAP in an earlier, rejected full-game 1 MHz attempt.

The exact stock firmware and settings were restored, the original media
reopened, and FTP was off. The FPGA and Golden design did not change.
