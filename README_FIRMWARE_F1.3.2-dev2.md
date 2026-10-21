# Firmware F1.3.2-dev2

This development image keeps the USB input parity port from F1.3.2-dev1 and
adds fixed config-menu controls alongside the remappable controls:

- D-pad moves up, down, left and right.
- LB/L1 and RB/R1 select the previous and next tabs.
- A/Cross selects OK; B/Circle goes Back.
- Home/Guide/Xbox/PS opens or closes the whole menu, including child pages.

Fixed controls remain available after changing saved bindings and on the
joystick preview page. Analog stick motion still does not move preview focus.
Both shoulders cancel each other while held together. Home is a one-shot press
and takes priority over other fixed buttons in the same report. Separate Xbox
Guide packets retain the same edge behavior.

A different explicit Gamepad binding runs alongside its fixed action; identical
actions run once. During binding learning, a button supplies its physical source
without also selecting or closing the menu. Mouse/keypad axis devices retain
native controls and explicit bindings. Normal gameplay input stays unchanged.

Both ARM frontend cores rebuild. The package retains the qualified F1.3.1 FPGA,
XSA and FSBL; no HDL changes or FPGA binary rebuild are needed. Golden B2.0.6
stays unchanged. The exact build, test, source and package audit records live
with `FIRMWARE.BIN` in `firmwares/firmware-F1.3.2-dev2/`.

Physical board acceptance remains pending for this exact image.
