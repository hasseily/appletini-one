# Boot and Firmware Update

Appletini One uses two golden-boot slots and one firmware slot in its
128 Mbit / 16 MB Quad SPI flash.

## Flash Layout

| Region | Offset | Size | Contents |
| --- | ---: | ---: | --- |
| Golden A | `0x00000000` | `0x00100000` | Primary `BOOT.BIN` |
| Golden B | `0x00100000` | `0x00100000` | Trial and backup `BOOT.BIN` |
| Firmware | `0x00200000` | `0x00DF0000` | `FIRMWARE.BIN`: FSBL, bitstream, and apps |
| Metadata | `0x00FF0000` | `0x00010000` | Last checked firmware record |

The image build scripts add a 32-byte manifest to each file. It records the
image role, recovery flag, payload size, and payload CRC32. Use `BOOT.BIN` and
`FIRMWARE.BIN` from the same release.

## Firmware Update

1. Put `FIRMWARE.BIN` in the root of the card's SD volume.
2. Reboot the card.
3. Let golden boot write and check the firmware slot.
4. Wait for the updater to rename the file to `FIRMWARE.OK` and start the
   frontend.

The firmware update path does not write either golden-boot slot. It checks the
whole flash copy before it marks the firmware as valid.

## Golden Boot Update

This is the supported field procedure for frontend Firmware F1.0.1 and later.
It works even when the golden boot already on the card predates B1.2.0.

1. Install and boot the matching `FIRMWARE.BIN`. Confirm that the normal
   frontend is F1.0.1 or later.
2. Put the matching `BOOT.BIN` in the root of the card's SD volume.
3. Stop USB SD sharing and FTP SD sharing if either is active.
4. Connect USB0 and open its control serial port at `921600` baud, 8 data bits,
   no parity, 1 stop bit, and no flow control.
5. Enter `:selfupdate` and press Enter.
6. Keep the card powered until all automatic reboots finish and the normal
   frontend starts again.

The frontend checks `BOOT.BIN` and the installed recovery firmware before it
starts an update. It writes and checks Golden B, boots that copy, then repairs
and checks Golden A. Do not remove power before the normal frontend returns.

If a check fails before the first write, the command stops without changing
either golden-boot slot and reports the cause on USB0.

## Release Check

1. Build matched `BOOT.BIN` and `FIRMWARE.BIN` files.
2. Install `FIRMWARE.BIN` and boot its F1.0.1-or-later frontend.
3. Follow the Golden Boot Update steps above.
4. Confirm that the log shows the Golden B trial, the Golden A repair and
   check, and the final boot through Golden A.
5. Confirm that the normal frontend starts and reports the new golden version.
