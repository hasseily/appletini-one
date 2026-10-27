# USB1 host startup

USB1 serves keyboards, mice, HID axis devices and wired Xbox 360/Xbox One
controllers, including devices behind a USB hub. USB0 has a separate device-side role.

## Startup and connector power

The Zynq EHCI controller and the USB3300 PHY both need host setup. Starting
EHCI and setting its port-power bit alone does not establish the PHY's
VBUS drive and detection state. The frontend now programs and reads back
the USB3300 registers, then enables connector power after EHCI is running.
It checks the PHY's VBUS-valid indication before reporting startup success.

The Appletini ONE board routes USB3300 CPEN to the TPS25221 power switch.
Its EXTVBUS input connects to the switch's open-drain FAULT# signal without
a pull-up. The host uses the PHY's internal VBUS comparator. The startup
settings are Function Control `0x41`, Interface Control `0x10`, and OTG
Control `0x66` after power-on. ULPI access and VBUS waits have time limits;
a failed check returns an error instead of reporting a working host.

On stop or failed startup, the frontend clears the VBUS drive bits before
suspending the PHY. If power-off cannot be verified, it keeps the PHY
accessible for cleanup and reports the failure. This change does not alter
USB0, the PHY reset pin, FPGA logic, or video timing.

The setup follows the same-board fix in
[Multitini commit c2b87fa](https://github.com/hasseily/multitini-one/commit/c2b87fad2913580de296eb9b06a9186b57deeefa).
See also the [Microchip USB3300 data sheet, sections 6.5.3 and 6.5.4](https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/00001783C.pdf).
The adapted source retains its GPL-3.0-only notice.

## UART evidence

A successful PHY startup prints:

```text
[usb1] ULPI 0424:0004 host VBUS valid func=41 iface=10 otg=66
```

That confirms power setup, not device enumeration. A connected hub or HID
device should then produce connection, reset, and configuration events.
`ev=12` is only `USBH_EVENT_INIT`; `settled during prompt, 1 events` by
itself does not show that any device was found.

Use `usb1 status` over the control UART to inspect the service, root port,
and cached PHY results. Reading status does not enumerate devices or run
new ULPI transactions. The cache records startup/shutdown checks; PORTSC
reports the current controller state.

## Standalone ONE//e keyboard controls

In an active standalone ONE//e session, the USB keyboard provides:

- **Ctrl+Delete**: warm reset, like Apple Control-Reset.
- **Ctrl+Alt+Delete**: cold reboot, like Apple Control-OpenApple-Reset.

Both shortcuts use forward Delete, not Backspace. These reset shortcuts apply
only to standalone ONE//e. The added warm-reset shortcut changes ARM firmware
only: rebuild the frontend and `FIRMWARE.BIN`; no HDL or FPGA binary rebuild
is needed.

ONE//e also provides fixed keyboard menu controls:

| Key | Action |
| --- | --- |
| Pause/Break | Open or close the menu |
| Left or right Windows/GUI | Open the menu |
| Arrow keys | Navigate while the menu is open |
| Enter or keypad Enter | Select while the menu is open |
| Tab / Shift+Tab | Next / previous tab while the menu is open |
| Esc | Go back while the menu is open |

Both Shift keys work with Tab. Fixed menu keys take precedence over saved
USB bindings in ONE//e. With the menu closed, arrows, Enter, Tab and Esc
keep their normal Apple input; only Pause/Break and Windows/GUI act as
keyboard menu shortcuts. Holding a saved keyboard OK key does not open or
close the ONE//e menu. Mouse and gamepad OK holds still work.

Left Alt remains Open Apple and right Alt remains Closed Apple. These changes
affect the ARM frontend only. Host tests cover key routing and menu actions;
physical USB-keyboard testing remains pending.

## ONE//e standby screen

A stopped card with a valid, quiet safety guard shows the Appletini logo and
an instruction to enable ONE//e in Boot Settings. Pause/Break or either
Windows key opens the menu with the ONE//e switch focused. The same fixed
arrows, Enter, Tab/Shift+Tab and Esc work in this menu while ONE//e is OFF.
Opening the menu does not turn the mode on or save an ON choice.

Standby uses a separate UI-only input flag. It does not assert the ONE//e
request, grant bus access or change the saved-ON restore and safety-stop
rules. Active Apple-bus signals, a missing guard, a pending restore, vTW
startup and firmware switching suppress standby. An open menu takes display
priority. This is a quiet-card setup hint, not proof that the card is unplugged.

CPU0 draws into the existing RGB565 output slots and shares the menu's decoded
logo cache. Each slot keeps the static screen until a menu, status, output-mode
or screenshot-overlay change needs a redraw. A failed logo decode falls back
to text. Native tests cover all six output sizes and state/input transitions;
physical display and keyboard testing remains pending.

## Validation

For USB joystick input, axis mapping, and paddle timing, see
[USB joysticks and paddles](README_USB_JOYSTICK.md).

```text
python scripts/test_usb1_phy_startup.py
python scripts/test_usb_hid_service.py
python scripts/test_usb_gamepad_input.py
python scripts/test_usb_controller_identity.py
python scripts/test_usb_xbox_360.py
python scripts/test_cherryusb_time.py
python scripts/test_boot_menu_usb_keybindings.py
python scripts/test_onee_usb_controls.py
python scripts/test_onee_input_service.py
python scripts/test_onee_standby.py
```

The PHY regression runs the actual controller glue against a simulated
register interface, including stale warm-start state and injected failures.
It checks power/IRQ order, bounded waits, shutdown, and USB0 isolation.
