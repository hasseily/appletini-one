# ONE//e Hardware Test Record and F0.9.79 Retest Plan

## F0.9.77 Test Result

The first hardware test used the F0.9.77 image built from source commit
`36a5bd21710caa75b48073cf6f46cdb0b4d3b002`:

- Archived file:
  `.timing_runs/20260816T152953Z-36a5bd21-full/FIRMWARE_TEST.BIN`
- Size: `4,236,236` bytes
- SHA-256:
  `1b5a932f836471075fdb9a7a92664be44af2f0537787ece638ad9314a18d9b0f`
- Timing build: `20260816T152953Z-36a5bd21-full`
- Route: WNS `+0.069 ns`, TNS `0`, WHS `+0.046 ns`, no failed or
  unrouted paths

The user powered the card while it was out of the Apple slot by applying the
needed +5 V to the slot bus power pin. The boot menu showed ONE//e as
`LOCKED`. This was a false lock, not proof that an Apple was present.

F0.9.77 assumed that an unplugged U533 presented one fixed Apple idle vector.
The open connector presented another stable vector, so the level comparison
kept the lock set. Physical isolation also disabled the main translator. That
removed the same PHI0, 7M, and Q3 observations which the guard needed to detect
an Apple after ONE//e selection.

The F0.9.77 image is now a historical test artifact. Do not use it to retest
the corrected start or clock-detection behavior.

## F0.9.78 Correction

F0.9.78 source commit
`7838f23580d95a03e2e9f2442d80f2e3ce9c6ebf` changes the guard as follows:

- Learn any stable U533 input vector instead of requiring fixed idle levels.
- Require 96 quiet fabric clocks before arming. At 133.333 MHz, that interval
  is about 0.72 microseconds.
- Watch transitions on PHI0, 7M, Q3, M2SEL, M2B0, and DEVSEL# through U533.
- Block start or kill a running ONE//e when any watched input changes.
- Keep the main translators enabled but forced to Apple-to-FPGA input
  direction during ONE//e, so the clock monitor stays live.
- Disable bidirectional auxiliary translator U234 during ONE//e. Its isolated
  RESET#, INH#, IRQ#, NMI#, RDY#, and DMA# outputs do not enter the guard.
- Keep the activity lock sticky and require a later manual off/on selection.

## F0.9.78 Test Image

- Source commit: `7838f23580d95a03e2e9f2442d80f2e3ce9c6ebf`
- Timing build: `20260816T164305Z-7838f235-full`
- Build status: `exported`
- Route: WNS `+0.019 ns`, TNS `0`, WHS `+0.063 ns`, THS `0`, and WPWS
  `+0.265 ns`
- Failures: zero setup, hold, or pulse-width failing endpoints; zero route
  errors, missing constraint objects, or unconstrained internal endpoints
- XSA SHA-256:
  `314435c8f18adfade2c2d766871d59c30b8d6078f548e33f9686a5c9dc758ed5`
- Bitstream SHA-256:
  `46a4f3ffa6d03bd6d9887506689e7b1c8ef605e6f7c252ad24a7e41f8094bbf6`
- Files: root `FIRMWARE.BIN` and
  `.timing_runs/20260816T164305Z-7838f235-full/FIRMWARE_TEST.BIN`
- Handoff record:
  `.timing_runs/20260816T164305Z-7838f235-full/test_firmware_manifest.txt`
- Size: `4,235,276` bytes each
- Firmware SHA-256:
  `47f7385846b8092e46784649a0f99f7b90e7d2415d2ee02eb627c2628a71d9fa`

The two firmware files are byte-identical. A fresh Vitis workspace used the
exact archived XSA; platform export and all application builds succeeded.
Bootgen readback reports `total_images=4`. All stated focused ONE//e HDL and
frontend regressions passed, including the vTW, boot-sector, and virtual-card
checks.

The Vivado manifest records `git_dirty=1` because these two ONE//e documents
were being edited. No HDL or firmware source was dirty. This image is below
the project's release-only +0.300 ns setup-margin gate. The user waived that
gate for this test image only; this is not a promoted release. The corrected
image starts ONE//e out of the Apple slot, but the functional test found the
three faults below.

## F0.9.78 Functional Test Result

F0.9.78 reached the Enhanced //e screen. The test then found these faults:

- Selecting SmartPort did not boot SmartPort. If Disk II drive 1 had bootable
  media, the ROM booted it instead; without Disk II media, it stayed at
  `Apple //e`.
- While Disk II drive 1 ran, the storage overlay could say
  `SMARTPORT SP1`.
- A bound USB TransWarp speed key changed its notice but did not change the
  machine's speed. Changing speed on the TransWarp config page did work.

These were state-model faults, not three unrelated device faults. ONE//e had
session state beside the saved config and normal host-run state, but three
callers still read the wrong copy:

- The cold-slot helper always hid slot 7. It did not receive the configured
  SmartPort or Disk II target.
- ONE//e enabled virtual Disk II for the session without changing the saved
  Slot 6 bit. The overlay used that saved bit to decide whether Disk II
  activity existed, then retained a SmartPort `STATUS` poll as its source.
- The USB key path checked saved host TransWarp intent and wrote `VTW_CTRL`
  only for the host state machine. It still made a success label. The config
  page had its own ONE//e write path, which is why that path worked.

## F0.9.79 Runtime Corrections

The F0.9.79 source uses one clear rule at each boundary:

- The configured target controls the ONE//e cold scan. SmartPort exposes slot
  7 on the first `$C7xx` probe. Disk II hides slot 7 until the first `$C6xx`
  probe, then releases it. A virtual warm reset re-arms the same configured
  target. The normal host handoff keeps its own fallback rule, so a disabled
  physical Slot 6 does not rewrite ONE//e's virtual target.
- The overlay polls the effective Disk II service state, not the saved Slot 6
  bit. SmartPort data has first priority, then active Disk II motor/read/write
  work, then SmartPort `STATUS`, then the last valid source. A discovery poll
  can no longer replace an active `DISK II D1` label.
- Menu and USB speed changes use one live `VTW_CTRL` writer. It chooses the
  running host or ONE//e word, writes it, and verifies the register readback
  before it shows success. A failed USB change restores its old override and
  reports `TW: CONTROL WRITE FAILED`.
- All reset and config paths use one effective Disk II setter. During ONE//e,
  it keeps the session service enabled without changing saved host state. When
  ONE//e stops, it applies the latest saved state instead of a value captured
  at session start. A private ONE//e reset also skips the host-only IIgs
  `$C029` DMA write.
- A SmartPort-target ONE//e session overrides saved SuperSprite slot-7
  ownership. Without that session override, SuperSprite could still claim the
  shared slot and keep SmartPort's ROM invisible even after the cold-scan fix.

The source correction and focused regression are complete. The full Vivado and
Vitis build, package, and hardware retest remain pending.

## Detection Limit

Slot +5 V cannot identify an Apple in this stand-alone setup because that same
pin supplies the card when it is out of the computer. The design therefore
uses the Apple bus clocks, not slot +5 V, to detect a powered and running host.
PHI0, 7M, or Q3 will change within the sub-microsecond guard window during
normal Apple operation.

There is no independent power sensor on this board. The clock guard cannot
prove that a host with all watched clocks stopped is off. A powered,
clock-stopped host and an unplugged connector may both present a stable U533
vector. The connected-Apple test must still prove translator direction,
output enable, leakage, back-power, and high impedance with test gear.

## F0.9.78 Package Checks

- [x] Revised guard and bus-isolation simulations passed.
- [x] All stated focused ONE//e HDL and frontend regressions passed.
- [x] The full Vivado run routed and exported its immutable bitstream and XSA.
- [x] Vitis platform export and application builds used that exact XSA and
  completed successfully.
- [x] F0.9.78 packaging, byte comparison, SHA-256 checks, and Bootgen readback
  passed.
- [x] Program F0.9.78 and start ONE//e out of the Apple slot. It reached the
  Enhanced //e screen and exposed the functional faults above.

## F0.9.79 Readiness

- [x] Pass the final cold-target, joined-bus, real-ROM, effective Disk II,
  SuperSprite override, storage-selection, and live-speed-control tests.
- [ ] Complete a full Vivado route and export with no failing path.
- [ ] Build Vitis from the exact exported XSA.
- [ ] Package F0.9.79, record its source commit, size, SHA-256, timing result,
  and Bootgen readback, and compare the root and archived files byte for byte.
- [ ] Program F0.9.79 and complete the hardware checks below.

Do not use the root `FIRMWARE.BIN` for this retest until this section names and
checks the F0.9.79 package. Keep the golden boot image unchanged.

## Program the F0.9.79 Test Slot

This step remains pending until the package checks above pass.

1. Connect the card's update UART and its stand-alone power source.
2. Find the port if needed:

   ```powershell
   python scripts\serial_firmware_update.py --list-ports
   ```

3. After checking that root `FIRMWARE.BIN` is the recorded F0.9.79 file,
   upload it, replacing `COM3` with the update UART port:

   ```powershell
   python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
   ```

The updater writes the firmware slot and checks the full programmed image.

## F0.9.79 Out-of-Slot Retest

This hardware retest is still pending.

Connect DVI, audio, a USB keyboard, and the SD card. A USB gamepad is optional.
Keep the Apple slot edge disconnected and apply the same stand-alone supply
used for the F0.9.77 test.

1. Power the card and confirm the on-screen firmware version is `F0.9.79`.
2. Attach bootable images to both SmartPort SP1 and Disk II drive 1. Keep both
   available so the result proves the target choice rather than media fallback.
3. Enable SuperSprite in saved config, select SmartPort as the boot target,
   then select `ONE//e standalone` in `Boot Settings`.
4. Confirm that the item changes from `OFF` to `RUNNING`, not `LOCKED`, and
   that the ROM takes the first slot-7 SmartPort path and boots SP1. It must not
   fall through to Disk II.
5. Confirm that SmartPort data work says `SMARTPORT SP1`. Use `Ctrl+Pause` and
   confirm that the virtual warm reset keeps SmartPort as the target and boots
   it again. The reset must not touch a physical host or run the IIgs `$C029`
   DMA write.
6. Stop ONE//e. Select Disk II as the boot target and start ONE//e again.
   Confirm that the ROM hides slot 7 for the cold scan, reaches `$C600`, and
   boots drive 1. The activity label must say `DISK II D1`, even if SmartPort
   sends `STATUS` calls.
7. While ONE//e runs, change the saved Slot 6 setting and apply config. Confirm
   that the session Disk II service stays enabled. Stop ONE//e and confirm that
   the latest saved Slot 6 state, not the state from session start, takes
   effect.
8. Press each bound TransWarp toggle, speed-up, and speed-down key. Confirm the
   notice changes and time a fixed loop or other repeatable task at each speed;
   the measured rate must change with the label. If the slug key is armed,
   check it the same way.
9. Turn ONE//e off, wait for the stable connector vector to pass the 96-cycle
   quiet check, and select it again.

A request never restarts itself after a real activity lock.

## Functional Checks

Run these checks before any Apple-connected safety test:

- Boot DOS and ProDOS to their ready screen.
- Use a standard full-size USB HID keyboard. Type letters, numbers,
  punctuation, arrows, Return, Escape, Delete, and numeric keypad keys.
- Check held-key repeat.
- Check Open Apple with left Alt or GUI, and Closed Apple with right Alt or
  GUI.
- Confirm the fixed US layout. A boot-protocol keyboard has six-key rollover
  plus modifiers; a parsed report keyboard tracks at most eight active key
  usages. Lock LEDs do not track ONE//e, and exotic NKRO reports are not
  guaranteed.
- Check a paddle or joystick program with all axes and the first three
  buttons.
- Use `Ctrl+Pause` and confirm that only the virtual //e resets.
- Run software which toggles the Apple speaker; check both audio channels,
  mute, and mix with card audio.
- Check 40/80-column text, lores, hires, double-hires, and mixed video.
- Reopen the menu, turn ONE//e off, and confirm the state returns to `OFF`.

Record the UART log, disk image name, USB device names, DVI mode, and card
control register `0x5B` for each failure.

## Apple-Connected Test Gate

Only move on after checking the connector with test gear. Prove that every
Apple-side driver stays high impedance while ONE//e starts, runs, stops, and
loses PL configuration. Confirm that the main translators remain input-only
and U234 stays disabled. Cover both power orders and an Apple which starts its
clocks while ONE//e runs.

PHI0, 7M, or Q3 activity must stop ONE//e at once, keep the connector isolated,
and require a later manual off/on selection. Also test the stated limit with a
clock-stopped Apple before approving use while the card remains installed.
