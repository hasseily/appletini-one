# ONE//e Stand-Alone Mode Plan and Implementation Record

Branch: `feature/self-contained-one-iie`

Baseline: `a4c2d22fc6029263f61a5cfe1178a2ab3c6ef762`

Last source audit: 2026-08-16

## Status Rules

- `[x]` means the named source path exists and the listed source check or
  simulation has passed.
- `[ ]` means the work or proof is still missing.
- An RTL simulation or a clean build does not count as board proof.

## Goal and Supported Shape

ONE//e is a session-only, self-contained 128K Enhanced US Apple //e. It runs
the existing soft 65C02/TransWarp core and uses Appletini's DVI output, USB
host, virtual cards, and audio output. It does not use a host Apple CPU,
motherboard I/O, or a physical Disk II controller.

The source implementation now has these parts:

- A fail-off supervisor which selects and isolates the virtual machine.
- A free-running virtual Apple bus with the same `AppleBus_read` and
  `AppleBus_write` records used by the physical slot path.
- Built-in //e keyboard, status, video switch, annunciator, paddle, cassette
  latch, utility-strobe, and speaker soft switches.
- A direct Enhanced //e ROM cold-start path for the soft 65C02.
- USB keyboard, Open Apple, Closed Apple, joystick, paddle, and warm-reset
  input paths.
- The existing renderer, virtual Disk II, SmartPort, card arbiter, and audio
  mixer on the virtual bus.

The source and simulation work is not a hardware safety sign-off. The final
sections list the board and system tests which still need to run.

### Implementation Checkpoints

- `44dc002`: safety-guard and virtual-bus scaffold plus the first plan.
- `2489298`: top-level safety interlock and physical output isolation.
- `ca87e16`: built-in motherboard I/O and speaker sample blocks.
- `8fec225`: joined virtual motherboard, cold slot scan, Disk II, SmartPort,
  card arbiter, floating-bus, and vTW integration.
- `4db50f9`: PS-to-PL keyboard, Apple key, joystick, paddle, and reset bridge.
- `22b38a8`: boot-menu action, cold vTW runtime, and USB input services.
- `1718b70`: input bridge, virtual warm reset, and speaker mixer top hookup.
- `53e2f0e`: NTSC renderer-input and real-ROM cold-slot simulations.
- `cd617e6`: held-key repeat with multi-key and multi-device selection.
- `d57ffb2`: fail-fast checks for the Vitis platform export result and XPFM.
- `ea39ff0`: reserve firmware version `F0.9.77` for this branch.
- `72d299f`: continuously veto held non-idle Apple-side levels.
- `b3d8391`: run the stock DOS 3.3 and ProDOS 2.4.3 boot sectors through the
  production virtual Disk II path.
- `0a155fa`: contain the immediate raw-input kill in one run-state flop so raw
  Apple pins do not feed the full virtual-card data path.
- `8550239`: shorten the unchanged ONE//e speaker high-pass calculation for
  the 133 MHz timing path.
- `9f3c259`: resolve and hold each virtual bus owner/address/R/W tuple before
  the slot address phase.
- `9a5deb8`: sample raw activity diagnostics before the AXI readback mux and
  keep the remaining immediate safety path within sticky state.
- `530b29e`: shorten the equivalent raw/synchronized activity detector without
  dropping edge or held-level coverage.
- `4a2d4d6`: retry one transient Vitis platform build, then keep the status and
  exported-XPFM checks ahead of all application creation.
- `a6cd13e`: preserve the CPU decimal-result timing stage and use exact
  sign-extension checks in the unchanged speaker clamp.
- `ad0f988`: carry validated ONE//e scanner-read state from `serve_en` to
  `data_en` instead of repeating the address decode on the BRAM enable path.
- `354cc9a`: use the existing east-side AXI write-data copy for vTW sync and
  posted-write commands, with the same register values and strobes.

## Safety Contract

Safety has priority over keeping ONE//e running.

The supervisor in `hdl/apple/onee_mode_safety_guard.sv` owns three separate
states:

1. `onee_request_q`: the user's current manual request.
2. `onee_activity_lockout`: sticky evidence of Apple-side activity.
3. `onee_enable_effective`: the only signal which may run the virtual machine.

The request itself asserts `physical_bus_isolate`. The supervisor does not set
`onee_enable_effective` until every monitored input matches its reset idle level,
the guard has seen no transition for its quiet interval, and a new selection is
armed. This orders physical isolation before soft-CPU start. A stopped-high
clock or held-low active control remains a continuous veto; it cannot age into
the quiet state. The current guard interval is 2,048 clocks in the 133.333 MHz
fabric domain, about 15.4 microseconds.

The guard watches raw PHI0, 7M, Q3, M2SEL, M2B0, DEVSEL#, RESET#, INH#, IRQ#,
NMI#, RDY#, and DMA# inputs. A raw transition or non-idle level asynchronously
sets the sticky activity lockout; that state asynchronously clears one
contained run-state flop and sets the physical-isolation hold. This path does
not wait for firmware or for the two-flop status synchronizer. The run-state
flop's Q drives the high-fanout virtual machine selection. Software's live
activity and inhibit fields use a one-clock sampled copy, not the raw pins.

After an activity stop:

- Effective mode drops at once.
- Firmware clears the session request and stops vTW.
- The request is never raised again by polling, startup, config load, or a
  profile.
- Quiet time alone cannot restart the machine.
- The user must select the boot-menu item again after the request has gone off,
  every monitored input has returned to its reset idle level, and the connector
  has shown no transition for the quiet interval.

While isolation is high, `apple_bus_wrapper` blocks address/R/W drive, data
drive, IRQ#, INH#, DMA#, the address and data directions, and the main bus
transceiver. `apple_top` sends an all-zero physical `AppleBus_write` record and
releases the separate physical RESET output. The virtual warm-reset path does
not connect to the physical RESET path.

### Board Limit: No Slot-Power Sense

This board revision has no physical Apple-slot power-sense input. The top level
ties `apple_power_present_raw` low and reports both the power-sense-present and
Apple-power bits as zero in card-control register `0x5B`.

The activity detector catches clock or control transitions and continuously
vetoes any monitored level which differs from the reset idle vector. It still
cannot identify a powered host whose monitored slot pins all match the same
idle vector as an unpowered host. The output-side design isolates the connector
before ONE//e starts, but simulation cannot prove translator behavior, leakage,
back-power, or board high impedance in that case. Until the board tests below
pass, do not treat ONE//e as safe for use while it remains plugged into an
Apple.

## Implemented Architecture

### Virtual Bus and Slot Pins

`hdl/apple/apple_virtual_bus.sv` generates the native phase contract:
`drive_en`, `addr_en`, `sss_en`, `serve_en`, and `data_en`. Idle cycles keep the
1 MHz card and scanner cadence running. The chosen bus record feeds the same
soft-switch manager, capture path, and virtual card modules used in host mode.
After `drive_en`, the bus samples the active owner/address/R/W tuple across a
window which includes the vTW posted-write pipeline, then holds that tuple from
`addr_en` through `data_en`. Slot decoders therefore do not use a live
high-fanout owner mux during the visible cycle.

`apple_top` keeps `physical_ab_read` and `virtual_ab_read` separate. Effective
ONE//e selects the virtual record. All card replies enter the existing
`apple_bus_write_arbiter`; its merged result returns to the virtual bus and the
soft CPU. The physical wrapper receives a masked zero record while isolation
is high.

The shared records provide slot device select, slot ROM, expansion ROM,
interrupt, inhibit, ready, reset, and bus-data behavior without a private
CPU-to-card interface. The vTW core uses the scanner address and main shadow RAM
for unclaimed floating reads. For status reads, it combines the claimed bit 7
with scanner bits 6:0.

`hdl/apple/onee_cold_slot_scan.sv` hides slot 7 at each ONE//e entry. The first
`$C600` ROM probe ends the hold, so the cold scan reaches virtual Disk II in
slot 6 first and slot 7 cards become visible after that probe.

The Appli-Card/AD8088 bus-master path stays blocked for the whole ONE//e
session. Virtual-card IRQ and NMI requests remain internal; physical DMA is not
used.

### Built-In Motherboard I/O

`hdl/apple/onee_motherboard_io.sv` implements the following shared-bus
behavior:

- `$C000`: keyboard code and strobe.
- `$C010`: any-key state and keyboard-strobe clear. Writes anywhere in
  `$C010-$C01F` also clear the strobe.
- `$C011-$C01F`: non-destructive //e MMU/video status reads, with scanner low
  bits.
- `$C020-$C02F`: cassette-output latch toggle.
- `$C030-$C03F`: speaker toggle.
- `$C040-$C04F`: utility-strobe pulse mirrors.
- `$C050-$C057`: existing video soft switches.
- `$C058-$C05D`: AN0-AN2 when IOUDIS is off.
- `$C05E/$C05F`: AN3 when IOUDIS is off and DHIRES when it is on.
- `$C060-$C067` and mirrors `$C068-$C06F`: cassette input, Open Apple, Closed
  Apple, PB2, and PDL0-PDL3 status.
- `$C070-$C07F`: paddle-trigger mirrors, plus write-only IOUDIS control at
  `$C07E/$C07F`, read status at the same addresses, and coexistence with the
  existing RamWorks `$C071/$C073` bank writes.
- Unclaimed reads: scanner/floating-bus data supplied by the vTW read path.

Paddle values are snapped on each `$C07x` access. Their counters expire on
native Apple cycles, so TransWarp speed does not shorten the software-visible
paddle time.

This models the base Enhanced //e utility strobe. It does not invent a
motherboard VBL-interrupt enable which the base //e does not have.

### CPU, Memory, Menu, and Reset

The Boot Settings page has a session-only row 2 named `ONE//e standalone`,
before the USB binding rows. It reports `OFF`, `RUNNING`, or `LOCKED`. It does
not enter config files, saved settings, or profiles.

The item help is:

> Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an
> Apple host. This mode is session-only and starts off after every card boot.
> Any Apple-bus activity stops ONE//e and keeps it off. After the connector is
> quiet, select this item again.

The sole high write to the ONE//e request register comes from an explicit menu
selection. A request waits for PL effective state with a bounded timeout. The
menu closes only when the vTW status confirms both effective enable and a
released core; the user can reopen it to stop the session.

`vtw_service_onee_start()` then:

1. Checks request, effective state, isolation, selection, signature, and
   inhibit reason.
2. Enables the Disk II service for this session without changing the saved
   slot mask.
3. Holds only the virtual machine in reset.
4. Clears shadow `$03F3/$03F4` to force a cold start.
5. Loads the fixed 16K Enhanced //e ROM into the existing ROM shadow, checking
   isolation on every byte.
6. Forces the private vTW Disk II shortcut off.
7. Releases virtual reset and the soft core.

The ONE//e start path does not wait for host-machine detection, physical DMA,
physical RESET, or the normal slot-7 host handoff. It keeps the saved host-vTW
intent, speed, divider, and compatibility options unchanged. Stopping writes a
literal zero to vTW control before it clears the ONE//e request and restores
the saved Disk II service state.

Ctrl+Pause requests a virtual warm reset. The PL holds virtual RESET for at
least eight full native cycles and acknowledges the input bridge. This resets
the soft CPU and virtual motherboard I/O but preserves shadow RAM. It never
asserts the Apple connector's RESET signal.

### USB Input

`onee_input_service` receives both boot-keyboard and parsed report-keyboard HID
reports. It translates letters, Shift/Caps Lock, Ctrl-A through Ctrl-Z,
numbers, punctuation, Return, Escape, Tab, Space, arrows, Delete, and keypad
keys to 7-bit Apple codes. Left Alt or GUI maps to Open Apple; right Alt or GUI
maps to Closed Apple. These Apple keys also feed PB0 and PB1.

The service recognizes report-protocol devices with at least two absolute
axes as joysticks. It normalizes the HID logical range to 0-255 and maps:

- X to PDL0.
- Y to PDL1.
- Rx, with Z fallback, to PDL2.
- Ry, with Rz fallback, to PDL3.
- The first three buttons to PB0-PB2.

The lowest active HID slot owns the joystick. Disconnect releases its buttons
and returns all paddles to `0x80` neutral.

Firmware has a 32-entry key queue in front of the PL's eight-entry FIFO. It
checks `RUNNING`, effective mode, bridge enable, and the `0xE1` bridge
signature before any write. Dropping effective mode masks the PL outputs at
once and clears all queued, live, paddle, and reset state on the next clock.

Mapped keys emit once on their press edge, then repeat after 1,000 active input
service polls and every 100 polls after that. The newest held mapped key across
all HID devices owns repeat. Releasing or disconnecting it selects the newest
remaining held key. Repeat uses the current Shift, Control, and Caps Lock state.
Caps Lock, Pause, modifier-only reports, and unmapped keys do not repeat. A
session stop clears all repeat state.

Caps Lock state exists only in the ONE//e key translator. Firmware does not
send a USB HID output report for it, so a keyboard's Caps Lock LED may not show
the current ONE//e state. This is an input-device limit, not a start or safety
blocker.

### Storage and Other Cards

In ONE//e mode, slot 6 always selects `disk2_card`, even if the saved slot-6
setting is off. Firmware also session-enables its track service. The vTW
private Disk II shortcut is forced off, so slot ROM, `$C0E0-$C0EF` soft
switches, sequencer, motor, write, WOZ timing, and drive-sound state all use
the shared synthetic slot bus. ONE//e has no path to a physical Disk II card,
cable, or drive.

SmartPort appears in virtual slot 7 after the cold `$C600` probe. Its private
vTW shortcut is disabled in ONE//e, so its slot ROM, `$C800` window, control,
data, and interrupt behavior use the same synthetic slot bus. Other enabled
virtual cards also see the selected bus record. Cards still need their own
compatibility tests; sharing the record does not prove every card or external
chip works without a host board.

### Video and Speaker

The virtual bus drives the existing native timing generator and normal cycle
capture path. The vTW core posts screen writes through the same capture queue
used by host mode. The focused video bench covers all 65 cycles across all 262
NTSC lines, starts VBL at line 192 cycle 0, checks frame wrap and frame-zero
reset, checks `$C050/$C057` state in renderer frame metadata, and checks that
posted `$0400/$2000` writes reach renderer-input records.

Each `$C03x` access toggles the motherboard speaker. `onee_speaker_audio.sv`
turns that state into signed, DC-blocked, saturated 16-bit mono samples. The
board top mixes the sample into both left and right channels after the existing
Mockingboard plus Disk II mix, using the existing saturating adder and output
volume/mute path. Effective-mode loss masks the ONE//e sample to zero.

The cassette-output latch exists for software compatibility, and cassette
input reads low. There is no analog cassette input or output path in this
scope. Annunciator state and the utility-strobe pulse also have no external
game-port output in this scope.

## Card-Control Register Contract

All offsets are in the existing card-control register bank.

| Offset | Write contract | Read contract |
| --- | --- | --- |
| `0x5B` | Bit 0 is the manual session request. Only the explicit Boot Settings action may write it high. | Bit 0 request; 1 effective; 2 physical isolation; 3 outputs forced off; 4 live activity; 5 sticky activity lockout; 6 quiet; 7 reselect armed; 8 selected; 9 isolation hold; 12:10 inhibit reason; 13 power-sense pin present; 14 Apple power sensed; 15 HDL present; 31:24 signature `0xE1`. |
| `0x5C` | Bits 6:0 enqueue one Apple key code. | Bits 6:0 FIFO head; 7 valid; 11:8 count; 12 full; 13 empty; 14 sticky overflow. |
| `0x5D` | Bit 0 any key; 1 Open Apple; 2 Closed Apple; 5:3 joystick PB0-PB2. | The saved live-input word. Open/Closed Apple are also ORed into effective PB0/PB1. |
| `0x5E` | Bytes 0-3 are PDL0-PDL3. | The four saved paddle bytes; neutral is `0x80808080`. |
| `0x5F` | Bit 0 request warm reset; 1 clear FIFO overflow; 2 flush key FIFO; 3 release live inputs and recenter paddles. | Bit 0 reset request; 1 overflow; 2 empty; 3 full; 7:4 FIFO count; 8 reset acknowledge; 9 bridge enabled; 31:24 signature `0xE1`. |

The `0x5B` inhibit values are 0 none, 1 reset, 2 Apple power, 3 live Apple
activity, 4 activity lockout, 5 manual reselect required, and 6 manual off.

The input bridge returns zero and exposes no consumer output when effective
mode is off.

## Delivery Checklist

### Safety and Control

- [x] Default the mode and firmware session request off.
- [x] Isolate physical outputs before effective mode can start.
- [x] Kill effective mode on raw Apple-side transitions without firmware.
- [x] Keep a stopped-high clock or asserted active-low control from becoming
  quiet until it returns to the reset idle level.
- [x] Latch activity and require a later manual selection.
- [x] Keep startup, profile, apply, and polling paths from raising request.
- [x] Mask physical address, R/W, data, IRQ, INH, DMA, transceiver, and RESET
  outputs in RTL.
- [ ] Prove connector pin voltage, high impedance, leakage, and back-power on
  a production board.
- [ ] Prove external translator OE/DIR defaults while the PL is unconfigured,
  configuring, or held in reset, under both Apple-first and card-first power
  order.
- [ ] Test an Apple already on and an Apple powered on during ONE//e.
- [x] Document the missing slot-power input and the undetectable powered host
  whose monitored pins all match the reset idle vector.
- [ ] Add a board-level power interlock or power-sense input if ONE//e must
  reject that all-idle powered-host case.

### Virtual Motherboard and Boot

- [x] Generate native virtual bus phases and scanner cadence.
- [x] Return card-arbiter data and local IRQ/NMI/RDY/INH behavior to the soft
  CPU without driving physical pins.
- [x] Implement keyboard, status, video, annunciator, paddle, cassette-latch,
  utility-strobe, and speaker soft switches.
- [x] Add the session-only boot-menu action.
- [x] Auto-start vTW through a stand-alone cold-ROM path.
- [x] Run the real embedded ROM reset entry and cold slot scan in simulation
  through the slot-6 `$C600` probe.
- [x] Run the stock DOS 3.3 System Master and ProDOS 2.4.3 track-0 paths through
  the production Disk II slot bus and enter each loaded boot sector at `$0801`
  in simulation.
- [ ] Reach a BASIC prompt or a known monitor loop in a full-ROM system test.
- [x] Add and test held-key repeat for mapped USB keyboard keys.

### Input, Video, and Audio

- [x] Bridge USB keyboard data, Open Apple, Closed Apple, and Ctrl+Pause reset.
- [x] Bridge four normalized USB joystick axes and three buttons.
- [x] Count paddle expiry in native bus cycles.
- [x] Route a virtual warm reset to the motherboard and soft CPU only.
- [x] Feed posted screen writes and switch state into normal renderer records.
- [x] Mix the motherboard speaker into both audio channels.
- [ ] Validate DVI text, lores, hires, double-hires, 40/80-column, mixed-mode,
  raster, and PAL output on a board.
- [ ] Validate USB keyboard layouts, held-key behavior, and a range of real
  joysticks/gamepads.
- [ ] Validate speaker pitch, level, mute, and mixed card audio on the analog
  output.
- [ ] Add analog cassette and external annunciator/utility-strobe I/O if
  connector-level //e game/cassette port compatibility enters the scope.

### Storage and Cards

- [x] Force only virtual Disk II into slot 6 for the ONE//e session.
- [x] Keep Disk II on the shared synthetic slot bus and restore saved state on
  exit.
- [x] Keep SmartPort on the shared slot-7 bus after the slot-6 cold probe.
- [x] Block the AD8088 virtual bus-master path during ONE//e.
- [x] Load and enter the first sector from the stock DOS 3.3 System Master
  image through virtual Disk II in simulation.
- [x] Load and enter the first sector from the stock ProDOS 2.4.3 image through
  virtual Disk II in simulation.
- [ ] Complete a DOS 3.3 boot to an OS-ready prompt.
- [ ] Complete a ProDOS boot to an OS-ready prompt.
- [ ] Prove multi-track Disk II service beyond track 0.
- [ ] Test Disk II reads, writes, WOZ timing, motor state, and sound on a board.
- [ ] Run a compatibility pass for each supported virtual card and its backing
  hardware or firmware service.

## Validation Record

### Passed Focused Checks

The following focused checks have passed on this branch:

- `python scripts/test_onee_mode_safety_guard.py`: source contract plus XSim
  for reset-off, every monitored raw transition, modeled power veto, direct
  kill, held-high PHI0, held-low RESET#, isolation hold, return-to-idle quiet
  time, and manual reselect.
- `python scripts/test_apple_virtual_bus.py`: native phase order, idle scanner
  cadence, card response, floating-bus fallback, local RESET#/IRQ#/NMI#/INH#/
  RDY#/DMA# returns, RDY replay, a late two-clock registered slot master,
  owner/address/R/W stability through `data_en`, virtual DMA ownership, and CPU
  resume after release.
- `python scripts/test_onee_integration.py`: top-level source contract plus
  `tb_apple_bus_isolation`; address, R/W, and data release, IRQ/DMA/INH release,
  transceiver disable, and direction clears pass in simulation.
- `python scripts/test_onee_motherboard_io.py`: keyboard clear rules, status
  polarity and floating low bits, C02x/C03x/C04x mirrors, video switches,
  IOUDIS/DHIRES, annunciators, C06x mirrors, and native paddle expiry.
- `python scripts/test_onee_bus_integration.py`: joined vTW, virtual bus,
  motherboard I/O, arbiter, Disk II, and SmartPort test. At commit `8fec225`,
  it proved C000/C010, Apple keys, status/scanner merge, side effects, internal
  INH, forced C600 Disk II ROM, slot-7 release, C7/C8 SmartPort bus access, no
  private SmartPort cycles, and no physical writes.
- `python scripts/test_onee_input_bridge.py`: FIFO, backpressure, simultaneous
  full pop/push, sticky overflow, live keys/buttons, paddles, reset handshake,
  disable masking, and isolated synthesis.
- `python scripts/test_onee_config_menu.py`: seven menu, register, session-only,
  manual-start, refusal, timeout, and runtime-binding checks.
- `python scripts/test_onee_vtw_runtime.py`: eight cold start, isolation, ROM,
  Disk II override, stop order, running-state, menu-close, and host-path checks.
- `python scripts/test_onee_input_service.py`: ten source checks plus a native
  host harness for key translation, initial edges, timed held-key repeat,
  multi-key selection, FIFO backpressure, Ctrl+Pause, Apple keys, axes,
  buttons, ownership, disconnect, and effective drop.
- `python scripts/test_usb_hid_service.py`: all 13 existing USB HID, hub,
  service-polling, Vitis-source, USB0 storage-priority, and diagnostic checks.
- `python scripts/test_boot_menu_usb_keybindings.py`: all nine boot-menu USB
  binding and input-capture checks.
- `python scripts/test_onee_top_io.py`: top-level input bridge, eight-native-
  cycle virtual reset, physical-reset separation, speaker hookup, and reset
  controller XSim.
- `python scripts/test_onee_speaker_audio.py`: signed waveform, DC removal,
  saturation, sample hold, disable, and reset behavior.
- `python scripts/test_onee_video_path.py`: passed; checks full NTSC
  cadence, VBL, frame reset/wrap, C050/C057 frame state, posted screen-write
  renderer-input records, and a real 16K ROM
  smoke from reset vector `$FA62` through hidden slot 7 to `$C600` and slot-7
  release.
- `python scripts/test_onee_disk2_boot.py`: passed for
  `software/DOS 3.3 System Master.dsk` in DOS sector order and
  `software/ProDOS_2_4_3.po` in ProDOS sector order. It host-validates the
  nibblized address and data fields, then runs the exact embedded 16K Enhanced
  //e ROM, production 130-clock virtual-bus cadence, 13-client arbiter,
  `disk2_slot6.mem`, and `disk2_card` with the private vTW Disk II port off.
  Each XSim run completed the stock 80-step calibration and entered `$0801` at
  130.77294375 ms. DOS matched `$0800-$0803` to `01 A5 27 C9`; ProDOS matched
  `01 38 B0 03`. Each run counted 73,479 slot cycles, 5,764 I/O cycles, 5,599
  data reads, 879 DDR reads, and 28 D5 prologs, with zero card-to-bus or engine
  response mismatches.
- The 20-test `python scripts/test_vtw.py` suite passed at commit `8fec225`.
- ARM GCC syntax-only checks passed for the new frontend ONE//e service, input
  service, USB HID changes, vTW service changes, config menu, and `main.c`.

### Synthesis and Firmware Build State

- Full `appletini_yarz_top` synthesis passed at commit `8fec225` with zero
  errors and zero critical warnings.
- [x] Candidate-source `appletini_yarz_top` synthesis at `530b29e`, including
  the input bridge, warm reset, speaker mixer, video hookup, held-level safety
  veto, virtual-cycle tuple, and contained diagnostic paths, passed on
  2026-08-16 with zero errors and zero critical warnings.
- Isolated `onee_input_bridge` synthesis passed with zero errors and zero
  critical warnings.
- The `72d299f` implementation routed and wrote a bitstream, but its final
  extra post-route physical optimization still reported WNS -5.051 ns, TNS
  -56007.917 ns, WHS +0.059 ns, and THS 0. The export script rejected the
  timing failure. Do not use that failed-timing bitstream as a delivery image.
- The next full implementation at `0a155fa` reduced the failure to WNS
  -0.593 ns, TNS -122.695 ns, and 581 failing setup endpoints, with WHS
  +0.020 ns. Analysis assigned 459 of those path objects to the live
  13-client write-arbiter owner/address path feeding virtual slot decoders.
  Commits `8550239`, `9f3c259`, `9a5deb8`, and `530b29e` shorten or contain
  those paths.
- [x] The full `530b29e` implementation met all reported setup, hold, and
  pulse-width constraints with WNS +0.038 ns, TNS 0, WHS +0.051 ns, zero
  critical warnings, and zero errors. It exported a same-source bitstream and
  XSA to immutable archive
  `.timing_runs/20260816T140140Z-530b29e5-full`.
- [ ] Reach the project's +0.300 ns setup-margin promotion gate and repeat the
  clean full build at the same commit. The +0.038 ns candidate proves that the
  source can route and export, but it is not a promoted final image.
- [ ] Synthesize and route current head `354cc9a`. The CPU arithmetic-stage,
  speaker-clamp, scanner-read, and local AXI write-data changes in `a6cd13e`,
  `ad0f988`, and `354cc9a` have focused test proof but no full-route result yet.
- [ ] Run the final full source regression after all branch commits.
- The first 2026-08-16 Vitis attempt made BSP content, but
  `vitis_workspace/appletini_platform/export/.buildstatus` reported
  `export=ERROR`. The expected
  `export/appletini_platform/appletini_platform.xpfm` did not exist, so app
  creation stopped before the frontend build. This run does not prove a
  firmware link or produce a new firmware image.
- `python scripts/test_vitis_platform_build_guard.py` passed. The workspace
  script now permits one bounded retry of a transient platform-build failure,
  then stops on another nonzero result or a missing exported XPFM instead of
  continuing into app creation.
- [x] A fresh Vitis 2025.2 workspace build after `4a2d4d6`, using the
  `530b29e` hardware candidate, completed on 2026-08-16. Platform
  `.buildstatus` reports `export=SUCCESS`, the XPFM exists, and the run linked
  `fsbl.elf`, `bootloader.elf`, `frontend_core1.elf`, and `frontend.elf`.
- [ ] Rebuild against the final promoted XSA, then package and byte-check the
  final firmware image. The successful workspace build proves the PS source
  and platform export; it is not a programmed or board-tested image.

### Missing Board and End-to-End Proof

- [ ] Program a final same-source bitstream and firmware image on an Appletini
  ONE.
- [ ] Verify all physical Apple pins and translators while ONE//e starts,
  runs, faults, stops, and returns to host mode, including PL configuration and
  both card/Apple power orders.
- [ ] Verify a live-host event stops ONE//e with no contention, back-power, or
  automatic restart.
- [ ] Complete real DOS and ProDOS boots to their OS-ready states through
  virtual Disk II.
- [ ] Check a rendered frame, USB input, paddles, warm reset, speaker, Disk II,
  SmartPort, interrupts, and normal host mode on hardware.

The deepest real-ROM simulations currently enter the first DOS 3.3 and ProDOS
2.4.3 boot sectors at `$0801`. They do not prove either OS-ready state,
multi-track service beyond track 0, a BASIC prompt, rendered DVI output, or
physical safety.
