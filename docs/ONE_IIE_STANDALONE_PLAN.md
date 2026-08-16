# ONE//e Stand-Alone Mode Plan and Implementation Record

Branch: `feature/self-contained-one-iie`

Baseline: `a4c2d22fc6029263f61a5cfe1178a2ab3c6ef762`

Last source audit: 2026-08-17

## Status Rules

- `[x]` means the named source path exists and the listed source check or
  simulation has passed.
- `[ ]` means the work or proof is still missing.
- An RTL simulation or a clean build does not count as board proof.
- The clean `36a5bd2` package is the historical F0.9.77 hardware test image.
  The user waived the +0.300 ns release margin for that test handoff only. The
  card ran it out of the Apple slot and reported a false `LOCKED` state.
- F0.9.78 contains the source correction for that false lock. The card starts
  ONE//e with that image, but the test exposed three runtime state faults:
  SmartPort could not be the cold-boot target, Disk II activity could retain a
  `SMARTPORT SP1` label, and USB speed keys changed the label without changing
  the running core.
- The F0.9.79 source changes correct those three faults. The focused source,
  native, ROM-path, and bus tests pass. The clean full build and test package
  are complete. Its board retest exposed three later faults: a speed key used
  during ONE//e takeover could be discarded and leave the core at full speed,
  key repeat used fast frontend poll counts instead of elapsed time, and the
  demo image viewer did not enable IOUDIS before selecting DHIRES, so DHGRi
  remained HGRi.
- F0.9.80 is a historical test image. The speed, repeat, and DHGRi fixes reached
  a timing-clean baseline build, but that build was not packaged because the
  user then required the manual ONE//e selection to stay latched. Checkpoint
  `36be6c5` implements that rule. The final source checks, full build,
  exact-XSA Vitis build, and test package are complete at `cd8e9b8`. Board
  programming and retest found the remaining persistence, interlaced-video,
  fixed-control, and menu-pause work recorded below.
- F0.9.81 is the current test image. Checkpoints `e076280`, `9feaccd`, and
  `b33b631` fix that work. The clean full nonincremental build, exact-XSA
  Vitis build, and package are complete. Programming and board retest remain
  open.

## Goal and Supported Shape

ONE//e is a manually selected, self-contained 128K Enhanced US Apple //e. Its
saved ON intent survives a card power cycle until Apple activity forces it
OFF. It runs
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
- `acb770d`: register the resolved virtual data phase while keeping card data
  live before that phase for existing `serve_en` users.
- `649faca`: pipeline the unchanged ONE//e speaker output calculation while
  preserving its public sample cadence and values.
- `36a5bd2`: pipeline vTW slowdown bookkeeping and add directed checks for the
  unchanged slowdown timing contract.
- `65adf37`: document the F0.9.77 test handoff.
- `9b5f474`: select the storage label from effective Disk II and SmartPort
  activity instead of the saved Slot 6 bit.
- `4b02a05`: route all live TransWarp speed controls through one verified
  host-or-ONE//e control writer.
- `6cf17a2`: keep the effective Disk II service and ONE//e session state intact
  across reset and config reapply.
- `d4be1dd`: latch the configured ONE//e boot target and give a SmartPort
  session slot-7 ownership over saved SuperSprite state.
- `27232cc`: preserve the configured Disk II target when physical Slot 6 is
  off; the host still applies its own SmartPort fallback in the PL.
- `7838f235`: learn the stable open-connector U533 input vector, use a
  96-cycle quiet interval, retain the main translators as an input-only clock
  monitor, disable auxiliary translator U234, and reserve F0.9.78.
- `a94aefc`: reserve F0.9.79 and form its source and packaging checkpoint.
- `3e978a1`: time ONE//e key repeat in milliseconds instead of frontend polls,
  with a 500 ms first delay and 100 ms repeat interval.
- `9cbf3e0`: make the demo viewer enable IOUDIS before DHIRES, rebuild its disk
  image, and execute the assembled HGRi and DHGRi mode setters in a regression.
- `147e56d`: retain a requested ONE//e speed through the pre-takeover window,
  first core release, and virtual warm reset.
- `dde9b20`: let an explicitly open Appletini menu stage the next ONE//e speed
  while host TransWarp is off; the same key stays off when the menu is closed.
- `e7fe40a`: reserve F0.9.80 and form the timing-clean pre-latch build
  checkpoint.
- `36be6c5`: keep a manual ONE//e selection with no software expiry, retry a
  failed private runtime without losing its speed, and require a fresh manual
  selection after every terminal stop.
- `e076280`: drain accepted old AXI reads before restarting DVI scanout,
  reserve and verify each displayed triple-buffer slot, and cache unchanged
  full-shadow interlaced frames.
- `9feaccd`: persist the global ONE//e latch, force it OFF after Apple
  activity, add fixed ONE//e USB controls, and pause the soft CPU while the
  config menu owns input.
- `b33b631`: reserve firmware version `F0.9.81`.

## Safety Contract

Safety has priority over keeping ONE//e running.

The supervisor in `hdl/apple/onee_mode_safety_guard.sv` owns three separate
states:

1. `onee_request_q`: the user's current manual request.
2. `onee_activity_lockout`: sticky evidence of Apple-side activity.
3. `onee_enable_effective`: the only signal which may run the virtual machine.

The request itself asserts `physical_bus_isolate`. The supervisor learns the
stable input vector presented by U533 instead of requiring fixed Apple idle
levels. It does not set `onee_enable_effective` until the guard has seen no
U533 transition for 96 clocks in the 133.333 MHz fabric domain, about 0.72
microseconds, and a new selection is armed. This orders physical isolation
before soft-CPU start and lets an open connector settle at either logic level.

The always-observed U533 vector contains PHI0, 7M, Q3, M2SEL, M2B0, and
DEVSEL#. A raw difference from the synchronized live vector or a synchronized
transition sets the sticky activity lockout. That state asynchronously clears
one contained run-state flop and sets the physical-isolation hold. In
particular, PHI0, 7M, and Q3 from a running Apple keep changing, so they block
start or kill a running ONE//e in less than a microsecond. This path does not
wait for firmware or for the status synchronizer. The run-state flop's Q
drives the high-fanout virtual machine selection. Software's live activity and
inhibit fields use a sampled copy, not the raw pins.

The input constraint gives all six watched U533 pins the same 5 ns
pad-to-first-flop route bound. PHI0, 7M, Q3, M2SEL, M2B0, and DEVSEL# must all
remain in that set; none may rely on an unconstrained asynchronous route.

U234 carries RESET#, INH#, IRQ#, NMI#, RDY#, and DMA#. ONE//e disables U234
while physical isolation is asserted, so changes or floating levels on its
FPGA side do not enter the activity detector. The main translators remain
enabled in forced Apple-to-FPGA direction during ONE//e. U533 therefore keeps
the three Apple clocks and select lines visible while every FPGA-side bus
driver remains off.

The manual selection has no firmware expiry. If REQUEST stays high and the PL
reports no hazard, long stable quiet time leaves the selection in force. A
failed private-runtime start, or a released core which later stops, suspends
vTW and retries inside the same selected session. That recoverable path keeps
the exact queued or live speed override.

Apple-side activity, a lost PL request echo, missing or invalid safety logic,
and explicit manual OFF are terminal stops. On a terminal stop:

- Effective mode drops at once.
- Firmware stops vTW, clears the session request, and clears session-only
  speed state.
- The request is never raised again by polling, startup, config load, or a
  profile.
- Quiet time alone cannot restart the machine.
- The user must select the boot-menu item again after the request has gone off
  and the learned U533 input vector has shown no transition for the quiet
  interval.

While isolation is high, `apple_bus_wrapper` blocks address/R/W drive, data
drive, IRQ#, INH#, DMA#, and the address and data output directions. It leaves
the main translators enabled as an input-only monitor and `apple_top` disables
auxiliary translator U234. `apple_top` sends an all-zero physical
`AppleBus_write` record and releases the separate physical RESET output. The
virtual warm-reset path does not connect to the physical RESET path.

### Board Limit: No Slot-Power Sense

This board revision has no independent Apple-slot power-sense input. The top
level ties `apple_power_present_raw` low and reports both the
power-sense-present and Apple-power bits as zero in card-control register
`0x5B`. Slot +5 V cannot provide that distinction when the same pin supplies
the card's stand-alone power: it is high with the card out of the Apple.

The running-host test therefore uses the bus clocks. The guard learns any
stable open-connector U533 vector, then treats each PHI0, 7M, Q3, M2SEL, M2B0,
or DEVSEL# transition as Apple activity. This detects a powered, clock-running
Apple in less than a microsecond. Without a separate sensor, it cannot prove
that a host whose clocks have stopped is unpowered; a powered, clock-stopped
host and an unplugged connector can both present a stable vector. The
output-side design isolates every drive path before ONE//e starts, but
simulation cannot prove translator behavior, leakage, back-power, or board
high impedance. Until the board tests below pass, do not treat ONE//e as safe
for use while it remains plugged into an Apple.

## Implemented Architecture

### Virtual Bus and Slot Pins

`hdl/apple/apple_virtual_bus.sv` generates the native phase contract:
`drive_en`, `addr_en`, `sss_en`, `serve_en`, and `data_en`. Idle cycles keep the
1 MHz card and scanner cadence running. The chosen bus record feeds the same
soft-switch manager, capture path, and virtual card modules used in host mode.
After `drive_en`, the bus samples the active owner/address/R/W tuple across a
window which includes the vTW posted-write pipeline, then holds that tuple from
`addr_en` through `data_en`. Slot decoders therefore do not use a live
high-fanout owner mux during the visible cycle. Card write or floating data
remains live before the data phase so existing `serve_en` consumers can see it.
One fabric clock before `data_en`, the bus captures the merged arbiter byte and
holds it through the public data phase and soft-CPU response.

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

`hdl/apple/onee_cold_slot_scan.sv` uses the configured boot target. For a
SmartPort target, slot 7 is visible on the first `$C7xx` probe. For a Disk II
target, slot 7 stays hidden until the first `$C6xx` probe, so virtual Disk II
answers first; slot 7 becomes visible after that probe. ONE//e entry and each
virtual warm reset re-arm this choice from the same configured target.

The configured target and the physical-host handoff are separate signals. The
physical-host path may fall back to SmartPort when the saved Slot 6 enable is
off. ONE//e must not use that fallback rule: it supplies its virtual Disk II
for the session even when the saved Slot 6 setting is off. This split keeps the
menu choice intact without changing normal host behavior.

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

The Boot Settings page has row 2 named `ONE//e standalone`, before the USB
binding rows. It reports `OFF`, `RUNNING`, or `LOCKED`. Its global ON/OFF latch
is saved in `0:/appletini_cfg.txt`; profiles neither write nor load it.

The item help is:

> Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an
> Apple host. The selection survives a card power cycle and starts only after
> the connector is quiet. Any Apple-bus activity stops ONE//e and saves it
> OFF. After the connector is quiet, select this item again to save it ON.

The sole high write to the ONE//e request register comes from an explicit menu
selection. A safe request has no software timeout: it remains selected while
physical isolation settles and while a recoverable private-runtime fault is
retried. The menu closes only when the vTW status confirms both effective
enable and a released core; the user can reopen it to stop the session.

F0.9.81 saves manual ON to the global config before it raises the PL request.
After a card boot it restores that intent only when the safety signature is
valid, the connector is quiet, reselect is armed, and no activity, power, or
lockout bit is set. Apple activity or a lost request forces a terminal stop
and queues a durable OFF write. The writer syncs a temporary file, keeps the
last committed file as a backup, and then installs the new global file. A
failed write stays pending and retries. A profile cannot arm or disarm ONE//e.

There is one hard limit. If all card power disappears after PL sees Apple
activity but before the PS records OFF on the SD card, that event cannot be
made durable on this board. If the Apple remains active at the next boot, the
PL guard still blocks ONE//e and firmware saves OFF then. An absolute record
across that power-loss window needs a nonvolatile hardware event latch.

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

Host TransWarp and ONE//e use separate run state: `g_state` records the host
path and `g_onee_running` records the stand-alone path. The old USB-key path
tested saved host intent and wrote `VTW_CTRL` only for a host `RUN` state. It
still formatted the new speed notice, which caused the label-only change seen
on F0.9.78. Menu speed changes had a separate ONE//e branch, so they worked.

All live menu and USB-key changes now use one `vtw_apply_ctrl_live()` writer.
It selects the active host or ONE//e control word, preserves that session's
run/reset bits, writes `VTW_CTRL`, and checks the register readback before it
reports success. The ONE//e word keeps private Disk II acceleration disabled.
On a failed readback, a USB speed action restores its prior override and shows
`TW: CONTROL WRITE FAILED` instead of a false speed label.

The F0.9.79 board test found another state boundary before that live writer
could act. With saved host TransWarp off, the old speed-key gate discarded a
choice made in the open Appletini menu before ONE//e selection. It also treated
the core as off after the user requested ONE//e but before `g_onee_running`
became true. The first released control word could therefore retain the
configured full-speed value. A later virtual warm reset did not itself choose
full speed; it exposed the speed which the session had actually retained.

The F0.9.80 source accepts a speed request while host TransWarp has saved
intent, while ONE//e is requested, effective, or running, or when the key event
comes from an explicitly open Appletini menu. The input handler passes that
menu context with only the current key event; it does not create a global
enable. If no core is live, the action queues the override and reports
`TW NEXT` instead of claiming a live write. The next ONE//e start uses it even
when saved host TransWarp is off. The same key in a closed menu and true-off
state still reports `TW: OFF` and queues nothing.

All three ONE//e start control words use the effective mode and divider, so the
first core release uses that queued or configured speed. The private warm
reset does not rewrite `VTW_CTRL`, so the same session rung survives it. A
recoverable runtime suspend writes vTW control to zero but retains the exact
temporary override; the retry uses that same mode and divider in every start
word. A terminal session stop clears the temporary override, and a new manual
session returns to the saved configured speed.

Reset and config reapply paths also need the effective session state, not just
saved host intent. One centralized Disk II setter now keeps the service on
while `g_onee_running` owns the machine, without changing the saved Slot 6
setting. On stop, the same setter applies the latest saved setting rather than
a restore value captured when the session began. The ONE//e-private reset path
skips the IIgs `$C029` DMA write, which belongs only to a physical-host reset.

Ctrl+Pause requests a virtual warm reset. The PL holds virtual RESET for at
least eight full native cycles and acknowledges the input bridge. This resets
the soft CPU and virtual motherboard I/O but preserves shadow RAM. It never
asserts the Apple connector's RESET signal.

When the Appletini config menu opens during ONE//e, firmware blocks keyboard
and joystick delivery and sets `VTW_CTRL.PAUSE`. The PL finishes any access in
flight, then parks the 65C02 at a completed cycle without reset or a speed
change. DVI scanout and other independent clocks keep running so the menu can
stay visible. On close, input remains blocked until all keys, modifiers,
buttons, hats, and joystick axes return to neutral; firmware then resumes the
same core state and speed.

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

Mapped keys emit once on their press edge. F0.9.79 then waited 1,000 active
input-service polls before repeat and 100 polls between repeats. The USB loop
can call that service twice in each fast frontend pass, so those counts could
expire during a brief tap and fill a menu with Down events.

F0.9.80 uses the CherryUSB millisecond clock instead. Repeat starts after 500
ms and continues every 100 ms. A late poll emits one event and starts the next
interval from the current time; it does not catch up by filling the queue with
missed events. The deadline check also works across the 32-bit clock wrap. The
newest held mapped key across all HID devices owns repeat. Releasing or
disconnecting it selects the newest remaining held key and starts a new 500 ms
delay. Repeat uses the current Shift, Control, and Caps Lock state. Caps Lock,
Pause, modifier-only reports, and unmapped keys do not repeat. A session stop
clears all repeat state.

Caps Lock state exists only in the ONE//e key translator. Firmware does not
send a USB HID output report for it, so a keyboard's Caps Lock LED may not show
the current ONE//e state. This is an input-device limit, not a start or safety
blocker.

A standard full-size USB HID keyboard should work, including its main key
block, arrows, modifiers, and mapped numeric keypad keys. The translator uses a
US layout. A boot-protocol keyboard supports six simultaneous non-modifier
keys plus modifiers. A parsed report-protocol keyboard tracks at most eight
active key usages. Unusual vendor reports and exotic NKRO modes are not
guaranteed. Firmware sends no keyboard LED output reports, so Caps Lock and
other lock LEDs do not track ONE//e state. Media and other extra keys have no
Apple //e character unless the firmware gives them a separate binding.

While ONE//e is selected, its fixed controls bypass the saved USB bindings
and cannot be edited: Page Up/Page Down change tabs; arrows move in the menu;
Enter or keypad Enter selects; Escape closes; Print Screen captures the Apple
screen; Shift+Print Screen captures the 1080p output; keypad `+` and `-` step
TransWarp speed; and keypad `0` toggles acceleration. Print Screen and the
keypad speed keys work with the menu closed. The saved long-hold menu key still
opens and closes the menu.

### Storage and Other Cards

In ONE//e mode, slot 6 always selects `disk2_card`, even if the saved slot-6
setting is off. Firmware also session-enables its track service. The vTW
private Disk II shortcut is forced off, so slot ROM, `$C0E0-$C0EF` soft
switches, sequencer, motor, write, WOZ timing, and drive-sound state all use
the shared synthetic slot bus. ONE//e has no path to a physical Disk II card,
cable, or drive.

When SmartPort is the configured boot target, it appears in virtual slot 7 on
the first `$C7xx` cold-boot probe. When Disk II is the target, SmartPort appears
after the `$C6xx` probe. Its private vTW shortcut is disabled in ONE//e, so its
slot ROM, `$C800` window, control, data, and interrupt behavior use the same
synthetic slot bus. Other enabled virtual cards also see the selected bus
record. Cards still need their own compatibility tests; sharing the record
does not prove every card or external chip works without a host board.

Saved SuperSprite state is another host policy which cannot own slot 7 during
a SmartPort-target ONE//e session. The effective slot-7 selector therefore
gives the configured SmartPort target session priority over SuperSprite, while
leaving the saved SuperSprite setting intact for normal host mode and after
ONE//e stops.

The storage overlay now polls the effective Disk II service state instead of
gating it on the saved Slot 6 bit. The old gate hid the session-only Disk II
which ONE//e had enabled, while a SmartPort `STATUS` poll remained as the last
visible source and produced the false `SMARTPORT SP1` label. Source selection
now gives SmartPort data first place, then current Disk II motor/read/write
work, then a SmartPort `STATUS` poll, then the retained valid source. Thus Disk
II work beats SmartPort discovery traffic and shows its actual drive number.

### Video and Speaker

The virtual bus drives the existing native timing generator and normal cycle
capture path. The vTW core posts screen writes through the same capture queue
used by host mode. The focused video bench covers all 65 cycles across all 262
NTSC lines, starts VBL at line 192 cycle 0, checks frame wrap and frame-zero
reset, checks `$C050/$C057` state in renderer frame metadata, and checks that
posted `$0400/$2000` writes reach renderer-input records.

The F0.9.79 demo viewer selected 80COL and accessed `$C05E` for its DHGRi
record, but it did not first write `$C07E` to turn IOUDIS on. On an Enhanced
//e, `$C05E/$C05F` control AN3 while IOUDIS is off and DHIRES while it is on.
The access therefore left DHIRES off, and the renderer correctly saw HGRi.
Checkpoint `9cbf3e0` adds the missing `$C07E` write before the common DHGR
mode setter. Its test runs the assembled HGRi and DHGRi setters against the
Enhanced //e switches and requires distinct final modes. The demo disk image
was rebuilt from that source and remains part of the F0.9.81 retest.

The later intermittent DHGRi lines were not an AUX RAM fault. Three timing
faults could damage a displayed frame: DVI scanout reset its FIFO and AXI
outstanding count while accepted old reads could still return; the reader
could claim a published slot while the writer reclaimed it; and rebuilding
every unchanged interlaced shadow frame could starve the CPU1 capture drain.
Checkpoint `e076280` drains old AXI traffic before restart, reserves then
verifies the displayed slot, and caches unchanged full-shadow modes by memory
generation and mode metadata. TEXT and MIXED stay uncached so flash advances.
New gap, underrun, and AXI counters support the board retest. A capture event
already lost before this fix cannot be reconstructed.

Each `$C03x` access toggles the motherboard speaker. `onee_speaker_audio.sv`
turns that state into signed, DC-blocked, saturated 16-bit mono samples. The
board top mixes the sample into both left and right channels after the existing
Mockingboard plus Disk II mix, using the existing saturating adder and output
volume/mute path. The output arithmetic is pipelined without changing the
public sample cadence or values. Effective-mode loss masks the ONE//e sample
to zero.

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

- [x] Start the PL request low, then restore only a saved global manual intent
  after the safety checks pass.
- [x] Isolate physical outputs before effective mode can start.
- [x] Reprove that raw U533 transitions kill effective mode without firmware
  after the F0.9.78 guard change.
- [x] Reprove that the guard learns an arbitrary stable open-connector vector
  instead of requiring fixed Apple idle levels.
- [x] Reprove that PHI0, 7M, and Q3 remain visible through U533 while ONE//e is
  isolated, so running Apple clocks cannot become quiet.
- [x] Latch activity and require a later manual selection.
- [x] Keep profiles and config apply from changing the latch; let boot raise
  request only from the saved global manual intent.
- [x] Keep a safe selected request active with no software expiry, including
  through long stable quiet intervals.
- [x] Suspend and retry a failed private runtime without clearing its exact
  queued or live speed override.
- [x] Treat watched Apple activity, lost request echo, and manual OFF as
  terminal saved-OFF stops; missing safety logic keeps outputs off without
  erasing the saved choice.
- [x] Bind all six watched U533 inputs to the 5 ns raw-input route constraint.
- [x] Persist manual ON across card power cycles, persist terminal Apple stops
  as OFF, and keep the latch out of profiles.
- [x] Reprove physical address, R/W, data, IRQ, INH, DMA, and RESET drive masks
  with the main translators forced input-only and auxiliary translator U234
  disabled.
- [ ] Prove connector pin voltage, high impedance, leakage, and back-power on
  a production board.
- [ ] Prove external translator OE/DIR defaults while the PL is unconfigured,
  configuring, or held in reset, under both Apple-first and card-first power
  order.
- [ ] Test a clock-running Apple already on and an Apple powered on during
  ONE//e.
- [x] Document that slot +5 V is also the stand-alone supply and cannot act as
  Apple-presence sense in that setup.
- [x] Document the powered, clock-stopped host which no clock-only detector can
  distinguish from a stable open connector.
- [ ] Add a board-level power interlock or power-sense input if ONE//e must
  reject that powered, clock-stopped case.

### Virtual Motherboard and Boot

- [x] Generate native virtual bus phases and scanner cadence.
- [x] Return card-arbiter data and local IRQ/NMI/RDY/INH behavior to the soft
  CPU without driving physical pins.
- [x] Implement keyboard, status, video, annunciator, paddle, cassette-latch,
  utility-strobe, and speaker soft switches.
- [x] Add the boot-menu action and durable global manual latch.
- [x] Auto-start vTW through a stand-alone cold-ROM path.
- [x] Select the configured SmartPort or Disk II cold-boot order without using
  the physical-host fallback rule.
- [x] Re-arm the configured target across a virtual warm reset.
- [x] Run the stock DOS 3.3 System Master and ProDOS 2.4.3 track-0 paths through
  the production Disk II slot bus and enter each loaded boot sector at `$0801`
  in simulation.
- [ ] Reach a BASIC prompt or a known monitor loop in a full-ROM system test.
- [x] Add and test elapsed-time held-key repeat for mapped USB keyboard keys,
  including a one-event short tap and no catch-up burst after a late poll.
- [x] Support standard full-size USB HID keyboards within the US-layout,
  six-key boot-report, and eight-key parsed-report limits.
- [x] Route menu and USB speed changes through the same readback-checked live
  `VTW_CTRL` writer for host and ONE//e sessions.
- [x] Queue a speed key from an open Appletini menu or requested/effective
  ONE//e takeover, put that speed in the first core-release word, and preserve
  it across a private warm reset. Keep true off closed-menu actions off.
- [x] Reapply effective Disk II state through one setter on config and reset,
  and skip the host-only IIgs `$C029` DMA write for a ONE//e reset.
- [x] Use fixed ONE//e menu, screenshot, and keypad speed controls which
  bypass saved bindings.
- [x] Pause the soft CPU and block Apple input while the config menu is open;
  wait for neutral input before resume.

### Input, Video, and Audio

- [x] Bridge USB keyboard data, Open Apple, Closed Apple, and Ctrl+Pause reset.
- [x] Bridge four normalized USB joystick axes and three buttons.
- [x] Count paddle expiry in native bus cycles.
- [x] Route a virtual warm reset to the motherboard and soft CPU only.
- [x] Feed posted screen writes and switch state into normal renderer records.
- [x] Enable IOUDIS before the demo viewer selects DHIRES and execute the
  assembled HGRi and DHGRi setters in a distinct-mode regression.
- [x] Drain late AXI reads on DVI restart, reserve and verify framebuffer
  claims, and cache unchanged full-shadow interlaced frames.
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
- [x] Put SmartPort on the first slot-7 probe when it is the configured target,
  or after the slot-6 probe when Disk II is the configured target.
- [x] Override saved SuperSprite slot-7 ownership during a SmartPort-target
  ONE//e session without changing the saved host setting.
- [x] Poll effective session Disk II state and let its motor/read/write work
  take priority over SmartPort `STATUS`-only activity in the storage overlay.
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

### Passed F0.9.77 Focused Checks

The following focused checks passed for the F0.9.77 source. They are a
historical record and do not validate the later F0.9.78 correction:

- `python scripts/test_onee_mode_safety_guard.py`: source contract plus XSim
  for reset-off, every monitored raw transition, modeled power veto, direct
  kill, held-high PHI0, held-low RESET#, isolation hold, return-to-idle quiet
  time, and manual reselect.
- `python scripts/test_apple_virtual_bus.py`: native phase order, idle scanner
  cadence, card response, floating-bus fallback, local RESET#/IRQ#/NMI#/INH#/
  RDY#/DMA# returns, RDY replay, a late two-clock registered slot master,
  owner/address/R/W stability through `data_en`, live pre-data card visibility,
  held resolved data through the public data phase, virtual DMA ownership, and
  CPU resume after release.
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
  saturation, pipelined output, unchanged sample cadence, sample hold, disable,
  and reset behavior.
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
- The 20-test `python scripts/test_vtw.py` suite passed at commit `36a5bd2`,
  including the directed slowdown bookkeeping checks.
- ARM GCC syntax-only checks passed for the new frontend ONE//e service, input
  service, USB HID changes, vTW service changes, config menu, and `main.c`.
- The final regression at `36a5bd2` passed all nine focused ONE//e HDL scripts,
  all 98 frontend checks, both DOS and ProDOS `$0801` boot-sector runs, and the
  Appli-Card, SSC, SuperSprite, Uthernet II, and VidHD card suites.

### F0.9.78 Validation

- [x] Run the revised guard source checks and XSim for arbitrary stable U533
  vectors, 96-cycle quiet qualification, PHI0/7M/Q3 start veto and immediate
  kill, sticky lockout, and manual reselect.
- [x] Run the revised physical-isolation simulation for input-only main
  translators and disabled U234.
- [x] Run all stated focused ONE//e HDL and frontend regressions against the
  corrected source, including the vTW, boot-sector, and virtual-card checks.
- [x] Complete a full Vivado route and export, exact-XSA Vitis build, and
  firmware package for F0.9.78.
- [x] Program F0.9.78 and start ONE//e out of the Apple slot. It reached the
  Enhanced //e screen, then exposed the runtime faults recorded below.
- [ ] Complete the live-clock and electrical board tests.

The completed source and firmware checks apply to commit
`7838f23580d95a03e2e9f2442d80f2e3ce9c6ebf`. They do not replace the pending
live-clock and electrical board tests or the F0.9.79 functional retest.

### F0.9.78 Runtime Test and F0.9.79 Fixes

The F0.9.78 out-of-slot test reached the Enhanced //e ROM, but it found three
faults caused by parallel saved, host-runtime, and session-runtime state:

- The cold-slot helper always hid slot 7, so a configured SmartPort boot could
  not answer the ROM's first `$C7xx` probe. Disk II then answered at `$C6xx` if
  it had media; with no Disk II boot, the machine stayed at `Apple //e`.
- ONE//e enabled Disk II for the session without setting the saved Slot 6 bit.
  The overlay used that saved bit as its poll gate, then retained a SmartPort
  `STATUS` probe as the source. It could therefore show `SMARTPORT SP1` while
  Disk II drive 1 was running.
- USB speed keys checked saved host-vTW intent and wrote `VTW_CTRL` only when
  the host state machine said `RUN`. ONE//e instead used
  `g_onee_running`. The key path changed its notice even though it had not
  changed the control register. The config menu worked because it had a
  separate ONE//e write branch.

The F0.9.79 source removes those split decisions: cold-slot order uses the
configured target, storage uses effective service state and activity priority,
and all live speed controls use one readback-checked writer for the active
host or ONE//e session. The reset/config path uses the same effective Disk II
setter, and a SmartPort-target session overrides saved SuperSprite slot-7
ownership. The menu also keeps a configured Disk II target when physical Slot
6 is off; only the physical-host path applies that fallback. Both saved host
choices remain intact after ONE//e stops.

- [x] Run the focused cold-slot, joined-bus, real-ROM, effective Disk II,
  SuperSprite override, storage-selection, and live-speed-control regressions
  at the final F0.9.79 source checkpoint.
- [x] Complete a full Vivado route and export, exact-XSA Vitis build, and
  F0.9.79 test package.
- [x] Program and run F0.9.79 out of slot. That test exposed the later speed,
  repeat, and DHGRi faults below; it did not complete the electrical safety
  checklist.

### F0.9.79 Runtime Test and F0.9.80 Fixes

The F0.9.79 board test exposed three more boundary errors:

- With saved host TransWarp off, a bound speed key used in the open menu before
  ONE//e selection, or after the request but before `g_onee_running` was set,
  could be discarded. The first release then used full speed, and a virtual
  warm reset made that retained choice visible again. A config-page change
  worked because it ran after takeover.
- Key repeat counted service calls. A brief Down tap could outlive the count
  on the fast USB/frontend loop and scroll the demo menu to its last row.
- The demo viewer's DHGRi setter did not turn on IOUDIS. Its `$C05E` access
  changed AN3 instead of DHIRES, so the resulting mode remained HGRi. The
  other new image modes worked.

F0.9.80 is the next planned test image. Checkpoint `3e978a1` replaces repeat
poll counts with 500/100 ms deadlines. Checkpoint `9cbf3e0` adds the viewer's
missing `$C07E` write, rebuilds the disk, and tests the assembled mode setters.
Checkpoint `147e56d` queues a speed request during ONE//e takeover, applies it
to the first core release, retains it across virtual warm reset, and clears the
temporary override when the session stops. Checkpoint `dde9b20` also accepts
the preselection while the Appletini menu is open and host TransWarp is off,
without enabling that action when the menu is closed. Checkpoint `36be6c5`
removes the request-to-effective software timeout, preserves the exact speed
through recoverable runtime suspend/retry, and reserves terminal stop for
watched Apple activity, lost request, missing safety logic, and manual OFF.
Stable quiet time cannot expire a selection or restart a terminally stopped
one.

- [x] Add focused source and native checks for a one-event short key tap,
  elapsed repeat timing, clock wrap, and no repeat catch-up burst.
- [x] Execute the assembled HGRi and DHGRi mode setters against Enhanced //e
  IOUDIS/DHIRES behavior and require distinct results.
- [x] Add focused firmware and joined-bus checks for configured and queued
  start speed plus `VTW_CTRL` retention across virtual warm reset.
- [x] Add native and RTL checks for selection with no software expiry,
  recoverable runtime restart at the exact speed, terminal-stop latching,
  fresh manual reselect, and no restart after a long quiet interval.
- [x] Check that the XDC gives PHI0, 7M, Q3, M2SEL, M2B0, and DEVSEL# the same
  5 ns raw-input route bound.
- [x] Complete a clean full Vivado route and export for F0.9.80.
- [x] Build Vitis from that exact XSA and package the F0.9.80 test image.
- [ ] Program F0.9.80 and run the out-of-slot hardware retest in
  `docs/ONE_IIE_HARDWARE_TEST.md`.

### F0.9.80 Runtime Test and F0.9.81 Fixes

The F0.9.80 board test confirmed the prior boot-target, storage-label, speed,
repeat, and DHGRi-selection fixes. It exposed the next three faults: saved
ONE//e intent returned to OFF after a power cycle; DHGRi could gain stray
lines which could remain after changing images despite a zero-error AUX test;
and ONE//e still used editable USB bindings and let menu input reach the soft
Apple.

- [x] `e076280` fixes the DVI restart, triple-buffer claim, and interlaced
  renderer-drain races and adds focused simulation and handoff stress tests.
- [x] `9feaccd` saves manual ONE//e intent globally, saves OFF after a terminal
  Apple event, bypasses editable bindings with the fixed USB map, and parks
  the soft CPU while the menu owns input.
- [x] `b33b631` reserves F0.9.81 and forms the build checkpoint.
- [x] Complete the clean full Vivado route, exact-XSA Vitis build, and F0.9.81
  package.
- [ ] Program F0.9.81 and run the focused persistence, video, input, pause,
  and Apple-safety retest.

### Synthesis and Firmware Build State

- Full `appletini_yarz_top` synthesis passed at commit `8fec225` with zero
  errors and zero critical warnings.
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
- The full `530b29e` implementation was the first later candidate to meet all
  reported setup, hold, and pulse-width constraints. It reached WNS +0.038 ns,
  TNS 0, and WHS +0.051 ns and exported the historical archive
  `.timing_runs/20260816T140140Z-530b29e5-full`.
- [x] The clean full build of `36a5bd2` completed and exported for the approved
  test handoff as build `20260816T152953Z-36a5bd21-full`. It reports WNS
  +0.069 ns, TNS 0, WHS +0.046 ns, and WPWS +0.265 ns, with zero failing
  endpoints, unrouted nets, errors, or critical warnings. The same-source XSA
  SHA-256 is
  `f0cfb90ca6e05a951799b84cc39d963c71b881edecf1b1f5ec51320aefe7acdb`.
- [x] The final source regression at `36a5bd2` passed the nine focused ONE//e
  HDL scripts, 98 frontend checks, all 20 vTW tests, both DOS and ProDOS
  `$0801` runs, and the Appli-Card, SSC, SuperSprite, Uthernet II, and VidHD
  suites.
- [ ] Promote a release build only after it reaches the project's +0.300 ns
  setup-margin gate, repeats cleanly at the same commit, and passes the board
  checks below. The user waived that margin for the F0.9.77 through F0.9.80
  test images and F0.9.81 test image. The margin is waived for test only;
  F0.9.81 is not a promoted release.
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
- [x] A fresh Vitis 2025.2 workspace build against the exact `36a5bd2` XSA
  completed successfully. Platform `.buildstatus` reports `export=SUCCESS`,
  the XPFM exists, and the run linked `fsbl.elf`, `bootloader.elf`,
  `frontend_core1.elf`, and `frontend.elf`.
- [x] The F0.9.77 test package is complete. Root `FIRMWARE.BIN` and
  `.timing_runs/20260816T152953Z-36a5bd21-full/FIRMWARE_TEST.BIN` are
  byte-identical, each 4,236,236 bytes, with SHA-256
  `1b5a932f836471075fdb9a7a92664be44af2f0537787ece638ad9314a18d9b0f`.
  The handoff record is
  `.timing_runs/20260816T152953Z-36a5bd21-full/test_firmware_manifest.txt`.
  This is a test-only package, not a promoted release image.
- [x] The first out-of-slot board test powered the card from the slot +5 V pin
  and F0.9.77 reported `LOCKED`. The open U533 inputs did not match the guard's
  fixed reset vector. Physical isolation also disabled the main translator, so
  that design could not keep watching the bus clocks after selection.
- [x] The full F0.9.78 implementation from source commit
  `7838f23580d95a03e2e9f2442d80f2e3ce9c6ebf` exported as build
  `20260816T164305Z-7838f235-full`. It reports WNS +0.019 ns, TNS 0, WHS
  +0.063 ns, THS 0, and WPWS +0.265 ns, with no setup, hold, or pulse-width
  failing endpoints, route errors, missing constraint objects, or
  unconstrained internal endpoints. Its bitstream SHA-256 is
  `46a4f3ffa6d03bd6d9887506689e7b1c8ef605e6f7c252ad24a7e41f8094bbf6`;
  its XSA SHA-256 is
  `314435c8f18adfade2c2d766871d59c30b8d6078f548e33f9686a5c9dc758ed5`.
  The run manifest records `git_dirty=1` because the two ONE//e documents were
  edited; no HDL or firmware source was dirty during the run.
- [x] A fresh Vitis 2025.2 workspace build used that exact archived XSA.
  Platform export and all application builds completed successfully. Bootgen
  readback reports `total_images=4`.
- [x] Root `FIRMWARE.BIN` and
  `.timing_runs/20260816T164305Z-7838f235-full/FIRMWARE_TEST.BIN` are
  byte-identical, each 4,235,276 bytes, with SHA-256
  `47f7385846b8092e46784649a0f99f7b90e7d2415d2ee02eb627c2628a71d9fa`.
  The handoff record is
  `.timing_runs/20260816T164305Z-7838f235-full/test_firmware_manifest.txt`.
  This F0.9.78 image is a test image under the user's +0.300 ns margin waiver,
  not a promoted release.
- [x] The clean full F0.9.79 implementation from source and packaging commit
  `a94aefc27353f8ccdefe7b9dbf012d343a9f1564` exported as build
  `20260816T180541Z-a94aefc2-full`. It reports WNS +0.103 ns, TNS 0, WHS
  +0.019 ns, THS 0, and WPWS +0.265 ns, with no failing endpoints, route
  errors, missing constraint objects, or unconstrained internal endpoints.
  Its bitstream SHA-256 is
  `a03e90819fee064d4163572925392d18e9a05e47c8426f32b6258d2dd03afea8`;
  its XSA SHA-256 is
  `0963dc9999f253dac21dd69d3af5a20c7e88008e613479497a9762fc87dcedce`.
- [x] A fresh Vitis workspace build used that exact archived XSA. Platform
  export and all application builds report `SUCCESS`. Bootgen readback passed
  with `total_images=3` and `total_partitions=3`; the frontend contains the
  `196684`-byte CPU1 blob.
- [x] Root `FIRMWARE.BIN` and
  `.timing_runs/20260816T180541Z-a94aefc2-full/FIRMWARE_TEST.BIN` are
  byte-identical, each 4,109,260 bytes, with SHA-256
  `3d4c6d9797eac2ebfae74d110963077a91897175f6c19cc21b56a9f69d567941`.
  The handoff record is
  `.timing_runs/20260816T180541Z-a94aefc2-full/test_firmware_manifest.txt`.
  This F0.9.79 image is a test image under the user's +0.300 ns margin waiver,
  not a promoted release.
- [x] The clean pre-latch F0.9.80 baseline at commit
  `e7fe40a4f3903331c2701c9cb2d5fc811b856882` exported as build
  `20260816T185749Z-e7fe40a4-full`. It is timing-clean with WNS +0.103 ns, TNS
  0, and WHS +0.019 ns. It was intentionally not packaged because the user
  added the latched-selection requirement after the build began. Do not use it
  as the F0.9.80 test image.
- [x] The clean final source and package checkpoint
  `cd8e9b8eb6eacd7f4dabdfdcd04e84af292feb99` exported as full build
  `20260816T193636Z-cd8e9b8e-full`. It reports WNS +0.104 ns, TNS 0, WHS
  +0.037 ns, and WPWS +0.265 ns. Setup, hold, and pulse-width failing
  endpoints, route errors, missing constraint objects, and unconstrained
  internal endpoints are all zero. Bus-skew status passes at +5.957 ns.
- [x] The final candidate DCP SHA-256 is
  `0c57a120686aa2536b34def9453aaaba195eece6ab6dd9d75eccd8451a7db6af`;
  bitstream SHA-256 is
  `d4c2fd7c78db7ba52763b5dc9c5612fdde8f6ae52ff035e0cabf1c6ed418dab3`;
  XSA SHA-256 is
  `5d899fb3d018c121fa19b1127f8ed39c6827e76af8cc10276e3de056e3c14435`.
- [x] A fresh Vitis workspace used that exact XSA. Platform export and all
  application builds report `SUCCESS`; the built strings are F0.9.80 and
  B1.1.0.
- [x] Root `FIRMWARE.BIN` and
  `.timing_runs/20260816T193636Z-cd8e9b8e-full/FIRMWARE_TEST.BIN` are
  byte-identical, each 4,257,036 bytes, with SHA-256
  `43a8eb8dca2816d065dbfbe0bde77ab6fac79b938a4d5321e51c819cdd771f3a`.
  Root/archive `fc /b` passed. Bootgen readback passed with three images and
  three partitions. The handoff record is
  `.timing_runs/20260816T193636Z-cd8e9b8e-full/test_firmware_manifest.txt`.
  This is a test-only package under the user's +0.300 ns margin waiver, not a
  promoted release.
- [x] The clean full nonincremental F0.9.81 build at
  `b33b63176d758ac25d582bdfbdd62717a17484ba` exported as
  `20260816T210350Z-b33b6317-full` with `rescue_used=0`. It reports WNS
  +0.194 ns, TNS 0, WHS +0.061 ns, THS 0, and WPWS +0.265 ns. Route and bus
  skew pass; all setup, hold, pulse-width, route, missing-constraint, and
  unconstrained-internal failure counts are zero.
- [x] Its candidate DCP SHA-256 is
  `dc13f81019146784367f33d44c47e0fe0a2e054159c36770411c9992bb0b38e7`;
  bitstream SHA-256 is
  `2e0e2a948aa647f1e13d4e2774f4d33db31ee239587920a0b50d7d4ef22bea42`;
  XSA SHA-256 is
  `601d18dc1ec0cef12619cf8b0262d41cb813af52ccdecc20db07759f4101275c`.
- [x] A fresh Vitis build used that exact XSA. Platform export and all apps
  report `SUCCESS`; the package contains F0.9.81, B1.1.0, and the
  `196684`-byte CPU1 blob.
- [x] Root `FIRMWARE.BIN` and the archived `FIRMWARE_TEST.BIN` are
  byte-identical, each 4,241,804 bytes, with SHA-256
  `e5a6fa0440b9eee92a1bdc59ad8f90bd5ddb53bc97f88cbea5b57fde47f94964`.
  Bootgen reports three named image headers but four total images and four
  partitions because `frontend.elf` has two load partitions. The handoff file
  is `.timing_runs/20260816T210350Z-b33b6317-full/test_firmware_manifest.txt`.

### Missing Board and End-to-End Proof

- [x] Run the archived F0.9.77 image on an Appletini ONE out of the Apple slot;
  it reached the menu but ONE//e falsely reported `LOCKED`.
- [x] Build and package the corrected F0.9.78 image.
- [x] Program F0.9.78 and start ONE//e out of the Apple slot; it booted the ROM
  and exposed the target, storage-label, and speed-control faults above.
- [x] Build and package the F0.9.79 functional retest image.
- [x] Program and run F0.9.79 out of slot; it exposed the speed, key-repeat,
  and DHGRi faults recorded above.
- [x] Build and package the corrected F0.9.80 functional retest image from
  final source checkpoint `cd8e9b8`.
- [x] Program and run the F0.9.80 functional retest; it exposed the F0.9.81
  work recorded above.
- [x] Build and package F0.9.81 from checkpoint `b33b631`.
- [ ] Program and run the F0.9.81 functional retest.
- [ ] Verify all physical Apple pins and translators while ONE//e starts,
  runs, faults, stops, and returns to host mode, including PL configuration and
  both card/Apple power orders.
- [ ] Verify a live-host event stops ONE//e with no contention or back-power,
  that long later quiet does not restart it, and that only a fresh manual
  selection can run it again.
- [ ] Complete real DOS and ProDOS boots to their OS-ready states through
  virtual Disk II.
- [ ] Check a rendered frame, USB input, paddles, warm reset, speaker, Disk II,
  SmartPort, interrupts, and normal host mode on hardware.

The deepest real-ROM simulations currently enter the first DOS 3.3 and ProDOS
2.4.3 boot sectors at `$0801`. They do not prove either OS-ready state,
multi-track service beyond track 0, a BASIC prompt, rendered DVI output, or
physical safety.
