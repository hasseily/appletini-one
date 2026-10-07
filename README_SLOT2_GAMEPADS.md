# Slot 2: Mouse, 4Play and SNES MAX

The `four-play` branch adds two USB-backed gamepad cards to slot 2. In the
**Slot 2** menu, choose **Off**, **Mouse**, **4Play**, or **SNES MAX**. Only the
selected card responds. Mouse sensitivity stays saved while another card is
selected. Reapplying other menu settings does not reset the mouse.

These cards need the updated FPGA and frontend together. They work through
the normal Apple bus and the private ONE//e bus, without requiring TransWarp.
The existing physical IIgs slot restrictions still apply: slot 2 remains
disabled on a physical IIgs host. ONE//e keeps its separate slot policy.

## Controllers and player choices

4Play has four player ports. SNES MAX has two player ports and the full twelve
SNES controls per player. Each visible player offers **Auto**, **USB 1–8**,
or **Off**.

Auto assigns available gamepads in report-arrival order and keeps each
assignment while that pad stays connected. Unplugging a pad releases its
player's controls; the remaining players keep their assignments. A spare pad
may fill the empty port. SNES MAX uses only two assignments, so hidden 4Play
player choices do not reserve pads. An explicit USB choice never falls back
to a different device. Two players may deliberately select the same device.

USB numbers refer to shared input slots, including keyboard and mouse
interfaces. They are not persistent controller identities and may change with
connection order. `usb1 status` shows the current slot-2 card, FPGA support,
input blocking, player presence, resolved USB numbers, and SNES button words.
A displayed USB number of zero means no assigned device.

Supported wired Xbox One and PS4 controllers use their existing normalized
button layout. Generic HID gamepads use the first eight HID button usages;
their physical labels can vary by device. Hats work with zero- or one-based
eight-way ranges and four-way ranges. Hat-only digital gamepads are accepted.
The D-pad takes priority over the left stick; otherwise, the middle third of
each stick axis is neutral.

| Physical control | SNES MAX | 4Play |
| --- | --- | --- |
| Bottom face button (Xbox A / PS Cross) | B | Trigger 1 |
| Right face button (Xbox B / PS Circle) | A | Trigger 2 |
| Left face button (Xbox X / PS Square) | Y | Trigger 3 |
| Top face button (Xbox Y / PS Triangle) | X | — |
| Left / right shoulder | L / R | — |
| View / Share, Menu / Options | Select, Start | — |
| D-pad, or left stick outside its neutral range | Directions | Directions |

Opening the menu releases controls delivered to the Apple while USB reports
continue updating saved input state. Closing the menu restores input. In
ONE//e, release held controls first so the paused machine can resume. The
existing USB paddle mapping remains a separate setting.

## Apple software interface

The implementation follows the author's
[4Play Rev B specification](https://lukazi.blogspot.com/2016/05/apple-ii-4play-joystick-card-revb.html)
and schematics, plus the
[SNES MAX hardware and diagnostics](https://lukazi.blogspot.com/2021/06/game-controller-snes-max-snes.html).
They are separate interfaces; software must support the chosen card.

4Play reads `$C0A0` through `$C0A3` for players 1–4. Bits 0–3 hold
Up/Down/Left/Right, bit 4 holds Trigger 3, bit 5 stays high, and bits 6–7
hold Trigger 2/1. Controls are active high. An idle or disconnected port reads
`$20`. The hardware decodes A0/A1, so these four ports repeat across `$C0A0–AF`.
Writes have no effect.

SNES MAX writes to `$C0A0` to latch both players, then reads D7/D6 for players
1/2. Writes to `$C0A1` advance both serial streams. Reads do not advance them.
Bits are active low, in this order: B, Y, Select, Start, Up, Down, Left, Right,
A, X, L, R, then four high bits. Position 16 reads low for a connected pad and
high for an empty port. Further clocks remain at that presence value. A latch
captures buttons and presence together, so USB updates cannot split a serial
read across reports. The card decodes only A0 for writes; all even write
offsets latch, all odd offsets clock, and all sixteen offsets can be read.

Neither gamepad card supplies slot ROM, expansion ROM, IRQ, or no-slot-clock
access. Choosing another card clears pending mouse state. Off leaves slot 2
unclaimed. Games with a fixed slot-4 address need a slot-2 setting, autodetection,
or a patch; selecting the card alone cannot change those game addresses.
See the author's [SNES MAX software notes](https://lukazi.blogspot.com/2021/09/game-controller-snes-max-software.html).

## Settings and FPGA registers

Global settings and profiles save `slot2.card` as `OFF`, `MOUSE`, `FOUR_PLAY`,
or `SNES_MAX`, and `slot2.player1.device` through `slot2.player4.device` as
`AUTO`, `1`–`8`, or `OFF`. Fresh settings default to Off and Auto. Old
`mouse.slot2.enabled` settings still select Mouse or Off. An explicit new
card key wins regardless of file order. New files also save the legacy mouse
key as ON only for Mouse, for safe use with older firmware. Config schema is 120.

The new PS registers use the existing fabric clock and need no new clock
crossing or placement constraint:

| Register index | Address | Function |
| --- | --- | --- |
| `$AD` | `$400002B4` | Commit staged state, card choice bits 1:0, presence bits 7:4 |
| `$AE` | `$400002B8` | Stage players 1/2 in low/high halfwords |
| `$AF` | `$400002BC` | Stage players 3/4 in low/high halfwords |

Each halfword uses bits 0–11 for active-high SNES controls in serial order.
A byte-0 write to CONTROL commits both staged words and selection together.
CONTROL reads return signature `$5332` in bits 31:16. Firmware checks that
signature before using these registers; an older FPGA can still use Mouse
but cannot enable either gamepad card. FPGA reset selects Mouse for older
firmware compatibility; the existing slot enable mask still gates access.

## Validation

The focused RTL simulation checks slot decoding, address aliases, all buttons,
four independent players, SNES snapshots and presence, atomic PS publication,
mouse switching, disabled-slot behavior, and pending read/IRQ cleanup. Existing
mouse, ONE//e bus/isolation, and IIgs safety regressions also pass.
An integration test compares the original and captured data inputs through
real Mouse and boot-menu transactions, including late replies and reset.

Native C checks cover the production gamepad service, HID/vendor input paths,
partial reports, disconnects, short and full-width axis ranges, menu blocking,
stable player assignments, old-FPGA rejection, settings migration and profile
loads. Menu renders pass at all six supported resolutions.

Run the focused tests with:

```text
python scripts/test_slot2_card.py
python scripts/test_slot2_data_phase.py
python scripts/test_slot2_gamepad_service.py
python scripts/test_slot2_runtime_control.py
python scripts/test_slot2_config_menu.py
python scripts/test_usb_gamepad_input.py
python scripts/test_usb_hid_service.py
python scripts/test_usb_ps4.py
python scripts/test_usb_xbox_one.py
```

For further 4Play checks, first confirm that
`PEEK(49312)` through `PEEK(49315)` return 32 at rest, then test each player's
directions and fire buttons. For SNES MAX, use its diagnostic with slot 2
selected. Test unplug/reconnect, two simultaneous pads, menu entry/exit, and
Mouse → gamepad → Mouse switching with the Apple program restarted as needed.
