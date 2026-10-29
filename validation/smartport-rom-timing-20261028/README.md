# SmartPort ROM timing evidence, 2026-10-28

The physical ONE//e board ran stock F1.3.3b3. A standard SmartPort block
read compared ROM and RAM code, with destinations in HGR2 and ordinary
MAIN RAM. All 256 reads passed data, guard and timer-wrap checks.
Moving the ROM destination from `$4751` to `$9000` removed the TURBO
slowdown. A separate RTL test isolated the per-byte video mirror flushes.

* `board-results.json`: speed checks, 32 samples per case, timer ranges
  and means, screens, and final settings/media restoration checks.
* `NATIVE.BIN`, `TURBO.BIN`: original 64-byte Apple result files. Each
  starts with a 16-byte `SCB1` header, followed by four 12-byte records:
  little-endian destination u16, raw-path u8, count u8, minimum u16,
  maximum u16, sum u32. Header byte 4 is the speed label; bytes 5–7
  are failure code, SmartPort return code and timer overflow flag.
* `uart.txt`: captured board commands and responses. FTP stayed off while
  the Apple benchmark ran. The trace contains no settings file contents;
  it uses LF line endings and omits trailing padding. The raw capture
  remains in ignored build output.
* `client-build.json`: Apple program, source, ProDOS and input-image hashes;
  the program's BSS ends below either test buffer.
* `transport-buffer-provenance.json`: buffer locations for the original
  transport clients. Archived v1/v3 sources relink to identical saved
  executable bytes; all versions put the buffer in HGR2.
* `rtl-xsim.log`, `rtl-provenance.json`: isolated copy results and source
  hashes. The bench checks every byte, FIFO pop counts and flush controls.
* `stock-rtl-comparison.json`: production simulation inputs match the
  F1.3.3b3 Git blobs after line-ending normalization.
* `SHA256SUMS.json`: hashes of the evidence and investigation sources.

ARM firmware, FPGA and golden boot were not rebuilt or written. The exact
saved settings were restored, the original STARFALL image reopened, the
owned `/SPCOPY` directory was removed, and FTP was off at completion.
Private settings backups and complete SD images remain in ignored build
output. See [the historical diagnosis](https://github.com/hasseily/appletini-one/blob/97a678cf27d083e30f237435822b7f9bfd9f8174/README_SMARTPORT_ROM_TIMING.md)
for the limits and reproduction commands.
