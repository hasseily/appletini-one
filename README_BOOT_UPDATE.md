# Boot and firmware updates

Golden boot validates and installs firmware from the physical SD card or
the USB-to-UART console. Appletini and Multitini occupy separate flash slots.
Golden itself has a primary slot and a trial slot for updates.

A normal firmware update uses `FIRMWARE.BIN`. Update Golden only when the
release or migration instructions provide a new `BOOT.BIN`; a firmware-only
release can keep the installed Golden version.

## GitHub updates from About

The configuration menu checks the latest published stable release from
[hasseily/appletini-one](https://github.com/hasseily/appletini-one/releases/latest).
Drafts, prereleases and version tags with prerelease suffixes are excluded.
The automatic check runs once per boot when the initial boot menu or an
isolated ONE//e menu is open and Ethernet and SD storage are available.
Use **Check for updates** in About to retry a failed check.

An **Update available** badge appears beside About when either image is newer.
About compares firmware and boot versions separately. It reads the boot version
from the verified `BOOT.BIN` image, so including the same boot image in another
firmware release does not trigger a boot update.

Select **Update firmware...** or **Update boot...**, then confirm **Download,
install and restart**. The initial choice is Cancel. The card downloads the
release asset directly over HTTPS to the physical SD card, checks its exact
size, GitHub SHA-256 digest, image manifest, product and linked version, then
uses the normal firmware or Golden updater. Escape cancels the download.
Keep the card powered once installation starts.

The transfer uses a 128 KiB HTTP ring and a separate 128 KiB SD write buffer
in the card's ARM RAM. Full SD writes use 128 KiB blocks; only the final write
can be smaller. The progress view shows elapsed time and average KiB/s.
During a transfer, progress redraws run at 10 Hz while input and error changes
still refresh at once. The Apple II's RAM is not used for these buffers.

The release assets must be named exactly `FIRMWARE.BIN` and `BOOT.BIN`, under a
stable firmware tag such as `F1.3.6`. Only newer versions are offered. Missing
assets do not enable an update button. No update is installed merely because
a check found one.

Downloads use `.part` files until verification and close succeed. Existing
SD-root `.BIN` or `.part` files are kept; move or remove them before retrying
an online download. The local **Update Golden Boot...** action remains
available for a manually copied `BOOT.BIN`.

The installed Golden can stay at B2.0.6. The online updater needs working
Ethernet, a writable FAT SD card, and the correct RTC time for HTTPS. SD
sharing, printing, screenshots, or a firmware handoff pause or cancel it.
It does not take Ethernet from a running physical Apple session.

About keeps the last update result in the page, including the failed step and
the underlying error. HTTP status, certificate verification, image validation,
and SD errors appear when available. These details remain visible if another
menu action changes the bottom status bar. If cleanup also fails, the first
failure is kept and the cleanup error is shown separately.

To read all About credits, press Down past the last update button. Up and Down
scroll the credits; Page Up and Page Down move by a page. Enter, Back or Esc
returns to the update buttons. The gamepad's direction pad and default A/B
buttons use the same controls. Long credit lines wrap to the display width.

## Flash layout

The production card has 16 MiB of QSPI flash. The current layout is defined
in [multiboot.h](ps_sources/lib/multiboot.h).

| Region | Offset | Capacity |
| --- | ---: | ---: |
| Primary Golden | `0x00000000` | `0x00100000` (1 MiB) |
| Trial Golden | `0x00100000` | `0x00100000` (1 MiB) |
| Appletini firmware | `0x00200000` | `0x00600000` (6 MiB) |
| Multitini firmware | `0x00800000` | `0x007E0000` (7.875 MiB) |
| Metadata copy A | `0x00FE0000` | `0x00010000` (64 KiB) |
| Metadata copy B | `0x00FF0000` | `0x00010000` (64 KiB) |

Current firmware images have a 32-byte manifest containing product identity,
size and CRCs, plus identity markers in the executable. Golden validates
these before installation and selects the slot from the image's contents.
Renaming a file does not change its product. See [multiboot](README_MULTIBOOT.md)
for migration from older layouts and startup selection.

## Firmware update from SD

1. Copy the firmware image supplied for your product to the SD card's FAT
   root as `FIRMWARE.BIN`.
2. Finish downloads, printing and other SD writes, then stop USB or FTP SD
   sharing.
3. Restart into Golden. From the firmware console, `:reboot` enters Golden.
4. Keep power on while Golden checks the file, programs its firmware slot
   and verifies the flash contents.
5. Check the update log and the firmware version after boot. The normal SD
   acknowledgement renames `FIRMWARE.BIN` to `FIRMWARE.OK`.

Golden scans these filenames in order:

| Candidate file | Acknowledged file |
| --- | --- |
| `FIRMWARE.BIN` | `FIRMWARE.OK` |
| `FIRMWARE-APPLETINI.BIN` | `FIRMWARE-APPLETINI.OK` |
| `FIRMWARE-MULTITINI.BIN` | `FIRMWARE-MULTITINI.OK` |

This permits installing both products in one pass. A bad candidate does
not prevent a later valid candidate from being checked. If several valid
files target the same product, the last successful installation wins.

Firmware installation writes the selected product slot and its metadata;
the other firmware slot and both Golden slots remain available. Golden
boots a verified selection after processing the complete batch.

The `.OK` rename follows flash verification and closing the source file.
An SD close or rename error can leave `.BIN` present even after successful
programming. Use the log and booted version to establish the result before
retrying. Keep user media and settings backed up when maintaining the card.

## Golden boot update

Use the Appletini configuration menu for the normal Golden update:

1. Boot a compatible working firmware and copy the supplied `BOOT.BIN` to
   the SD card's FAT root.
2. Finish downloads, printing, screenshots and other SD writes. Stop USB
   and FTP SD sharing.
3. On a physical Apple II, enter the **initial boot configuration menu**.
   Reopening the menu after handing storage to the Apple does not enable
   this operation. The isolated ONE//e configuration menu also supports it.
4. Open **About**, select **Update Golden Boot...** beside the Golden
   version, then select **Update and reboot** in the confirmation dialog.
   The initial selection is **Cancel**.
5. Keep the card powered and leave the SD card installed until the automatic
   reboots finish and the normal firmware returns.
6. Check the installed Golden version in **About** and the `BOOT.OK`
   acknowledgement on SD.

The button appears when `BOOT.BIN` is present. It is disabled outside the
allowed startup/isolation state; file presence alone does not validate it.
The updater checks the image and a verified recovery-capable firmware before
writing. The menu also saves media and obtains a safe Apple-bus hold.

The update writes and verifies trial Golden at `0x00100000`, then boots it.
That running trial verifies and repairs primary Golden at `0x00000000`,
leaving the trial copy intact, and boots the verified primary.

After the primary image is verified and matches the SD file byte for byte,
Golden renames `BOOT.BIN` to `BOOT.OK`. An SD acknowledgement error leaves
the file for a later retry; it does not invalidate a verified primary image.

The console command `:selfupdate` can also stage SD-root `BOOT.BIN`. Finish
all downloads, printing, screenshots and SD activity before using this
serial alternative; it does not perform all the menu's activity checks.

## Serial recovery

Connect the card's CP2105 USB-to-UART bridge and select one of its serial
ports. Both console UARTs use **921600 baud, 8 data bits, no parity, one stop
bit and no flow control**. Port numbers depend on the PC. The serial tools
require Python and `pyserial`. Finish SD activity before rebooting a running
frontend for a serial upload.

List available ports:

```bat
python scripts\serial_firmware_update.py --list-ports
```

Replace `COM3` below with the card's console port. Close any terminal using
that port before starting the upload tool.

To upload firmware from a running compatible frontend:

```bat
python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
```

`--reboot-golden` sends `:reboot` and catches the Golden monitor. If the card
is already in that monitor, omit this option. Golden receives the image
with XMODEM-CRC and validates it before installing it.

To upload a Golden image through the monitor:

```bat
python scripts\serial_firmware_update.py .\BOOT.BIN --golden --port COM3 --reboot-golden
```

`--golden` selects the monitor's `selfrx` command. This receives the image
into RAM and uses the verified trial-to-primary procedure; it needs no
SD-root `BOOT.BIN`. A verified recovery-capable firmware must still be
installed. Keep power on through the automatic reboots.

For a local image check that does not open a serial port:

```bat
python scripts\image_manifest.py verify .\BOOT.BIN --role golden --require-recovery-capable
python scripts\image_manifest.py verify .\FIRMWARE.BIN --role firmware --require-recovery-capable
```

Factory/JTAG programming tools are described in
[the scripts guide](scripts/SCRIPTS_README.md).
