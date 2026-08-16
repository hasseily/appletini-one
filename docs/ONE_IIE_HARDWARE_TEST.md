# ONE//e First Hardware Test

This guide tests the `F0.9.77` ONE//e image built from source commit
`36a5bd21710caa75b48073cf6f46cdb0b4d3b002`.

## Test Image

- File: `FIRMWARE.BIN`
- Size: `4,236,236` bytes
- SHA-256:
  `1b5a932f836471075fdb9a7a92664be44af2f0537787ece638ad9314a18d9b0f`
- Timing build: `20260816T152953Z-36a5bd21-full`
- Route: WNS `+0.069 ns`, TNS `0`, WHS `+0.046 ns`, no failed or
  unrouted paths

The same bytes are saved as
`.timing_runs/20260816T152953Z-36a5bd21-full/FIRMWARE_TEST.BIN` with a test
manifest. This is a user-approved test image. It is below the project's
release-only `+0.300 ns` setup-margin gate.

## Safety Limit

Run the first test with Appletini ONE physically out of the Apple slot. The
current board has no slot-power sense. RTL catches Apple bus clocks, edges,
and held non-idle levels, but it cannot tell an unpowered Apple from a powered
Apple whose watched pins all sit at the idle levels.

Do not use a powered Apple for the first test. Do not test live insertion,
Apple-first power, card-first power, or the activity shutoff until the board
translator OE, DIR, leakage, and back-power checks have passed.

## Program the Test Slot

Keep the golden boot image unchanged. Install only the firmware slot image.

1. Connect the card's update UART and its stand-alone power source.
2. Find the port if needed:

   ```powershell
   python scripts\serial_firmware_update.py --list-ports
   ```

3. Upload the image, replacing `COM3` with the update UART port:

   ```powershell
   python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
   ```

The updater writes the firmware slot and checks the full programmed image.

## First Boot

Connect DVI, audio, a USB keyboard, and the SD card. A USB gamepad is optional.
Keep the Apple slot edge disconnected.

1. Power the card and confirm the on-screen firmware version is `F0.9.77`.
2. In the normal menu, attach a bootable DOS or ProDOS image to virtual Disk II
   drive 1.
3. Open `Boot Settings` and select `ONE//e standalone`.
4. The item should change from `OFF` to `RUNNING`, and the menu should close.
5. The Enhanced //e ROM should scan slot 6 and boot the virtual Disk II image.

If the item shows `LOCKED`, turn it off, wait for the connector inputs to be
quiet, then select it again. A request never restarts itself.

## Functional Checks

Run these checks before any Apple-connected safety test:

- Boot DOS and ProDOS to their ready screen.
- Type letters, numbers, punctuation, arrows, Return, Escape, and Delete.
- Check held-key repeat.
- Check Open Apple with left Alt or GUI, and Closed Apple with right Alt or
  GUI.
- Check a paddle or joystick program with all axes and the first three
  buttons.
- Use `Ctrl+Pause` and confirm that only the virtual //e resets.
- Run software which toggles the Apple speaker; check both audio channels,
  mute, and mix with card audio.
- Check 40/80-column text, lores, hires, double-hires, and mixed video.
- Reopen the menu, turn ONE//e off, and confirm the state returns to `OFF`.

Record the UART log, the disk image name, USB device names, DVI mode, and any
`LOCKED` status bits for each failure.

## Apple-Connected Test Gate

Only move on after checking the connector with test gear. Prove that every
Apple-side driver stays high impedance while ONE//e starts, runs, stops, and
loses PL configuration. Cover both power orders and an Apple which turns on
while ONE//e runs. The live Apple activity test must stop ONE//e at once, keep
the connector isolated, and require a later manual off/on selection.

