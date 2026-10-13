# Appletini and Multitini multiboot

Branch `multiboot` in each repository adds shared Golden B2.0.1 and two firmware
slots. Multitini firmware is M0.2.0; Appletini firmware is F1.3.0-dev. These are
development builds. Board validation is pending. No FPGA image changes are
required: the packages retain their selected supervisor, machine and Appletini
bitstreams.

## Flash layout

The production card has 16 MiB QSPI NOR.

| Region | Offset | Capacity |
| --- | --- | --- |
| Primary golden | `0x000000` | 1 MiB |
| Trial golden | `0x100000` | 1 MiB |
| Appletini firmware | `0x200000` | 6 MiB |
| Multitini firmware | `0x800000` | `0x7E0000` bytes |
| Metadata copy A | `0xFE0000` | 64 KiB |
| Metadata copy B | `0xFF0000` | 64 KiB |

Firmware files carry a version 2 manifest with their product identity, exact
length and CRC. Each executable also links its own product marker. Renaming a
file or changing its footer cannot turn an Appletini image into a Multitini
image. Golden keeps the existing version 1 manifest for migration through the
installed recovery updater, and links a new multiboot capability marker.

## Startup and menus

Golden checks the full image in flash before every boot. It tries a one-shot
menu request, then the saved startup choice, then the other verified firmware.
A missing, damaged, wrong-product or stale image record never authorizes a
boot. If neither tagged image is valid, a verified legacy image can still boot;
otherwise Golden stays in its recovery monitor.

In Multitini, use **System > Startup firmware** or **Switch to Appletini**.
In Appletini, use **Boot Settings > Startup firmware** or **Switch to Multitini**.
Changing Startup firmware saves the power-on preference. Switching firmware
uses a one-shot request and leaves that preference unchanged. Missing targets
are disabled in the menus. Both actions check the live flash again before
writing a request or preference. They require compatible Golden.

The handoff flushes writable media, drains video activity, closes SD owners,
stops USB, holds CPU1 in reset and restarts through Golden. A failed preparation
cancels the switch and attempts to restore the running firmware. Appletini
also requires a safe Apple-bus hold or guarded stand-alone isolation. It refuses
switches during active SD sharing, screenshots or print jobs. Running Appletini
while the card is installed in an Apple II still requires the host conditions
that Appletini normally needs.

## Upgrade from a single firmware

1. Start with a recovery-capable installed firmware, such as M0.1.0 or
   F1.2.6-d1. Preserve user SD files and retain the previous firmware package.
2. Put the new shared `BOOT.BIN` in the SD root. Run `:selfupdate` from the
   firmware's serial console. This stages the trial Golden, verifies it and
   repairs the primary through the existing recovery procedure.
3. With B2.0.1 installed, copy `sd-root/FIRMWARE-APPLETINI.BIN` and
   `sd-root/FIRMWARE-MULTITINI.BIN` from the bundle to the SD root together,
   then enter Golden once (for example, run `:reboot` in the serial console).
   Golden installs each file serially and replaces `.BIN` with `.OK` after
   a successful update. The updater verifies before erasing and preserves
   the other firmware slot.
4. Select the power-on preference in either configuration menu. Merge the
   bundle's `multitini/sd/multitini/` into SD `/multitini/` if core files are
   needed, preserving user media and settings.

Golden scans `FIRMWARE.BIN`, `FIRMWARE-APPLETINI.BIN`, then
`FIRMWARE-MULTITINI.BIN`. These names only identify files to inspect. The
manifest and linked product marker determine the destination. An Appletini
image named `FIRMWARE-MULTITINI.BIN` still updates Appletini. A failed update
leaves its `.BIN` file in place and does not stop the remaining files from
being checked. If multiple valid files target the same product, the last
successful update in scan order wins. `BOOT.BIN` still requires the explicit
`:selfupdate` procedure above. A failed file is not retried during that pass;
leave it in place and enter Golden again after resolving the failure.

An old version 1 firmware file remains usable as a boot fallback. The new
updater refuses to install it because it lacks an unambiguous product identity.
The first Multitini-slot update refuses a legacy image that extends beyond the
new 6 MiB Appletini boundary. Current selected legacy packages fit within that
boundary. A multiboot firmware refuses a Golden downgrade without the new
capability marker.

Do not feed tagged firmware to an old single-slot Golden: it lacks version 2
support. Upgrade Golden first. Direct programming scripts require product-tagged
files and choose fixed product offsets, but do not provide the transactional SD
updater's power-loss protection. No hardware programming occurs during builds.

## Failure handling and evidence

Two CRC-protected metadata sectors retain the most recent complete record.
Updates first revoke only the target slot, withhold its Bootgen boot identifier
through erase/program/readback, then commit the verified image and new record.
Power-loss and injected read/write-failure tests cover every update mutation.
The unaffected verified slot remains available. A failed one-shot-clear write
can repeat the requested firmware on a later reset, but cannot authorize an
unverified image. CRCs detect corruption; they do not authenticate publishers.

`scripts/check_multiboot_sync.py` checks that shared code matches between the
two repositories. Host tests exercise the production image validator, metadata,
updater, Golden self-update and CPU1 controls. Build manifests and payload audits
record exact retained hardware and rebuilt ARM inputs. Flash installation,
power cycling and live switching in both directions still need board tests.
