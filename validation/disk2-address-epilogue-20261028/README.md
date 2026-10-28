# Address-epilogue diagnosis and correction — 28 October 2026

The cause and implementation are in the
[fix guide](../../README_DISK2_ADDRESS_EPILOGUE_FIX.md). This record separates
the stock raw capture, actual-C decoder regression, source ARM build, and
isolated hardware experiment.

## Proof chain

1. The [earlier stock sector-image test](../disk2-stock-F1.3.3b3-20261028/README.md)
   failed on its first unthrottled TURBO write, with Disk II acceleration on
   or off. SD readback showed block 2 zeroed and no payload update at block 56.
2. The same stock firmware and Apple program passed 32 rounds on a raw NIB
   image. Raw NIB writeback bypasses ARM sector conversion. Captured addresses
   on tracks 0 and 7 have `DE AA FF` tails, with valid addresses and checksums.
   Data fields retain valid GCR, checksums, and `DE AA EB` tails.
3. The stock converter's third address-epilogue check rejects these sectors;
   its initially zeroed output loses their data. Removing only that check
   recovers the entire captured disk exactly, with hash
   `538ab64953e1132555063d3efaae8713ca246e924d31fca983c8440f61d1d453`.
4. A native test compiles the actual C decoder, reproduces that loss with
   the old rule, and checks recovery with the correction. It also checks
   all 256 third-byte values in DOS and ProDOS orders and eight invalid-field
   cases. The existing 20 standard-disk tests pass.
5. An otherwise byte-identical stock ARM ELF with one instruction changed
   passes the previously failing physical tests. This isolates the decoder
   requirement without changing the CPU, FPGA, write timing, or other ARM code.

`capture-analysis.json` records the four altered address fields and hashes.
`raw-nib-case-2.json`/`.txt` hold the actual stock run and its PASS screen.
The original collector used the strict converter and failed when it found the
converted volume header missing; that saved collector error is preserved.
It is separate from the 32 passing Apple-side rounds. The captured track-7
fixture and expected decoded hash are under `scripts/fixtures/`.

## Hardware comparison

| Payload | CPU speed | Acceleration | Slot-6 slowdown | Verified Disk II rounds |
| --- | --- | --- | --- | ---: |
| Exact stock F1.3.3b3 | TURBO | On | Off | 0; first write failed |
| Exact stock F1.3.3b3 | TURBO | Off | Off | 0; first write failed |
| Stock plus isolated decoder correction | 1 MHz | On | Off | 32 |
| Stock plus isolated decoder correction | TURBO | On | Off | 32 |
| Stock plus isolated decoder correction | TURBO | Off | Off | 32 |

Each fixed case also passes eight SmartPort control rounds. All fixed images
have valid volume structures and the final expected payload; they are byte
identical to the successful native stock image. `fixed-case-1/2/3.json`, their
UART logs, and binary result files record the profiles and results. These
are physical ONE//e tests; external Apple bus and WOZ are not covered.

## Exact experiment payload

`patch-provenance.json` records the stock and changed ELF hashes and the
single instruction at `$0015124C`: `E24220EB` becomes `E3A02000`. Every ELF
byte outside that four-byte instruction is identical to stock, including
the embedded CPU1 image. The source guide explains the disassembly and the
checks that remain. `stock-disk2-disassembly.txt` records the original code.

FSBL and FPGA are the exact stock inputs; no FPGA build occurred. Golden
was never written. The package hash is
`e654ac5015e331dd6381ded0c052ddc17a45138173125bd9e279e55e37d7b8ca`.
The normal SD updater received a full matching SHA-256 readback before boot.
Each fixed case verifies the live corrected instruction and stock service
code fingerprints before starting. The package still reports F1.3.3b3, so
the hash and live instruction identify this experiment.

`source-arm-build.json` and its log record a separate normal ARM compilation
of the source fix, with HTTP and transport diagnostic features off and the
linked 64 KiB stack / 64 MiB heap audit passing. That source-built ELF was
not the isolated hardware payload. A normal release should compile the
source correction rather than use this diagnostic instruction patch.

## Restoration

The raw-capture experiment restored its original settings and deleted its
owned NIB media before the corrected-image experiment began. The corrected
image experiment restores exact stock F1.3.3b3 through the normal SD updater,
then reinstates the byte-for-byte saved settings, removes its owned disk
images, and reboots to reopen the original mounts. Restore JSON/UART files
record the final state. No user disk image was written.

The reason the splice alignment differs is consistent with faster execution
before Q7 forces native write pacing. Exact read-to-write cycle timing was
not captured. The decoder failure and correction are established independently
by the raw capture, full-disk recovery, and one-instruction hardware experiment.
