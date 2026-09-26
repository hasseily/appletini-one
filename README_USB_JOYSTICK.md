# USB joysticks and paddles

USB1 HID joysticks can supply Apple paddle positions and three pushbuttons
while vTW runs the physical Apple II. Connecting a supported controller
selects USB paddle input automatically. Disconnecting it restores the
physical game connector and the existing II/II+ button policy. Native Apple
CPU operation does not use the USB joystick bridge.

ONE//e keeps its existing input path. Both virtual CPU modes use the same
saved mapping settings. Physical Open/Solid Apple keys still work on a //e:
vTW combines their button reads with USB button presses. On a II/II+ with
forced-zero buttons enabled, USB supplies the button state directly.

## Set up a controller

Open **USB > Joystick / Paddles** in the config menu. Select a paddle from
PDL0 through PDL3, then adjust:

- **Source:** Auto, X, Y, Z, Rx, Ry, Rz, or Off.
- **Invert:** reverse that paddle's direction.
- **Sensitivity:** 25% through 200%, with 100% as the default.
- **Deadzone:** 0% through 50% of the travel from center to an endpoint.

Auto maps PDL0/1 to X/Y and PDL2/3 to Rx/Ry, falling back to Z/Rz when those
rotation axes are absent. Missing axes and Off return the center value 128.
Defaults use Auto, no inversion, 100% sensitivity, and no deadzone, which
preserves the earlier ONE//e mapping. Use zero deadzone for a paddle that
needs continuous movement through its center.

The page shows raw axes, mapped paddle values, buttons, connection state,
and whether vTW input is active. Axis and hat movement does not navigate
the menu while this page is open; keyboard, mouse, and buttons remain
available. **Restore defaults** resets all four mappings. Settings use the
normal config save and profile flows, with keys under
`usb.joystick.paddle.0` through `usb.joystick.paddle.3`.

While the config menu captures input, a connected joystick supplies centered
paddles and released buttons to the Apple. The preview still tracks movement.
Closing the menu restores the current position without requiring a new report.

The lowest connected HID slot owns all four paddles and PB0 through PB2.
If it disconnects, the next controller takes over. Axis-only and button-only
reports retain the values omitted from that report. Current HID detection
requires at least two absolute axes; relative mice and a lone throttle are
not selected as joysticks. This does not add support for non-HID controller
protocols.

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
python scripts/test_joystick_config_menu.py
```

Physical controller and game tests remain pending for this implementation.
Record the exact firmware image and controller models when those tests run.

### F1.1.5 test image

`firmwares/F1.1.5/FIRMWARE.BIN` was built from clean source `d5b43743` on
2026-09-26. The HDL simulations, native input/UI checks, full Vitis build,
and firmware integrity checks passed. The final USB paddle simulation
checked 5,381,600 timer values and real CPU polling at all four speed modes.

The image has **+0.076 ns setup**, **+0.065 ns hold**, and **+0.265 ns pulse
width** slack under nominal constraints, with no failing timing endpoints
or routing errors. Further timing attempts did not improve this result.
This test image falls below the **+0.200 ns setup target**; it is not a
promoted timing reference and has not been tested on a board.

Firmware SHA-256:
`e1ff775d834cc6176ef2119b75a7b9c332e54eef59b03bcdb3a3d2320d7678d7`.
The delivery folder contains `firmware_manifest.txt`. Reports, component
hashes, and build logs are archived under
`.timing_runs/20260926T134422Z-d5b43743-incremental/positive_trial`.

For board testing:

- Record the firmware hash, Apple model, and controller model. Check raw
  axes, all four mapped paddles, and PB0 through PB2 in the preview.
- Try source selection, Off, inversion, sensitivity, and deadzone. Save and
  reload a profile. Confirm stick movement leaves the menu focus unchanged.
- Test a paddle diagnostic or game at each vTW speed, including TURBO with
  **Slow Paddles/joystick** disabled.
- Hold an axis or button while opening and closing the menu and switching
  vTW off and on. Check that held input returns without a new movement.
- Check controller handoff and physical game connector fallback after
  disconnect. On a //e, check physical Apple keys alongside USB buttons.
  Also check the saved mapping in ONE//e and physical inputs with the
  native Apple CPU.
