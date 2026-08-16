# ONE//e Stand-Alone Mode Plan

Branch: `feature/self-contained-one-iie`
Baseline: `a4c2d22fc6029263f61a5cfe1178a2ab3c6ef762`

## Goal

Add a self-contained 128K Enhanced Apple //e mode that runs on Appletini ONE
without using a powered Apple motherboard. The first supported setup uses DVI,
USB keyboard, USB joystick, the existing audio outputs, and only Appletini's
virtual slot devices.

The mode reuses the current virtual TransWarp CPU, shadow RAM and ROM, Apple bus
records, virtual-card decoders, video renderer, and audio path. It replaces the
physical motherboard's timing, built-in I/O, and slot-pin signals with a virtual
motherboard.

## Required Product Behavior

- ONE//e mode starts off after every card boot and reset.
- The user starts it manually from the boot menu. It is a session action, not a
  saved setting that can start on its own.
- Starting ONE//e also starts the virtual TransWarp core. ONE//e has no native
  physical-CPU mode.
- Any sign of a live Apple on the edge connector disables ONE//e at once.
- Apple activity latches a lockout. The mode stays off after the signal stops.
  The user must return to the boot menu and select ONE//e again.
- A manual selection succeeds only after the Apple inputs have stayed quiet for
  a defined guard time.
- No ONE//e path may drive the physical Apple address, data, control, reset,
  interrupt, or DMA pins.
- Slot 6 uses only the virtual Disk II. ONE//e never uses a physical Disk II
  controller, cable, or drive.
- Existing virtual cards receive virtual equivalents of the Apple slot signals
  instead of private device-specific CPU connections wherever practical.
- USB keyboard input supplies the Apple keyboard latch, modifiers, Reset, Open
  Apple, and Closed Apple.
- USB joystick axes supply four Apple paddle values. Its buttons may supply the
  three pushbutton inputs, but keyboard Open/Closed Apple remain supported.
- The Apple `$C030-$C03F` speaker toggle is mixed into the current digital audio
  stream.

## Safety Contract

Safety takes priority over starting or keeping ONE//e running.

There are three separate controls:

1. `one_iie_request`: the user's current manual request.
2. `apple_activity_latched`: sticky evidence that the Apple side became live.
3. `one_iie_effective`: the only signal allowed to enable the virtual machine.

`one_iie_effective` must fall without waiting for firmware when any monitored
Apple-side input shows activity. The same raw activity signal must block all
physical output enables. Synchronized logic then records the event, clears the
session request, and reports the lockout reason to firmware.

The first activity detector will monitor all available motherboard timing and
control inputs, including PHI0, 7M, Q3, reset, DMA, IRQ, NMI, RDY, device
selects, and machine-mode inputs. Clock activity is the main powered-machine
test; control inputs add coverage. A quiet counter prevents re-entry during a
short gap between Apple clock edges.

RTL activity sensing does not replace electrical protection. Before hardware
use, the physical bus translators must have a fail-safe output-disable path that
works during PL reset and while the Apple slot is unpowered. The current fixed
translator-enable connections must not remain active in the completed mode.
This safety promise requires a real slot-power sense that gates the translator
output enables in hardware, unless board tests prove an existing independent
signal provides the same protection. The PL also records that signal and stops
the virtual machine. Until the hardware gate is present and proved, ONE//e may
run only with the edge connector physically disconnected from an Apple or on a
purpose-built safe carrier.

## Architecture

### 1. Stand-Alone Supervisor

The supervisor owns:

- Manual request and cancel commands from the control register bank.
- Apple-activity detection, sticky lockout, and quiet qualification.
- Immediate `one_iie_effective` kill.
- Stand-alone cold reset and warm reset.
- Status and reason registers for the boot menu and UART.
- A hard `physical_bus_isolate` output used by every edge-connector driver.

Firmware may request a state change, but firmware polling never forms part of
the safety path.

Use card-control offset `0x5B` for the first control/status register. It is free
between the current SDD readbacks and machine-mode registers. A write to bit 0
is the manual session request. The proposed readback is:

- Bit 0: request.
- Bit 1: effective enable.
- Bit 2: live Apple activity.
- Bit 3: activity lockout.
- Bit 4: connector quiet.
- Bit 5: reselect armed.
- Bit 6: ONE//e selected.
- Bit 7: physical bus isolated.
- Bits 10:8: inhibit reason.

Activity has priority over a same-cycle ARM write. No startup, profile-load,
generic apply, or polling path may write request bit 0 high.

### 2. Virtual Motherboard Timing

Generate the Apple native cadence from a local fabric clock. It must provide the
same phase strobes used by `globals::AppleBus_read`:

- `drive_en`
- `addr_en`
- `sss_en`
- `serve_en`
- `data_en`

The generator also provides the 65-cycle line count, NTSC/PAL frame count, VBL,
and scanner address. It continues to run while the accelerated CPU is stopped
or executing only private RAM cycles.

### 3. Virtual Slot Pins

Create one synthetic `globals::AppleBus_read` record for the virtual
motherboard. Existing cards consume this record as though it came from
`apple_bus_wrapper`.

The virtual motherboard derives the same slot-facing selects from the CPU
address and current soft-switch state:

- Per-slot device select for `$C080-$C0FF`.
- Per-slot ROM select for `$C100-$C7FF`.
- Shared expansion-ROM selection for `$C800-$CFFF`.
- Slot `$C800` claim and `$CFFF` release.
- `INTCXROM`, `SLOTC3ROM`, and internal slot-3 behavior.

Existing `globals::AppleBus_write` responses go through the current response
arbiter. In ONE//e mode, the selected data returns to the soft CPU internally;
it never enables a physical data-bus driver. Open-drain IRQ, NMI, and RDY
requests become local CPU inputs. Any future virtual bus master must use an
internal arbiter and may not reuse physical `/DMA` ownership.

The first integration splits `apple_top` into physical and virtual bus records:

- `physical_ab_read` comes only from `apple_bus_wrapper`.
- `virtual_ab_read` comes only from `apple_virtual_bus`.
- ONE//e selects `virtual_ab_read` for the soft-switch manager, capture path,
  virtual cards, and TransWarp bus engine.
- The current merged `AppleBus_write` feeds `apple_virtual_bus` internally.
- A separately masked write record feeds `apple_bus_wrapper`; it is all zero
  while physical isolation is active.

The existing TransWarp bus engine can act as the first virtual bus master. Its
address, R/W, data, and `/DMA` requests already use `AppleBus_write`, so the
stand-alone path need not add a private CPU-to-card bus.

This shared bus is the main compatibility boundary. Private short paths may
remain only when they produce the same visible slot behavior and timing.

### 4. Built-In Motherboard I/O

Add an IOU/MMU responder on the virtual bus for:

- `$C000` keyboard data and strobe.
- `$C010-$C01F` keyboard clear and IIe status reads.
- `$C020-$C02F` cassette output state.
- `$C030-$C03F` speaker toggle.
- `$C040-$C04F` utility strobe and VBL interrupt controls.
- `$C050-$C05F` video switches and annunciators.
- `$C060-$C063` cassette input and pushbuttons.
- `$C064-$C067` paddle timers.
- `$C070-$C07F` paddle trigger aliases, IOUDIS, DHIRES status, and RamWorks
  register coexistence.
- Floating-bus data on every unclaimed read.

The existing private TransWarp soft-switch state and address translator remain
the authority for memory banking. New logic should extend them rather than
create a second set of IIe memory state.

### 5. CPU, Memory, and Boot

ONE//e forces the supported machine identity to a 128K Enhanced US Apple //e.
It uses the existing 65C02 core, 64K main shadow, 64K auxiliary shadow, 16K ROM
shadow, and built-in character ROM.

The boot service gets a separate stand-alone path:

1. Verify the supervisor is quiet and unlocked.
2. Assert local reset and physical isolation.
3. Initialize cold-start state and load the Enhanced IIe ROM.
4. Select virtual slot 6 Disk II or the requested virtual boot target.
5. Enable and release the TransWarp core without machine detection, physical
   DMA ownership, physical reset, or native slot-7 handoff.
6. Stop at once if `one_iie_effective` falls.

Do not force the existing external-host machine-mode register to IIe. That
register also permits physical INH and DMA behavior. The ONE//e identity and
TransWarp gate stay in their own control plane.

Implement this as an explicit `vtw_service` stand-alone start mode or a small
ONE//e service which calls shared ROM-load helpers. Do not call the current
normal vTW enable path unchanged: it waits for host identification, physical
slot-7 handoff, `/DMA` ownership, and physical RESET. The PL request register
also clears itself on Apple activity; ARM then cancels its session intent rather
than writing the request high again.

Warm Reset preserves RAM. A new ONE//e session performs a defined cold start so
old `$03F3/$03F4` values cannot skip initialization.

### 6. Video

Feed virtual native-cycle records into the existing capture and renderer path.
Posted screen writes, soft-switch changes, scanner position, VBL, and frame
markers must come from the virtual motherboard. This keeps current raster and
mid-frame switch support without adding a second renderer.

### 7. USB Keyboard and Joystick

USB firmware translates HID key reports to Apple key codes and sends events to
a small PL keyboard FIFO/latch. PL owns the software-visible `$C000/$C010`
state. Firmware supplies modifier state, including Open Apple and Closed Apple,
and requests local Reset for the correct key chord.

USB joystick firmware publishes normalized axes and buttons. PL snapshots axis
values when software accesses a paddle-trigger address, then expires each
paddle against the native Apple cycle clock. This preserves timing-based paddle
reads even when the CPU runs faster than 1 MHz.

### 8. Speaker

Every `$C030-$C03F` access toggles a one-bit speaker state. An edge accumulator
or equivalent resampler converts native-cycle transitions to the existing audio
sample rate. The result enters the current mixer beside Mockingboard and Disk II
audio and follows the existing output volume and mute path.

### 9. Virtual Disk II

In ONE//e mode slot 6 always targets the existing virtual Disk II card. Complete
the current private path so all slot-6 reads, writes, soft switches, sequencer
timing, motor state, write data, WOZ timing, boot ROM, and drive sound work from
the virtual bus. Physical Disk II access does not exist in this mode.

## Delivery Stages

### Stage 0: Safety and Control

- [ ] Add and test the stand-alone safety supervisor.
- [ ] Add control/status registers with sticky lockout reason.
- [ ] Route one common isolation gate to all physical Apple outputs.
- [ ] Prove reset defaults and raw-activity kill in simulation.
- [ ] Resolve the board-level slot-power/interlock requirement.

### Stage 1: Virtual Bus Skeleton

- [ ] Generate local Apple bus phases and scanner cadence.
- [ ] Create the synthetic `AppleBus_read` record.
- [ ] Return virtual-card arbiter data to a test bus master.
- [ ] Verify slot I/O, slot ROM, expansion ROM, IRQ, and floating-bus cases.
- [ ] Keep every physical driver disabled throughout the test.

### Stage 2: CPU and Boot Menu

- [ ] Add a session-only row 2 on Boot Settings, between Boot Device and the USB
  binding section: `ONE//e standalone: OFF / RUNNING / LOCKED - APPLE ACTIVE`.
- [ ] Keep the action out of saved settings and profiles.
- [ ] Refuse start while activity is present or latched.
- [ ] Start TransWarp automatically through the stand-alone boot path.
- [ ] Stop and lock out on simulated Apple activity.
- [ ] Cold-boot the Enhanced IIe ROM to BASIC over the virtual bus.

Planned item help:

> Runs the built-in Enhanced Apple //e on Appletini's soft 65C02 without an
> Apple host. This mode is session-only and starts off after every card boot.
> Any Apple-bus activity stops ONE//e and keeps it off. After the connector is
> quiet, select this item again.

### Stage 3: Video and Built-In I/O

- [ ] Feed the renderer from virtual cycles.
- [ ] Implement complete keyboard and modifier behavior.
- [ ] Implement video/status switches, VBL, and floating bus.
- [ ] Implement USB paddles, buttons, and annunciators.
- [ ] Mix the motherboard speaker into current audio.

### Stage 4: Storage and Cards

- [ ] Finish virtual Disk II slot-6 reads, writes, and timing.
- [ ] Boot DOS and ProDOS disk images.
- [ ] Run SmartPort and each enabled virtual card on the synthetic slot bus.
- [ ] Add internal arbitration or explicitly block virtual bus-master cards.

### Stage 5: Hardware Safety and Compatibility

- [ ] Prove all Apple connector pins remain high impedance in ONE//e mode.
- [ ] Test Apple power-on while ONE//e runs: immediate stop, no pin contention,
  no back-power, sticky lockout, and manual-only restart.
- [ ] Test Apple already on when the user selects ONE//e: request refused.
- [ ] Test normal Appletini mode after every ONE//e fault and reset case.
- [ ] Run 40/80-column text, lores, hires, double-hires, raster, paddle,
  speaker, Disk II, IRQ, warm-reset, and cold-reset compatibility tests.

## First Implementation Increment

The first branch increment contains only logic that cannot enable the feature:

1. A stand-alone safety guard with a sticky Apple-activity lockout.
2. Source-level and RTL tests for default-off, immediate kill, quiet-time
   qualification, and manual reselect.
3. A virtual-bus scaffold or interface contract that reuses `AppleBus_read` and
   `AppleBus_write` without connecting it to physical pins.

Boot-menu activation comes only after those tests pass and the common physical
isolation gate exists.

## Current Branch Status

Implemented, but deliberately not connected to the top level:

- `onee_mode_safety_guard.sv`: default-off manual selection, direct slot-power
  veto, activity monitoring, immediate virtual-machine kill, sticky lockout,
  quiet-time reselect, and a separate sticky physical-isolation output.
- `apple_virtual_bus.sv`: free-running Apple bus phases, idle native cycles,
  the shared `AppleBus_read`/`AppleBus_write` contract, card read responses,
  floating-bus fallback, RDY replay, and virtual DMA ownership.
- Focused RTL benches and Python launchers for both modules.

The branch does not yet expose a menu action, alter `apple_top`, enable the soft
CPU, or drive any connector pin. This is intentional: top-level activation is
blocked until the connector isolation path and real slot-power input exist.
