# USB joysticks and paddles

For USB-backed 4Play and SNES MAX in slot 2, see
[Slot 2 gamepad cards](README_SLOT2_GAMEPADS.md). Those interfaces work without
TransWarp; the paddle bridge described below has its own requirements.

USB1 controllers can supply Apple paddle positions and three pushbuttons
only while TransWarp runs the physical Apple II. Connecting a supported
controller selects USB paddle input automatically. Disconnecting it restores the
physical game connector and the existing II/II+ button policy. Native Apple
CPU operation does not use the USB joystick bridge.

Standalone ONE//e keeps its existing input path. Both modes use the same
saved mapping settings. Physical Open/Solid Apple keys still work on a //e:
vTW combines their button reads with USB button presses. On a II/II+ with
forced-zero buttons enabled, USB supplies the button state directly.

## Controller bindings

The USB binding editor accepts **Gamepad 1** through **Gamepad 32** for menu,
screenshot, and Turbo speed actions. Config files store these as
`GAMEPAD.BUTTON1` through `GAMEPAD.BUTTON32`. Existing mouse and keyboard
bindings keep their meaning. An explicit controller binding takes priority
over that button's old mouse-menu action.

Generic HID pads keep the button numbers in their descriptors. Parsing now
preserves nonconsecutive axis usages and each report ID's input offsets;
output and feature reports cannot shift input fields. Simulation trigger
usages feed Z/Rz. A report that updates one group of buttons retains held
buttons from other report IDs. The live joystick preview keeps all 32 bits.
Apple PB0..PB2 and the fixed slot-2 SNES mapping still use their existing
buttons; extra controls cannot alias them.

Wired Xbox One and DualShock 4 use these common binding numbers:

| Gamepad button | Xbox One | DualShock 4 |
| --- | --- | --- |
| 1 | A | Cross |
| 2 | B | Circle |
| 3 | X | Square |
| 4 | Y | Triangle |
| 5 | LB | L1 |
| 6 | RB | R1 |
| 7 | View | Share |
| 8 | Menu | Options |
| 9 | Left stick click | L3 |
| 10 | Right stick click | R3 |
| 11 | Guide | PS |
| 12 | - | L2 switch |
| 13 | - | R2 switch |
| 14 | - | Touchpad click |

Both controllers retain their six analog axes, including the two triggers.
Xbox Guide packets update Guide independently of main button/stick reports.
DS4 report-counter bits never count as buttons.

With the menu open, unbound LB/RB (L1/R1) select the previous/next tab. Both
held together cancel tab movement; holding one does not repeat. Learning a
binding or viewing joystick calibration suppresses these tab shortcuts, and
normal gameplay keeps its existing controller input. Assigning either shoulder
an explicit binding disables the default shoulder pair's tab shortcut.

The new descriptor helper derives from Multitini One commit `1c4f8be`.
Host tests exercise real parsing, 32 separate binding sources, partial reports,
button arrays, malformed descriptors, shoulder edges and capture, PS4 controls,
Xbox Guide/main packet ordering, and the unchanged Apple PB/SNES outputs.

## Set up a controller

Open **USB > Joystick / Paddles** in the config menu. Select a paddle from
Paddle 1 through Paddle 4, then adjust:

- **Device:** Auto or USB input 1 through 8. Each paddle can use a different
  input. Auto selects the lowest connected joystick slot.
- **Source:** Auto, X, Y, Z, Rx, Ry, Rz, or Off.
- **Invert:** reverse that paddle's direction.
- **Sensitivity:** 25% through 200%, with 100% as the default. Higher values
  make the same movement have a larger effect. Below 100%, even full travel
  cannot reach the maximum paddle value.
- **Deadzone:** 0% through 50% of the travel from center to an endpoint.

Source Auto maps Paddle 1/2 to X/Y and Paddle 3/4 to Rx/Ry on each paddle's
selected device, falling back to Z/Rz when those rotation axes are absent.
Missing devices, missing axes, and Off return the center value 128. An explicit
device selection does not fall back to a different connected slot.
Defaults use Auto for both device and source, no inversion, 100% sensitivity,
and no deadzone. This preserves the earlier standalone ONE//e mapping. Use zero deadzone for a
paddle that needs continuous movement through its center.

The page shows the selected input's raw axes and connection state, all four
mapped paddle values, and buttons. Compact menus group the preview into three
rows so all six raw axes, four mapped values, and buttons stay visible at
1024x768 while changing settings.
A separate status line shows whether USB controls are available with TransWarp
or in standalone ONE//e. If TransWarp is not active, you can still test and
adjust the controller, but it cannot control games on the Apple II. To enable
TransWarp, restart, press A during boot, open **TransWarp**, and turn on
**Accelerate the Apple II**. The page gives this guidance when TransWarp is
off, and distinguishes a saved enabled setting from active TransWarp.
Help explains the selected setting; the USB tab also states the requirement.
Axis and hat movement does not navigate the menu while this page is open;
keyboard, mouse, and buttons remain
available. **Restore defaults** resets all four mappings. Settings use the
normal config save and profile flows, with keys under
`usb.joystick.paddle.0` through `usb.joystick.paddle.3`.

While the config menu captures input, a connected joystick supplies centered
paddles and released buttons to the Apple. The preview still tracks movement.
Closing the menu restores the current position without requiring a new report.

Each of the four paddles can use any of the six supported axes on any of the
eight shared input slots: up to 48 axis sources. Multiple paddles may also use
the same device or axis. For two sticks, for example, set Paddle 1/2 to X/Y
on one USB input and Paddle 3/4 to X/Y on the other. Four separate controllers
can each supply one paddle.

The input numbers identify shared USB slots, not saved hardware identities.
Keyboard and mouse HID interfaces use slots too. The numbers follow USB
connection order and may change after a different startup or reconnect order;
use the raw preview to identify each stick before saving the mapping.
Settings store `usb.joystick.paddle.N.device` as `Auto` or a number from 1 to 8
in both global settings and profiles. Older settings without that key retain
Auto. The selected slot's absence centers only its mapped paddles; other
selected slots continue updating.

The lowest connected joystick slot still owns PB0 through PB2. If it
disconnects, the next controller supplies the buttons and any paddles whose
device is Auto. Axis-only and button-only
reports retain the values omitted from that report. HID detection accepts
two absolute axes or a D-pad hat; relative mice and a lone throttle are
not selected as joysticks. HID devices and supported vendor gamepads share
eight input slots. A new HID device cannot replace a connected vendor gamepad
just because its CherryUSB minor number matches that slot.

## Wired Xbox One controllers

The EG-C50700X reports USB ID `045e:02ea` and Xbox One interface class,
subclass, and protocol `ff/47/d0`. The USB1 host binds its gamepad interface
(interface 0), sends the Xbox One S startup commands, and reads both sticks,
triggers, buttons, and D-pad. The separate Home/Guide report gets the required
acknowledgement. Audio and accessory interfaces remain unsupported.

This driver currently matches `045e:02ea` only. It does not enable CherryUSB's
Xbox 360 driver: that driver uses a different USB protocol. The packet format
and startup sequence follow [Linux xpad](https://github.com/torvalds/linux/blob/master/drivers/input/joystick/xpad.c)
and the [Xbox One protocol notes](https://github.com/quantus/xbox-one-controller-protocol).

The sticks map to X/Y and Rx/Ry, with up as the lower Y value. The triggers
map to Z/Rz. Button bits 0 through 7 map to A, B, X, Y, LB, RB, View, and
Menu; the first three supply PB0 through PB2. The D-pad navigates menus.
Guide and stick clicks also count as held input for the input-release guard.
The existing paddle mapping, preview, menu capture, and TransWarp requirements
apply to these controllers.

A connected controller prints:

```text
[usb1] Xbox One connected slot=... vid=045e pid=02ea intf=0
[usb1] Xbox One input active slot=... joystick=1
```

`usb1 status` includes the startup count, input report count, transfer errors,
and pending transfers for each Xbox controller. The driver retries failed
submissions after a short delay and keeps output buffers stable until each
transfer ends. If an endpoint stalls, the polling path clears its USB halt
before retrying. Disconnect releases its input slot and cancels both transfers.

## Wired PS4 controllers

Sony DualShock 4 USB IDs `054c:05c4` and `054c:09cc` use the existing HID
driver with a decoder for their 64-byte input report 1. Basic input needs no
Xbox-style startup commands. Other HID devices keep their descriptor-based
input path. The layout follows the [TinyUSB DS4 host example](https://github.com/hathach/tinyusb/blob/master/examples/host/hid_controller/src/hid_app.c).

Both sticks map to X/Y and Rx/Ry; L2/R2 map to Z/Rz. This avoids mixing the
right stick with the triggers when the generic parser reads the DS4's
nonconsecutive axis usages. Cross, Circle, and Square supply PB0 through PB2.
Triangle, L1, R1, Share, and Options fill the remaining button bits. The D-pad
navigates menus; PS, touchpad click, and stick clicks count for the release
guard. Motion sensors, touch coordinates, rumble, and audio are not mapped.

The connection line includes `joystick=1 ps4=1`. Check both sticks and
triggers under **USB > Joystick / Paddles**, then confirm disconnect releases
the buttons and restores the next controller or the physical game connector.

## Paddle timing and soft switches

USB paddle reads use `$C064-$C067` and their `$C06C-$C06F` mirrors. USB
buttons use `$C061-$C063` and `$C069-$C06B`. Cassette input and writes to
the `$C06x` range keep their physical behavior.

Every `$C070-$C07F` access restarts the four local paddle timers. `$C070`
can complete privately; the remaining aliases still reach the physical bus
and keep their RamWorks and TransWarp speed effects. An axis value selects
`4 + 11 * value` native Apple cycles, matching ONE//e. Timers continue while
the CPU is paused, and each trigger takes a fresh snapshot of the axes.

Actual USB paddle measurement runs at native speed, including TURBO and
presets with the optional **Slow Paddles/joystick** setting disabled. Exact
`$C070` or a paddle read opens this measurement window; incidental bank or
speed writes alone do not slow execution. Normal speed resumes once the
timers and current paddle access finish.

## ARM register interface

Both registers use the CARD_CTRL block at `0x40000000`, with four bytes per
register index. This bridge is separate from ONE//e keyboard/reset controls.

| Index | Address | Function |
| --- | --- | --- |
| `$AB` | `0x400002AC` | Stage PDL0..3 in low-to-high bytes; honor byte strobes. |
| `$AC` | `0x400002B0` | Commit staged axes with presence bit 0 and PB0..2 in bits 3:1. |

A control write requires the low-byte strobe. Clearing presence releases
USB input. Control readback has signature `$4A` in bits 31:24, the physical
vTW enable in bit 8, and live presence/buttons in bits 3:0. Firmware checks
the signature and enable before publishing, so an older FPGA image cannot
accept these updates. Mode exit masks outputs immediately and clears saved
state; a new session republishes the current controller state.

## Validation

Focused checks cover register writes, mode boundaries, real CPU paddle
polling at each speed, physical buttons, address aliases, axis transforms,
device handoff, menu preview, and config persistence:

```text
python scripts/test_vtw_joystick_bridge.py
python scripts/test_vtw_usb_joystick.py
python scripts/test_vtw.py
python scripts/test_onee_input_service.py
python scripts/test_usb_hid_service.py
python scripts/test_usb_xbox_one.py
python scripts/test_usb_gamepad_input.py
python scripts/test_usb_ps4.py
python scripts/test_multi_joystick_axes.py
python scripts/test_joystick_config_menu.py
```
