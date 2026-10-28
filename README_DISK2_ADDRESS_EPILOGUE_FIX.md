# Disk II TURBO write failure: address-epilogue fix

The failure came from the ARM sector-image decoder in stock F1.3.3b3. It
rejected an address field unless all three epilogue bytes were `DE AA EB`. A
normal data-write splice can overwrite the third byte with `$FF` sync, leaving
`DE AA FF`. The address, data, and checksums stay valid, but the decoder
dropped the sector. Its zero-filled output then replaced that sector in the
`.PO` image.

The correction is in `ps_sources/frontend/disk2_service.c`: check `DE AA`
and leave the third address-epilogue byte unconstrained. Keep the 4-and-4
encoding, track, sector number, address checksum, data GCR, data checksum,
and data epilogue checks. This needs an ARM firmware update, with no FPGA
change.

## Evidence for the cause

The [stock test](README_DISK2_STOCK_CHECKPOINT.md) failed on the first
unthrottled TURBO `.PO` write, with acceleration either on or off. To inspect
the bytes before ARM sector conversion, the same program ran on an owned raw
`.NIB` image under exact stock F1.3.3b3. All 32 Apple-side rounds passed.
NIB writeback preserves the stream without running the sector converter.

The capture shows four changed address tails:

| Track | Physical sectors | Address ending | Data ending |
| --- | --- | --- | --- |
| 0 | 8, 10 | `DE AA FF` | `DE AA EB` |
| 7 | 0, 2 | `DE AA FF` | `DE AA EB` |

All four addresses have valid 4-and-4 bytes, track/sector values, and address
checksums. Their data decodes with valid checksums. The strict stock converter
rejects these addresses. Decoding the entire capture with only the third-byte
requirement removed recovers the successful native-speed disk **byte for
byte**, SHA-256
`538ab64953e1132555063d3efaae8713ca246e924d31fca983c8440f61d1d453`.

This explains both symptoms: lost data at block 56 and zeroed directory/header
data at block 2. It also explains why the raw NIB test passes while sector-image
writeback fails. The initial raw-capture collector used the strict converter
and could not validate the converted volume; its saved error belongs to that
conversion check, not to the Apple program, whose screen and result report
32 passing rounds.

The address field's third epilogue byte is not required by the reference
[AppleWin sector converter](https://github.com/AppleWin/AppleWin/blob/master/source/DiskImageHelper.cpp#L421-L494).
The Appletini correction keeps its existing checksum and range checks.

## Why TURBO exposes it

The captured change is the start of a sync-write splice, not altered address
contents or bad GCR data. ProDOS accepts the address's `DE AA` ending; sync
bytes written before the following data field may cover the nominal `$EB`.
TURBO changes the alignment of that transition. The CPU may run fast before
Q7 write mode becomes active; Q7 itself forces native pacing in the existing
RTL. Faster read-to-write turnaround is the timing explanation consistent
with that source and the capture. Its exact cycle count has not been traced.

The zeroing failure does not depend on resolving that cycle count: changing
only the ARM third-byte test restores the complete captured disk and the
physical sector-image writes. The slot-6 slowdown setting avoids the splice
alignment that exposes the strict test, which is why it worked as a workaround.

## Checks and hardware proof

`scripts/test_disk2_address_splice.py` compiles the actual C decoder. It uses
the captured track in `scripts/fixtures/disk2_turbo_address_splice.nib`, proves
that the stock rule loses its data, and checks the corrected decoded hash.
It also tests all 256 possible third bytes in both DOS and ProDOS sector
orders and checks eight invalid-field cases. The existing 20 standard-disk
tests pass. The normal ARM source build and linked memory audit pass with
diagnostic features off.

To isolate the correction on hardware, the experiment changes exactly one
instruction in the archived stock ELF:

```text
$0015124C: E24220EB  sub r2,r2,#235
        -> E3A02000  mov r2,#0
```

The following CLZ/LSR sequence then treats the third byte as valid. The
subsequent 4-and-4 validity result still gates acceptance. All other ELF bytes,
the embedded CPU1 image, FSBL, and FPGA payload are unchanged. The data-epilogue
check at `$00150B00` remains in place. This is a diagnostic experiment, not a
release method; production firmware must compile the source correction.

The experiment package SHA-256 is
`e654ac5015e331dd6381ded0c052ddc17a45138173125bd9e279e55e37d7b8ca`.
It retains the F1.3.3b3 version string; its hash and live corrected instruction
identify it. The normal source-built ELF is recorded separately and was not
the hardware-test payload.

With this isolated correction, the native control and both formerly failing
unthrottled TURBO cases pass 32/32 Disk II rounds each, with acceleration on
and off in TURBO. All SmartPort control rounds pass. All three persisted disk
images exactly match the native stock result. Restoration is recorded in the
[validation record](validation/disk2-address-epilogue-20261028/README.md).

These are physical ONE//e tests. External Apple bus, physical DOS-order media,
and WOZ acceptance remain separate.

## Missing sectors keep their stored bytes

The zeroed block 2 came from a second rule: the decoder started each
writeback from a zero track, as AppleWin does, so every sector that it could
not decode became 256 zero bytes. A DSK, DO, or PO file cannot hold a missing
sector, because the next load builds all 16 sectors again, so those zeros
only destroyed data. Commit `5c97255` replaces that rule:

- Writeback starts from the stored track in the RAM image copy. Only sectors
  with a valid address and data field replace their stored bytes.
- After a normal DOS or ProDOS write, all 16 sectors decode. If one does not,
  the firmware still saves the valid sectors, then write-protects the drive
  until the next mount. The PL refuses writes while the drive is read-only,
  so the Apple gets a write-protect error instead of silent data loss.
- UART prints `Disk II write disabled` with the drive, track, and missing
  sector mask. `disk2` status shows the last fault as
  `sectorfault=t<track>:$<mask>`. A new mount clears it.

Commit `d9883ee` lets `cached_nib_address_valid` accept a `DE AA` address
ending, so a spliced third byte does not hide a valid 16-sector address when
the firmware checks a NIB image for 13-sector format.

`test_disk2_address_splice.py` checks the merge on the actual C decoder:
a full track replaces all 16 sectors, a sector with a bad data checksum keeps
its stored bytes, and a track with no valid address field stays unchanged.
`test_disk2_standard.py` checks the write-protect path and the status field.
This change has not run on the board yet.
