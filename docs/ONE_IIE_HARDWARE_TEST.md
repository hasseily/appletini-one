# ONE//e Hardware Test Record and F0.9.81 Retest Plan

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
  physical Slot 6 does not rewrite ONE//e's virtual target. The menu now keeps
  that Disk II choice instead of changing it back to SmartPort.
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

The source correction, focused regression, full Vivado and Vitis build, and
package are complete. The hardware retest result is recorded below.

## F0.9.79 Test Image

- Source and packaging commit:
  `a94aefc27353f8ccdefe7b9dbf012d343a9f1564`
- Clean full build: `20260816T180541Z-a94aefc2-full`
- Route: WNS `+0.103 ns`, TNS `0`, WHS `+0.019 ns`, THS `0`, and WPWS
  `+0.265 ns`
- Failures: no failing endpoints, route errors, missing constraint objects, or
  unconstrained internal endpoints
- Bitstream SHA-256:
  `a03e90819fee064d4163572925392d18e9a05e47c8426f32b6258d2dd03afea8`
- XSA SHA-256:
  `0963dc9999f253dac21dd69d3af5a20c7e88008e613479497a9762fc87dcedce`
- Files: root `FIRMWARE.BIN` and
  `.timing_runs/20260816T180541Z-a94aefc2-full/FIRMWARE_TEST.BIN`
- Size: `4,109,260` bytes each
- Firmware SHA-256:
  `3d4c6d9797eac2ebfae74d110963077a91897175f6c19cc21b56a9f69d567941`
- Handoff record:
  `.timing_runs/20260816T180541Z-a94aefc2-full/test_firmware_manifest.txt`

The two firmware files are byte-identical. A fresh Vitis workspace used the
exact archived XSA; platform export and all application builds report
`SUCCESS`. Bootgen readback passed with `total_images=3` and
`total_partitions=3`. The frontend contains the `196684`-byte CPU1 blob.

This image is below the project's release-only +0.300 ns setup-margin gate.
The user waived that gate for this test image; this is not a promoted release.

## F0.9.79 Functional Retest Result

The user ran F0.9.79 out of the Apple slot. ONE//e booted, but the test found
three more faults:

- A TransWarp speed selected with a bound key during the boot-menu/takeover
  window did not reach the core. ONE//e started at full acceleration. After a
  `Ctrl+Alt+Pause` virtual warm reset, it again ran at full acceleration. A
  speed change made later on the config page did take effect.
- A brief key tap could repeat many times. In the Appletini demo menu, one
  Down tap could scroll to the last choice.
- The new DHGRi image mode appeared as HGRi. The other new image modes worked.

The speed fault extended the state-boundary error fixed in F0.9.79. With saved
host TransWarp off, a key used in the open menu before ONE//e selection, or
after the ONE//e request but before `g_onee_running` was set, still failed the
old live-core gate. The requested rung was discarded, so the first core-release
word kept full speed. The private warm reset did not choose a new speed; it
exposed the speed word which the session had retained.

Key repeat used 1,000 calls to `onee_input_service_poll()` for its first delay
and 100 calls for its steady rate. The USB service can call that function
twice in each fast frontend pass. Those call counts were not human-time delays
and could expire before a short tap's release report was handled.

The DHGRi fault was in the demo viewer, not the renderer or ONE//e switch
model. `select_dhgr` set 80COL and accessed `$C05E`, but it did not write
`$C07E` first. On an Enhanced //e, IOUDIS routes `$C05E/$C05F` to DHIRES;
with IOUDIS off those addresses control AN3. DHIRES therefore stayed off and
the renderer correctly saw HGRi.

## F0.9.80 Source Corrections

F0.9.80 is a historical test image. A timing-clean build of the speed, repeat,
and DHGRi fixes finished before the user added the latched-selection
requirement.
That baseline was intentionally not packaged. The final source checks, full
build, exact-XSA Vitis build, and test package are now complete at `cd8e9b8`.
The board retest ran and its result is recorded below.

- Speed commits: `147e56d23be402a4feed68e0719d41636a74e3e4` and
  `dde9b204e426d28ee4a11586f68af2e8b4c8271c`. A speed action is now
  allowed while host TransWarp has saved intent, while ONE//e is requested,
  effective, or running, or for a key event from an explicitly open Appletini
  menu. If no core is live, the action queues the rung and reports `TW NEXT`.
  This lets a menu-open key stage the upcoming ONE//e start even when saved
  host TransWarp is off. The input handler passes menu context for that event
  only; the same key with the menu closed still reports `TW: OFF` and queues
  nothing. Each of the three ONE//e start control words uses the effective
  speed and divider. The same word remains in `VTW_CTRL` across private warm
  reset. Stopping the session clears the temporary override, so a later
  session returns to its saved configured speed.
- Key-repeat commit:
  `3e978a10fa5cf975a69fa1d44cd4c983349901ee`. Repeat now uses the
  CherryUSB millisecond clock: 500 ms before the first repeat and 100 ms
  between later repeats. A late poll emits one event and starts a new interval
  from the current time, so it cannot fill the queue to catch up. The native
  test covers a short Down tap, exact deadlines, a long stall, release and
  disconnect reselection, session stop, and 32-bit time wrap.
- DHGRi commit:
  `9cbf3e0dfdf4cfeb33b09c88a4e739959ddae82c`. The assembled viewer now
  writes `$C07E` before it selects DHIRES, and the demo disk contains that
  rebuilt viewer. Its regression executes the assembled HGRi and DHGRi mode
  setters against Enhanced //e switches and requires `HGRi != DHGRi`.
- Selection-latch commit:
  `36be6c5cf8f22796a986548482f994f48818e7e1`. A manual ONE//e selection
  has no software expiry. Stable quiet, including a long wait for effective
  state, does not clear it. A failed private-runtime start or a released core
  which stops suspends vTW and retries inside the same selected session while
  preserving the exact queued or live speed override. Watched Apple activity,
  a lost PL request echo, missing or invalid safety logic, and manual OFF are
  terminal stops: they stop vTW, clear session-only speed state, and require a
  fresh manual selection. Later quiet time never restarts a terminally stopped
  session.

Every full card and PS boot starts ONE//e in `OFF` and writes its request low.
ONE//e is not saved in config or profiles. A saved automatic start is not safe
without a nonvolatile hardware activity latch: a software restart cannot know
that a connected Apple was active before the card or PS restarted.

## F0.9.80 Pre-Latch Baseline Build

- Source commit: `e7fe40a4f3903331c2701c9cb2d5fc811b856882`
- Clean full build: `20260816T185749Z-e7fe40a4-full`
- Route: WNS `+0.103 ns`, TNS `0`, and WHS `+0.019 ns`
- Build result: timing-clean and exported
- Package result: intentionally not built

The user added the latched-selection requirement after this build began. Its
bitstream and XSA do not contain checkpoint `36be6c5`, so no F0.9.80 firmware
package was made from them. The final `36be6c5` plus documentation build
is recorded below.

## F0.9.80 Final Test Image

- Source commit used for the package:
  `cd8e9b8eb6eacd7f4dabdfdcd04e84af292feb99`
- Clean full build: `20260816T193636Z-cd8e9b8e-full`
- Build status: full, clean, and exported
- Route: WNS `+0.104 ns`, TNS `0`, WHS `+0.037 ns`, and WPWS `+0.265 ns`
- Failures: zero setup, hold, or pulse-width failing endpoints; zero route
  errors, missing constraint objects, or unconstrained internal endpoints
- Bus skew: `PASS`, WNS `+5.957 ns`
- Candidate DCP SHA-256:
  `0c57a120686aa2536b34def9453aaaba195eece6ab6dd9d75eccd8451a7db6af`
- Bitstream SHA-256:
  `d4c2fd7c78db7ba52763b5dc9c5612fdde8f6ae52ff035e0cabf1c6ed418dab3`
- XSA SHA-256:
  `5d899fb3d018c121fa19b1127f8ed39c6827e76af8cc10276e3de056e3c14435`
- Files: root `FIRMWARE.BIN` and
  `.timing_runs/20260816T193636Z-cd8e9b8e-full/FIRMWARE_TEST.BIN`
- Size: `4,257,036` bytes each
- Firmware SHA-256:
  `43a8eb8dca2816d065dbfbe0bde77ab6fac79b938a4d5321e51c819cdd771f3a`
- Handoff record:
  `.timing_runs/20260816T193636Z-cd8e9b8e-full/test_firmware_manifest.txt`

A fresh Vitis workspace used the exact archived XSA. Platform export and all
application builds report `SUCCESS`; the built strings are F0.9.80 and
B1.1.0. Root/archive `fc /b` passed. Bootgen readback passed with three images
and three partitions.

This package uses the user's test-only waiver for the +0.300 ns setup-margin
gate. WNS is positive, but the image is not a promoted release. Programming,
the out-of-slot functional retest, and the Apple-connected electrical tests
remain open.

## Detection Limit

Slot +5 V cannot identify an Apple in this stand-alone setup because that same
pin supplies the card when it is out of the computer. The design therefore
uses the Apple bus clocks, not slot +5 V, to detect a powered and running host.
PHI0, 7M, or Q3 will change within the sub-microsecond guard window during
normal Apple operation. The XDC gives all six watched U533 inputs—PHI0, 7M,
Q3, M2SEL, M2B0, and DEVSEL#—the same 5 ns pad-to-first-flop route bound.

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

## F0.9.79 Result

- [x] Pass the final cold-target, joined-bus, real-ROM, effective Disk II,
  SuperSprite override, storage-selection, and live-speed-control tests.
- [x] Complete a full Vivado route and export with no failing path.
- [x] Build Vitis from the exact exported XSA.
- [x] Package F0.9.79, record its source commit, size, SHA-256, timing result,
  and Bootgen readback, and compare the root and archived files byte for byte.
- [x] Program and run F0.9.79 out of the Apple slot. It exposed the speed,
  repeat, and DHGRi faults above. The Apple-connected electrical checks remain
  pending.

Root `FIRMWARE.BIN` is now the checked F0.9.80 test file recorded above. Keep
the golden boot image unchanged.

## F0.9.80 Readiness

- [x] Correct and test menu-open and requested pre-takeover speed staging,
  first-release speed, warm-reset retention, and closed-menu true-off behavior
  at commits `147e56d23be402a4feed68e0719d41636a74e3e4` and
  `dde9b204e426d28ee4a11586f68af2e8b4c8271c`.
- [x] Correct and test elapsed key-repeat timing at commit
  `3e978a10fa5cf975a69fa1d44cd4c983349901ee`.
- [x] Correct and test the demo viewer's IOUDIS/DHGRi selection at commit
  `9cbf3e0dfdf4cfeb33b09c88a4e739959ddae82c`.
- [x] Remove the selected-state software expiry and test long stable quiet at
  commit `36be6c5cf8f22796a986548482f994f48818e7e1`.
- [x] Test recoverable private-runtime suspend/retry with the exact queued or
  live speed override retained.
- [x] Test terminal stop and fresh manual reselect for watched Apple activity,
  lost request echo, missing safety logic, and manual OFF. Test that long later
  quiet never restarts the mode.
- [x] Bind PHI0, 7M, Q3, M2SEL, M2B0, and DEVSEL# to the 5 ns raw-input route
  constraint.
- [x] Confirm that every full card and PS boot writes request low and starts
  `OFF`, with no config or profile auto-start.
- [x] Complete timing-clean pre-latch build
  `20260816T185749Z-e7fe40a4-full` at `e7fe40a4`; do not package it because it
  predates the latch requirement.
- [x] Complete a clean full Vivado route and export for final F0.9.80 source
  checkpoint `cd8e9b8eb6eacd7f4dabdfdcd04e84af292feb99`.
- [x] Build Vitis against that exact XSA; platform export and all applications
  report `SUCCESS`, with F0.9.80 and B1.1.0 strings.
- [x] Package F0.9.80, record its source commit, build, timing, size, hashes,
  and Bootgen readback, then compare root and archived files byte for byte.
- [ ] Program F0.9.80 and complete the out-of-slot retest below.

## Program the F0.9.80 Test Slot

The checked F0.9.80 test file is root `FIRMWARE.BIN`: 4,257,036 bytes, SHA-256
`43a8eb8dca2816d065dbfbe0bde77ab6fac79b938a4d5321e51c819cdd771f3a`.
Programming remains pending:

1. Connect the card's update UART and its stand-alone power source.
2. Find the port if needed:

   ```powershell
   python scripts\serial_firmware_update.py --list-ports
   ```

3. Check that root `FIRMWARE.BIN` matches the recorded F0.9.80 size and
   SHA-256, then upload it, replacing `COM3` with the update UART port:

   ```powershell
   python scripts\serial_firmware_update.py .\FIRMWARE.BIN --port COM3 --reboot-golden
   ```

The updater writes the firmware slot and checks the full programmed image.

## F0.9.80 Out-of-Slot Retest

This hardware retest is still pending.

Connect DVI, audio, a USB keyboard, and the SD card. A USB gamepad is optional.
Keep the Apple slot edge disconnected and apply the same stand-alone supply
used for the F0.9.77 test.

1. Power the card and confirm the on-screen firmware version is `F0.9.80`.
   Before any menu action, confirm that ONE//e says `OFF`. Perform a full card
   power cycle and a PS reboot; after each, confirm it returns to `OFF`. No
   saved setting or profile may select it.
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
8. Set a divided configured speed, start ONE//e without a speed key, and time
   a fixed loop. The first running core must use that configured speed, not
   full acceleration.
9. Stop ONE//e and leave saved host TransWarp off. Close the Appletini menu and
   press a bound speed key; it must report `TW: OFF` and queue nothing. Open the
   menu, press the same key before selecting ONE//e, and require `TW NEXT`.
   Select ONE//e and confirm that its first running core uses that queued speed.
   Repeat with the toggle, speed-up, speed-down, and armed slug actions. Also
   use a key after the ONE//e request but before core takeover finishes and
   confirm that it stages the first running speed.
10. While ONE//e runs at a non-full speed, use `Ctrl+Alt+Pause`. Confirm that
    only the virtual //e resets and that the timed rate stays the same after
    reboot. Stop and start a new ONE//e session; it must return to the saved
    configured speed rather than retain the prior session override.
11. In the Appletini demo menu, tap Down once. The choice must move down one
    row only. Hold Down: the first repeat should start near 500 ms and later
    repeats should occur near 100 ms apart. Release it and confirm that repeat
    stops at once.
12. In the demo image viewer, show both HGRi and DHGRi samples. They must select
    distinct modes, and DHGRi must show double-hi-res pixels rather than HGRi.
    Recheck the other new image modes for regression.
13. Leave a selected ONE//e running for a long stable quiet interval and
    confirm that it stays selected and running. A safe request which is still
    settling must not time out. Turn ONE//e off, wait for the stable connector
    vector to pass the 96-cycle quiet check, and select it again.

A request never restarts itself after watched Apple activity, a lost request,
missing safety logic, or manual OFF. Long later quiet must leave it off until a
fresh manual menu selection.

## Functional Checks

Run these checks before any Apple-connected safety test:

- Boot DOS and ProDOS to their ready screen.
- Use a standard full-size USB HID keyboard. Type letters, numbers,
  punctuation, arrows, Return, Escape, Delete, and numeric keypad keys.
- Check that a brief tap emits one key only. Hold a key and check the 500 ms
  first delay, 100 ms steady repeat, and prompt stop on release.
- Check Open Apple with left Alt or GUI, and Closed Apple with right Alt or
  GUI.
- Confirm the fixed US layout. A boot-protocol keyboard has six-key rollover
  plus modifiers; a parsed report keyboard tracks at most eight active key
  usages. Lock LEDs do not track ONE//e, and exotic NKRO reports are not
  guaranteed.
- Check a paddle or joystick program with all axes and the first three
  buttons.
- Use `Ctrl+Alt+Pause` and confirm that only the virtual //e resets and the
  current session speed remains unchanged.
- Run software which toggles the Apple speaker; check both audio channels,
  mute, and mix with card audio.
- Check 40/80-column text, lores, hires, double-hires, HGRi, DHGRi, and mixed
  video. HGRi and DHGRi must produce distinct switch states and images.
- Reopen the menu, turn ONE//e off, and confirm the state returns to `OFF`.
- Leave it off through a long quiet interval and confirm it does not restart.
  Select it manually and confirm that it can start a new session.

Record the UART log, disk image name, USB device names, DVI mode, and card
control register `0x5B` for each failure.

## Apple-Connected Test Gate

Only move on after checking the connector with test gear. Prove that every
Apple-side driver stays high impedance while ONE//e starts, runs, stops, and
loses PL configuration. Confirm that the main translators remain input-only
and U234 stays disabled. Cover both power orders and an Apple which starts its
clocks while ONE//e runs.

PHI0, 7M, or Q3 activity must stop ONE//e at once, keep the connector isolated,
and require a fresh manual selection. Keep the Apple quiet afterward long
enough to prove that quiet alone cannot restart ONE//e. Also test the stated
limit with a clock-stopped Apple before approving use while the card remains
installed.

## F0.9.80 Functional Retest Result

The user confirmed the earlier boot-target, storage-label, speed, key-repeat,
and DHGRi mode-selection fixes. The test then found three remaining issues:

- Manual ONE//e ON returned to OFF after a card power cycle.
- DHGRi could show stray lines, and changing back could leave the prior image
  corrupt. The AUX RAM test reported zero errors.
- ONE//e needed fixed USB controls and full menu input ownership, with the soft
  Apple paused while the config menu was open.

The video fault came from frame delivery, not the tested AUX RAM. DVI scanout
could clear its accounting while accepted old AXI reads were still in flight;
the reader could claim a slot as the writer reclaimed it; and repeated full
DHGRi shadow rebuilds could starve the CPU1 record drain.

## F0.9.81 Corrections

- `e076280` drains accepted old AXI traffic before DVI restart, reserves then
  verifies the displayed triple-buffer slot, and caches unchanged full-shadow
  interlaced modes. TEXT and MIXED stay uncached. Gap, underrun, and AXI
  counters support the board check.
- `9feaccd` saves manual ON globally before raising the PL request and restores
  it only after boot safety checks. Apple activity or a lost request forces
  OFF and queues a synced, backed-up global write. Profiles cannot change the
  latch.
- ONE//e fixed controls bypass saved bindings: Page Up/Page Down switch tabs;
  arrows move; Enter or keypad Enter selects; Escape closes; Print Screen
  captures the Apple screen; Shift+Print Screen captures 1080p; keypad `+` and
  `-` step speed; and keypad `0` toggles acceleration.
- Opening the config menu blocks keyboard and joystick delivery and parks the
  65C02 after its current bus access, without reset or a speed change. DVI
  remains live. Closing waits for neutral keys, modifiers, buttons, hats, and
  axes before resuming.
- `b33b631` reserves F0.9.81.

Persistence has one hardware limit. If all card power disappears after PL
detects Apple activity but before the PS records OFF on the SD card, firmware
cannot preserve that event. If the Apple remains active at the next boot, PL
still blocks ONE//e and firmware then saves OFF. Closing this window requires
a nonvolatile hardware event latch.

## F0.9.81 Test Image

- Source commit: `b33b63176d758ac25d582bdfbdd62717a17484ba`
- Clean full nonincremental build: `20260816T210350Z-b33b6317-full`
- Build status: exported; `rescue_used=0`
- Route: WNS `+0.194 ns`, TNS `0`, WHS `+0.061 ns`, THS `0`, and WPWS
  `+0.265 ns`
- Route and bus skew: `PASS`; bus-skew WNS `+5.657 ns`
- Failures: zero setup, hold, or pulse-width failing endpoints; zero route
  errors, missing constraint objects, or unconstrained internal endpoints
- Candidate DCP SHA-256:
  `dc13f81019146784367f33d44c47e0fe0a2e054159c36770411c9992bb0b38e7`
- Bitstream SHA-256:
  `2e0e2a948aa647f1e13d4e2774f4d33db31ee239587920a0b50d7d4ef22bea42`
- XSA SHA-256:
  `601d18dc1ec0cef12619cf8b0262d41cb813af52ccdecc20db07759f4101275c`
- Files: root `FIRMWARE.BIN` and
  `.timing_runs/20260816T210350Z-b33b6317-full/FIRMWARE_TEST.BIN`
- Size: `4,241,804` bytes each
- Firmware SHA-256:
  `e5a6fa0440b9eee92a1bdc59ad8f90bd5ddb53bc97f88cbea5b57fde47f94964`
- Handoff record:
  `.timing_runs/20260816T210350Z-b33b6317-full/test_firmware_manifest.txt`

A fresh Vitis build used the exact archived XSA. Platform export and all app
builds report `SUCCESS`; the image contains F0.9.81, B1.1.0, and the
`196684`-byte CPU1 blob. Root and archive files are byte-identical. Bootgen
found three named image headers but four total images and four partitions
because `frontend.elf` contributes two load partitions.

WNS is positive but below the project's +0.300 ns promotion gate. The user
waived that gate for this test image only. Programming and hardware retest are
open; this is not a promoted release.

## F0.9.81 Focused Retest

1. Program root `FIRMWARE.BIN` and confirm F0.9.81 on screen.
2. Out of slot, select ONE//e ON, power-cycle the whole card, and confirm it
   returns to RUNNING. Select OFF, power-cycle, and confirm it stays OFF.
3. Run DHGRi and switch repeatedly among interlaced and prior images. Require
   no stray lines and no new capture-gap, scanout-underrun, or AXI-error count.
4. Confirm the fixed Page Up/Page Down, arrow, Enter, Escape, Print Screen,
   Shift+Print Screen, keypad `+`, keypad `-`, and keypad `0` controls. Saved
   USB bindings must not alter them in ONE//e.
5. Open the config menu while code runs. Confirm the Apple CPU stops, no menu
   key reaches Apple input, all menu controls work, and close resumes the same
   state and speed only after input returns to neutral.
6. Recheck SmartPort and Disk II boots, storage labels, warm-reset speed,
   single-tap repeat, paddles, speaker, and normal host mode.
7. Complete the Apple-connected high-impedance and live-clock stop tests. An
   Apple event must force OFF and leave it OFF across later card power cycles
   until manual reselect.
