# USB1 host startup

USB1 serves the keyboard, mouse, and other HID devices, including devices
behind a USB hub. USB0 has a separate device-side role.

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

## Validation

```text
python scripts/test_usb1_phy_startup.py
python scripts/test_usb_hid_service.py
python scripts/test_boot_menu_usb_keybindings.py
python scripts/test_onee_usb_controls.py
```

The PHY regression runs the actual controller glue against a simulated
register interface, including stale warm-start state and injected failures.
It checks power/IRQ order, bounded waits, shutdown, and USB0 isolation.

The reported Appletini failure was intermittent hub detection at boot,
with only the host-init event; reconnecting the hub did not recover it.
The missing PHY setup is consistent with that report and with the prior
Multitini failure. This Appletini firmware still needs physical checks:
repeat cold boots and warm resets with the affected hub attached, then
check hub reconnect and USB1 refresh. Source and simulated-register tests
do not replace those checks.
