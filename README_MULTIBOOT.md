# Appletini and Multitini multiboot

Golden B2.0.6 supports two firmware slots. Multitini firmware is M0.2.3;
Appletini firmware is F1.3.0. Appletini includes the
[custom glyph API](README_GLYPH_API.md) and the changes in
[the F1.3.0 release notes](README_FIRMWARE_F1.3.0.md). This build retains the
audited Appletini FPGA image and rebuilds both ARM frontend cores.

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
3. With B2.0.1 or later installed, copy `sd-root/FIRMWARE-APPLETINI.BIN` and
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

An old version 1 firmware file remains usable as a boot fallback. Golden
B2.0.6 can also restore an identified, recovery-capable Appletini F1.1.0 or
later image after validating its size and contents. Unsupported or ambiguous
legacy images are refused.
The first Multitini-slot update refuses a legacy image that extends beyond the
new 6 MiB Appletini boundary. Current selected legacy packages fit within that
boundary. A multiboot firmware refuses a Golden downgrade without the new
capability marker.

The historical B1.1.0 and B1.2.0 SD installers copy `FIRMWARE.BIN` verbatim to
`0x200000`; they do not parse its trailing manifest. A tagged Appletini image
can therefore be installed before B2, and its normal frontend can run with
multiboot controls disabled. Those installers ignore `BOOT.BIN` and the named
firmware files. B1.1.0 requires FAT32. Never use a Multitini image as the generic
file on B1: it has no product-aware destination selection.

Appletini F1.3.0-dev10 supports firmware-first migration without serial commands:
copy the bundle's `appletini/FIRMWARE.BIN` as `FIRMWARE.BIN`, its `golden/BOOT.BIN`
as `BOOT.BIN`, and `sd-root/FIRMWARE-MULTITINI.BIN` to SD root. Let the installed
B1 update Appletini first. In About, choose **Update Golden Boot...** beside the
Golden Boot version from the initial boot menu, then explicitly choose
**Update and reboot** in the modal. On a physical Apple II the button is disabled
after boot handoff, even if the config menu is opened again. ONE//e remains eligible.
This stages and verifies the trial from the running firmware, then reboots into
the new Golden. Golden repairs primary and automatically installs the named
Multitini file. The complete F-then-B-then-M procedure still needs board validation.

Multitini's System page has the matching action. The button appears only while
SD-root `BOOT.BIN` is present. File presence does not validate its contents: the
existing updater checks the image and recovery fallback again before writing.
Updates flush media and hold the machine safely. A pre-handoff failure returns
to the menu with an error; a failure after CPU1 was reset retains a safe hold.
Confirmation starts on Cancel. Nothing updates merely because BOOT.BIN exists.

Both menus read the installed Golden version from its loaded executable in
flash, rather than showing a version compiled into the running firmware. A
valid manifest provides full CRC verification. Historical raw B1.1 has only
structural/startup-identity checks because it has no payload CRC. An unreadable
or unrecognized primary reports Unknown; an invalid primary can use a valid
trial copy. Drawing the menu does not read SD or flash.
Direct programming scripts require product-tagged
files and choose fixed product offsets, but do not provide the transactional SD
updater's power-loss protection. No hardware programming occurs during builds.

## Appletini startup correction

The owner reported that both F1.3.0-dev bundles stop after the initial SMARTPORT
handoff message on an Apple //e with Golden 1.x. F1.3.0-dev2 restores the existing
CPU1 cold-start path: publish its entry address and issue SEV without resetting
the processor. A CPU1 reset can instead enter the retained FSBL at its low reset
vector and repeat hardware initialization while CPU0 is running.

The frontend also reserves a 64 KiB stack. Its menu state and new flash-validation
call chain exceed the previous 8 KiB reservation. The builder checks the actual
linked stack and heap spans independently and records them in the package.
The FPGA, FSBL and CPU1 payload remain the exact selected F1.2.6-d1 inputs.
The owner confirmed that F1.3.0-dev2 boots on the Apple //e with Golden 1.x.
F1.3.0-dev3 retains those corrections. The owner confirmed that it reports the
installed B1.2.0, but its Golden update cancels with an Apple bus hold timeout.
F1.3.0-dev4 replaces the obsolete DMA-hold debug window, which is absent from the
retained FPGA, with the existing vTW Apple RESET control. It verifies both the
control readback and physical RESET status before continuing, saves media and
reserves the real PS-DMA port only when its software owner and hardware BUSY are
clear. The physical host stays in RESET through the final handoff. A recoverable
failure clears reset-stale SmartPort state and restores the saved control; the
Apple then resumes through its reset handler. The config error screen stays open.
Native tests use the actual register headers and model the absent legacy DMA
window as zero-read/write-ignored. The menu-driven update still needs a board test.
B2.0.2 also reserves a checked 64 KiB Golden stack: installation
and nested validation cannot fit the previous 8 KiB template stack.
A failed firmware handoff after CPU1 was held
requires a card reboot; it does not attempt an unsafe warm restart.

## Multitini handoff correction

The owner reports successful B2.0.2 and M0.2.1 installation, followed by an
unexpected Multitini startup, a minute-long System entry, and a cancelled switch
to Appletini. Subsequent core loads report FatFs `FR_NOT_ENABLED` (backend 12)
before PCAP reads any bytes. This does not establish that the machine cores run.

B2.0.3 preserves the existing verified tagged startup choice when creating
multiboot metadata for the first time. Adding Multitini no longer promotes it
over the Appletini already installed by B1. Existing saved choices are preserved:
an affected card should select **System > Startup firmware > Appletini** once.

M0.2.2 uses `f_mount(NULL, "0:/", 0)` to detach SD. The previous option 1 detached
the filesystem and then failed an immediate mount with no filesystem object,
leaving the wrapper's cached mounted state wrong. The fix permits a clean
handoff and remount; open files and failed closes still block unmount. Switch and
Golden-update transactions finish metadata, SD, UART and boot-register checks
before the final CPU1 hold. An attempted CPU1 reset cannot be resumed in place.

System and Appletini menu status use bounded live header/footer/metadata probes
and a Golden identity cache verified once during startup. These display hints
do not authorize a boot. Startup-choice writes and Golden selection retain full
live payload, role and CRC checks. An altered payload may remain visible until
Golden checks it after a switch, but it is never booted unchecked.
System entry no longer rereads whole firmware images. Full switch validation
has a visible progress message.

Native tests exercise the exact installed FatFs `f_mount` implementation,
SD detach/remount and fault ownership, menu-cache read counts, migration power
cuts, and deferred handoff failure boundaries. These corrections retain all
FPGA, FSBL and CPU1 inputs. The corrected runtime switching still needs a board test.

## Golden update completion

The owner confirms the preceding bundle works. B2.0.4 renames SD-root `BOOT.BIN`
to `BOOT.OK` only when its complete bytes, including the manifest, match the
live fully validated primary Golden image. Trial staging keeps `BOOT.BIN`; a
running trial first repairs and verifies primary before acknowledgement. Primary
boot retries acknowledgement after a power cut between primary commit and rename.
An identical file is already installed and can be acknowledged; a different
file remains an update candidate and is never installed merely by this check.

The source must close successfully before replacing an older regular `BOOT.OK`.
A directory at that path is preserved. SD mount/read/close/delete/rename errors
leave the source for retry and do not invalidate verified flash or block boot.
FatFs directory changes retain their normal power-loss limits. The completion
step writes no flash. The B2.0.4 bundle retains the accepted M0.2.2 and F1.3.0-dev5
images exactly; only Golden is rebuilt. The rename still needs a board test.

## Switch performance

M0.2.3, F1.3.0-dev6 and B2.0.5 reduce flash validation time. A firmware switch
fully verifies Golden and probes the requested firmware's header and committed
record before saving the one-shot request. It uses the verified Golden offset
for reset. Golden performs the full target payload and CRC check after reset.
A corrupt payload therefore reboots through Golden and falls back instead of
cancelling immediately in the running firmware. If metadata must first be
created, adoption still fully verifies the slots before recording them.
Golden selects once after its SD update pass and boots the selected offset.
Missing or corrupt targets still fall back to another fully verified firmware.

Shared CRC uses a constant lookup table with the same incremental CRC result.
Flash reads use batches of up to 4 KiB. Known compatible 16 MiB NOR parts on the
single four-bit QSPI connection can use quad I/O reads at 25 MHz, matching the
retained FSBL's command and clock. Qualification compares ordinary and quad reads
at several addresses and requires non-erased data. Unsupported parts, mismatches
and transfer errors use ordinary reads at 6.25 MHz when the controller remains
usable. Failed state restoration blocks further flash operations. The driver
restores controller state after reads and does not change flash protection or
quad-enable bits.
Erase and page program keep their existing conservative path.

Golden retains its one-second serial recovery window. UART stage timings and
flash read diagnostics distinguish validation from reset, FSBL and frontend
startup. Host CRC equivalence, QSPI fault fixtures, request power cuts and ARM
builds do not establish a board speedup. Measure both switch directions on the
card after installing all three images; no FPGA/core rebuild is needed.

## Appletini metadata diagnostics

The owner reports faster switching with the preceding speed bundle, but
Appletini displays `boot metadata invalid`. Its old diagnostic reader accepts
only single-firmware FWMT/v4 metadata and reads the new FE0000 layout base.
Multiboot instead uses CRC-protected ATNMBT1 records at FE0000 and FF0000.

F1.3.0-dev7 reads both modern records through the shared metadata validator.
If neither record is valid, it can recognize a CRC-valid legacy v4 record at
its original FF0000 address. Read failures retain the warning. Startup caches
this diagnostic result; drawing and control snapshots do not read flash. The
HUD shows the actual cached Golden version independently of metadata health.
This correction writes no flash metadata and does not change boot selection.
M0.2.3, B2.0.5, FPGA, core, FSBL and CPU1 inputs are retained exactly. Only the
Appletini firmware needs updating. The replacement HUD still needs a board test.

## Standalone power and Appletini screenshots

F1.3.0-dev8 corrects Boot Settings help: Multitini installed in an Apple //e gets
5V from the host and needs no standalone jumper. The jumper supplies 5V when the
card is outside the Apple for standalone ONE//e or Multitini. It does not select
the firmware or operating mode.

Full output screenshots now include the FPGA aperture, shadow or LCD mask using
the completed output slot's mask, dimensions and mode. Scanlines, ghosting, blur,
glow and dot bleed are already in that slot's pixels and are preserved directly.
The capture copies pixels and settings before yielding; later video changes do
not change the saved image. Raw Apple screenshots retain their existing colors,
border selection, scaling and scanline rules.

PNG rows use 64 KiB staging, up to 16 rows per poll, with a 4 ms budget checked
between writes. A synchronous SD write or close can exceed that budget. The file
still closes before returning to other services so remounts remain safe. At
1080p, native tests reduce row writes from 2160 to 135 and row batches from 540
to 68; those operation counts do not establish elapsed board timing. Partial
files, cancellation, USB/SD ownership and non-clobber publication are retained.

Native tests decode PNG pixels and CRC/Adler data, exercise I/O failures and
frame changes, and check all six output modes. The software mask also matches
simulation of the exact retained RTL. Only Appletini ARM firmware changes;
M0.2.3, B2.0.5, FPGA/core, FSBL and CPU1 inputs are retained. Screenshot speed and
display matching still need the owner's card test.

## Saved vTW startup

F1.3.0-dev9 starts saved host vTW before the physical Apple //e reaches the
initial prompt or config menu. CPU0 installs a firmware C8 ROM patch while the
initial Apple RESET is held. Only the IIe identification return waits for vTW;
II/II+, IIgs and unidentified hosts retain their existing startup paths.

The accelerated core runs the initial prompt/menu at 1 MHz to preserve the ROM's
VBL timing, then applies the requested speed at storage handoff without another
RESET or ROM copy. Disk II selection returns after the first CORE_RUN echo so
slot 7 remains visible for that initial prompt. Cancellation, policy refusal,
failed takeover and ONE//e startup release the native hold. Late SD restores and
manual host-vTW enabling retain the existing post-handoff path.

ONE//e now publishes menu PAUSE before its first CORE_RUN write. The reported
bezel-only case had ONE//e disabled; it does not require a renderer change.
Native startup, ROM execution and failure tests accompany this change. Only
Appletini ARM firmware changes; M0.2.3, B2.0.5, FPGA/core, FSBL and CPU1 inputs
are retained. First boot and menu exit still need the owner's card test.

## Native ProDOS 80-column startup

With Appletini supplying auxiliary RAM, the native Apple //e cold-start
probe runs before that RAM is available and leaves external slot-3 ROM
selected. ProDOS 2.4.3 scans the slots before restoring the internal ROM,
so BASIC.SYSTEM caches its "No device connected" handler for `PR#3`.
The reported first-boot values were `$BF98=$B0`, `$BF99=$86`, and
`$BE16=$9AEE`; a `PR#7` reload changed them to `$B2`, `$8E`, and `$C300`.

F1.3.0 patches the ARM-loaded C8 boot helper to wait for the RAM policy
and then select the internal slot-3 ROM before booting ProDOS. The
selection applies when Appletini provides RAM. Physical auxiliary cards,
disabled RAM, and vTW retain their normal ROM-selection paths. The C7
slot ROM and FPGA bitstream remain unchanged. Regression tests use the
actual Apple //e ROM, ProDOS 2.4.3, and BASIC.SYSTEM bytes. Device retesting
of the rebuilt image remains pending.

## Shorter vTW slowdown windows

F1.3.0-dev10 adds 32, 64 and 128 Apple-cycle choices before the existing
256 through 65535 presets. The saved default remains 512 cycles; an unlisted
saved value still uses that default as the starting point for menu navigation.
The retained FPGA already accepts arbitrary 16-bit durations. Only Appletini
ARM firmware changes; Golden B2.0.5, Multitini M0.2.3 and hardware inputs remain
byte-identical.

Speed Race on the owner's physical //e in TURBO slows when Disk II slot 6 is
enabled. The retained Disk II core replays classic-cycle timing debt after all
TURBO steps, including while the motor is stopped, and globally gates the CPU
until that debt drains. This predates multiboot. The existing Disable DiskII
Acceleration option removes that idle replay cost while retaining slot 6 and
normal 1 MHz Disk II accesses. Optimizing the accelerated idle path requires an
FPGA change; dev10 does not change disk timing or the acceleration setting.

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
