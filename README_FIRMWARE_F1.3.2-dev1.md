# Firmware F1.3.2-dev1

This development image ports the shared USB input handling from Multitini One
commit `6f2a90286956bf8e4956dfc9b94c94dfc3335b05` to Appletini.

- Wired Xbox 360/XInput enumeration, full analog input, player LED, retries,
  diagnostics and disconnect handling.
- Collection-aware HID parsing, compact keyboard bitmaps, held keys across
  report IDs, short-packet checks and checked boot/report protocol selection.
- Ten selectable axis sources: X, Y, Z, Rx, Ry, Rz, Slider, Dial, Wheel and Pan.
  Relative motion expires; wheel and dial bursts retain bounded detents.
- Saved device identities keep explicit paddle mappings stable across USB slot
  order changes. Serial-bearing devices can move between ports; devices without
  serial numbers use their saved hub path. Missing devices stay centered.
- Product names and descriptor details help identify devices in the menu and
  `usb1 status` output.
- A macro-safe USB millisecond clock keeps retries and timer deadlines correct.

Generic mouse and keypad axes start unassigned. They need an explicit paddle
mapping and do not take Auto, Apple pushbutton or slot-2 gamepad ownership.
Their normal keyboard/mouse behavior and explicit button bindings still work.
Existing four-way hats, HID button arrays, Xbox One/DS4 controls, warm/cold
keyboard reset shortcuts and saved source values retain their meaning.

The change affects ARM firmware only. Build both ARM frontend cores and package
the image with the qualified F1.3.1 FPGA/XSA/FSBL inputs. No HDL changes or FPGA
binary rebuild are required. Golden B2.0.6 stays unchanged.

The firmware package contains `FIRMWARE.BIN`, exact input copies, source archives,
`ARM_BUILD.json`, `FIRMWARE_AUDIT.json`, checksums and validation records. Read its
`README.md` and `TEST_RESULTS.md` for the exact hashes and completed checks.

This image has not been installed or checked on an Appletini board. Validate
ONE//e input with the actual Xbox 360 controller, hotplug and disconnect while
held, a mixed keyboard/mouse/controller hub, menu capture, saved identity reload
and relative-axis expiry. Host tests and the package audit provide separate
evidence from those board checks.

See [USB joysticks and paddles](README_USB_JOYSTICK.md) for settings and mapping
behavior, and [USB1 startup](README_USB1.md) for power and UART checks.
