# Firmware F1.3.3b3

F1.3.3b3 replaces the blank standalone screen with the Appletini logo and
instructions to enable ONE//e in Boot Settings. A stopped card shows this
screen once its safety guard reports a valid, quiet state.

Connect a USB keyboard to USB1. Pause/Break or either Windows/GUI key opens
Boot Settings with the ONE//e Standalone Mode switch focused. Opening the
menu leaves the mode OFF; select the switch to start it.

| Key | Action |
| --- | --- |
| Pause/Break | Open or close the menu |
| Left or right Windows/GUI | Open the menu |
| Arrow keys | Navigate while the menu is open |
| Enter or keypad Enter | Select while the menu is open |
| Tab / Shift+Tab | Next / previous tab while the menu is open |
| Esc | Go back while the menu is open |

These fixed menu controls now also work while ONE//e is OFF on the standby
screen. A held key cannot trigger another action when the routing changes.
The open menu takes display priority. The screen fits all six output sizes
and reuses the menu logo cache and the existing RGB565 framebuffers.

Activation remains manual when the saved setting is OFF. The existing saved
ON restore and safety-stop rules stay in place. Standby does not request
ONE//e, clear a stop latch or grant bus access. Apple-bus activity, a missing
guard, a pending restore, vTW startup and firmware switching suppress it.
A quiet connector does not prove that the card is physically unplugged.

This ARM-only update retains the F1.3.3b1 stack fix: a 64 KiB frontend stack,
a 64 MiB heap, and linked-ELF checks before packaging. It keeps the exact
F1.3.2 FPGA bitstream, XSA, FSBL and CPU1 image used by F1.3.3b2. Golden
B2.0.6 stays unchanged. No FPGA rebuild is needed.

Native checks cover all six screen sizes, logo-decode fallback, framebuffer
bounds, static-slot caching, screenshot restoration, manual OFF behavior,
safety stops, menu focus, fixed keys and held-key transitions. Physical
display and USB-keyboard testing remains pending. The F1.3.2 configuration
corruption reproduction also still needs a board retest with the stack fix.

The broader `test_fb16.py` source checks have two known failures that also
occur on the unchanged F1.3.3b2 commit. The standby renderer tests pass using
the real drawing code.

The GitHub prerelease provides only `FIRMWARE.BIN` as an uploaded asset.
