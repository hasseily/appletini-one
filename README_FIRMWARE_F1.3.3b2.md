# Firmware F1.3.3b2

F1.3.3b2 adds fixed USB keyboard menu controls in standalone ONE//e.
Connect the keyboard to USB1. Saved USB bindings do not need to be set up
for these controls.

| Key | Action |
| --- | --- |
| Pause/Break | Open or close the menu |
| Left or right Windows/GUI | Open the menu |
| Arrow keys | Navigate while the menu is open |
| Enter or keypad Enter | Select while the menu is open |
| Tab / Shift+Tab | Next / previous tab while the menu is open |
| Esc | Go back while the menu is open |

Both Shift keys work with Tab. Fixed menu keys take precedence over saved
bindings in ONE//e. With the menu closed, navigation keys keep their normal
Apple input. Only Pause/Break and Windows/GUI act as keyboard menu shortcuts;
holding a saved keyboard OK key no longer opens or closes the ONE//e menu.
Mouse and gamepad OK holds still work. Left Alt remains Open Apple and right
Alt remains Closed Apple.

The HID service keeps keyboard press history when the menu opens or closes.
A held Pause/Break or Windows key cannot fire again just because the menu
state changed. Fixed navigation runs once per press and cannot also run a
conflicting saved action. Boot Settings and its help show the new controls.

This ARM-only update retains the F1.3.3b1 stack fix: a 64 KiB frontend stack,
a 64 MiB heap, and linked-ELF checks before packaging. It keeps the exact
F1.3.2 FPGA bitstream, XSA, FSBL and CPU1 image used by F1.3.3b1. Golden
B2.0.6 stays unchanged. No FPGA rebuild is needed.

Host regressions cover fixed-key decoding, real HID report routing, saved
binding conflicts, menu actions and tab changes, held-key transitions,
ONE//e reset and pause/release behavior, gamepad controls and screenshots.
Physical USB keyboard and board testing remains pending. The F1.3.2
configuration-corruption reproduction also still needs a board retest with
the stack fix.

The GitHub prerelease provides only `FIRMWARE.BIN` as an uploaded asset.
