# Apple IIgs safety profile

Firmware F1.0.2 uses a strict physical-bus policy for Apple IIgs use. Install
the card in physical slot 7 and set slot 7 to **Your Card** in the IIgs Control
Panel.

## Why the profile is strict

Pin 39 is active-high processor `SYNC` on an Apple IIe and active-low
`/M2SEL` on an IIgs. Before the host reports its type, a high pin cannot prove
that an apparent slot address is valid. F1.0.2 therefore answers only the
normal low-pin boot signature scan. A selected low-pin `$C700` fetch locks a
provisional IIgs state and enables active-low `/M2SEL` checks before the boot
ROM calls fast IIgs firmware.

This keeps the IIgs slot-7 boot path, but it means that F1.0.2 cannot
auto-start on a physical IIe: the IIe `$C700` opcode fetch has `SYNC` high.
ONE//e is isolated from the physical bus and keeps its normal virtual-card
behavior.

## Physical IIgs features

The safe physical interface is limited to slot 7. Slot-ROM and C8 reads require
active-low `/M2SEL`; `$C0F0-$C0FF` reads and writes also require the live,
active-low `/DEVSEL` input. The boot ROM records the IIgs `$C02D` slot setting,
and the firmware keeps optional slot-7 services off unless bit 7 says
**Your Card**.

If the IIgs later maps slot 7 back to its internal ROM, `/M2SEL` and
`/DEVSEL` stop selecting the card. The final pad gates then release C7, C8,
and C0F data without relying on the cached boot report.

- The boot menu and SmartPort boot/service remain available in slot 7.
- SuperSprite register access works in polling mode when SuperSprite is
  enabled.
- The linear text overlay register block works when SuperSprite is disabled.
- The no-slot clock may use the selected slot-7 ROM path.
- Passive Apple-bus capture and video output do not drive the Apple bus.

The firmware disables these physical-host features in the strict profile:

- `/INH` memory replacement, including aux memory and RamWorks
- `/DMA`, address/R/W drive, vTW, AD8088, and PS host-memory commands
- logical slot 1-6 cards
- fake SHR and firmware writes to the IIgs `$C029` register

`RDY` and `/NMI` remain high-impedance. The strict slot-7 path passes only the
data byte of a live selected read; it does not grant DMA, address, reset, or
interrupt output rights.

## Release checks

Before a hardware release:

1. Run `python scripts/test_iigs_bus_safety.py` and
   `python scripts/test_iigs_ps_policy.py`.
2. Run the full `scripts/test_*.py` set and the Vivado timing checks.
3. Build a fresh bitstream and PS image, then package and audit
   `FIRMWARE.BIN`.
4. On a real IIgs, scope `/INH`, `/DMA`, address direction, data direction,
   `PHI0`, `/M2SEL`, and `/DEVSEL` through cold boot, warm reset, 1 MHz mode,
   and fast mode. `/INH`, `/DMA`, and address direction must never assert.
   Data direction may assert only on a selected slot-7 read and must release
   when `PHI0` falls.

The automated tests and routed timing report do not replace this final scope
check on production hardware.
