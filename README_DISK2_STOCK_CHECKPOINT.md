# Disk II writes on stock F1.3.3b3 — 28 October 2026

Unthrottled TURBO Disk II writes fail on the exact stock F1.3.3b3 firmware
in ONE//e mode. The failure needs no HTTP/S or transport diagnostic ARM code.
Native writes pass. The built-in 256-tick slot-6 slowdown also passes in TURBO.
This check does not cover an external Apple bus, WOZ, or DOS 3.3 disks.

The [validation record](validation/disk2-stock-F1.3.3b3-20261028/README.md)
contains the live profiles, UART screens, result bytes, disk readback hashes,
and stock ARM code checks. The firmware and FPGA were not rebuilt or written.

## Results

Every case starts with fresh owned ProDOS images. The Apple program writes,
closes, opens, reads, and checks a 512-byte file with a new byte pattern each
round. It first checks SmartPort for eight rounds, then Disk II for 32 rounds.
The host checks the persisted final file bytes from SD after each completed
case. FTP stays off while the Apple program runs.

| CPU speed | Disk II acceleration | Slot-6 slowdown | Disk II result |
| --- | --- | --- | --- |
| 1 MHz | On | Off | 32/32 pass; SD readback matches |
| TURBO | On | Off | First write fails verification |
| TURBO | Off | Off | First write fails verification |
| TURBO | On | 256 ticks | 32/32 pass; SD readback matches |
| TURBO | Off | 256 ticks | 32/32 pass; SD readback matches |

All SmartPort control rounds pass. Both failed Disk II cases report successful
file calls, then read `$00` instead of the expected `$53` at byte 0. SD
readback shows the same damaged image in both cases: ProDOS block 2, which
holds the volume header, has become all zeros. The scratch data at block 56
has not changed. The volume parser reports `Missing volume header`.

All three passing disk images have the same SD readback hash. This establishes a
stock-firmware write failure and a tested compatibility setting; it does not
identify the underlying RTL or ARM fault. No fix is part of this check.
The reported Disk II underrun counts for cases 1–5 are 0, 1, 0, 6, and 2;
those counters alone do not identify the write failure in these runs.

For the slowdown profile, the saved mask is `0x20`, which selects slot 6.
The live mask is `$028` because the existing Phasor slot-4 setting adds `$008`.
The duration is 256 native ticks. All cases have `c074=0`; the Apple program
never changes speed. The host sets the requested speed at the program menu.

## Reproduce

Use the physical board in ONE//e mode, COM13 at 921600 baud, Ethernet at
`10.17.1.18`, and the exact stock firmware. Install Python dependencies from
`scripts/requirements-net-transport.txt`, with cc65's `cl65` on PATH.

```powershell
python scripts/build_disk2_write_checkpoint.py
python scripts/run_disk2_write_checkpoint.py prepare
python scripts/run_disk2_write_checkpoint.py case --case 1
python scripts/run_disk2_write_checkpoint.py case --case 2
python scripts/run_disk2_write_checkpoint.py case --case 3
python scripts/run_disk2_write_checkpoint.py case --case 4
python scripts/run_disk2_write_checkpoint.py case --case 5
python scripts/run_disk2_write_checkpoint.py restore
```

Case 5 checks TURBO with slot-6 slowdown and Disk II acceleration off. The
runner saves the original settings once and refuses to overwrite that restore
copy. It uses only `/D2CHECK/HOST.PO` and `/D2CHECK/DISK.PO`, and refuses an
existing D2CHECK directory. Preparation checks the stock FIRMWARE.OK hash.
Live code checks against the archived stock ARM ELF are recorded for cases
2 through 5; the runner now makes those checks in every case.

The source is `software/disk2_write_checkpoint/main.c`. It uses only ordinary
ProDOS file calls, with no private selectors, VIA timer changes, audio setup,
or clock overrides. `build_disk2_write_checkpoint.py` checks cc65's AppleSingle
metadata and extracts a SYS payload at `$2000`, then builds and checks both
owned volumes. Generated images and the saved private settings stay under
the ignored `build/disk2-stock-checkpoint` directory.

Restoration reinstates the saved settings with exact SD readback, removes
only the owned test images and empty directory, and reboots to reopen the
original media after FTP closes. The runner then records the firmware,
speed, original disk mounts, and FTP-off status over UART.

Restoration completed: the original STARFALL mount, TURBO profile, and Disk II
settings are back, the scratch images are removed, and FTP is off. Golden
remains B2.0.6. No user disk image was written.

The [later root-cause check and ARM-only correction](README_DISK2_ADDRESS_EPILOGUE_FIX.md)
identify the strict third address-epilogue byte test as the cause. The raw GCR
write is valid; sector conversion rejects its `DE AA FF` address ending.
Commit `ca8cf1f` accepts that ending. Commit `5c97255` keeps the stored bytes
of any sector that does not decode, so writeback no longer zeros it.
