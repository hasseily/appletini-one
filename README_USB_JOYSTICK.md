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

### F1.2.1 clone activation fix

The board test of the Hello startup image still received only two Hello
packets. Its log confirms `hello=1`, all four startup commands completed,
and no Xbox transfer errors. Waiting for Hello fixed the startup order but
did not make this controller send input.

[xpad issue 161](https://github.com/paroj/xpad/issues/161) records the same
failure on a ZEROPLUS clone with USB ID `045e:02ea`, endpoints `82/02`, and
the same first three Hello identity bytes (`7e ed 81`). Owners report that
an Identify request followed by a fixed serial-response acknowledgement
enables input. The [working driver variant](https://github.com/sirkhancision/xpad/blob/master/xpad.c)
sends these bytes, including sequence 1 in both packets:

```text
04 20 01 00
01 20 01 09 00 1e 20 10 00 00 00 00 00
```

The driver now tries this pair once if the normal four-command startup has
finished and no input report arrives within one second. It sends both
commands through the existing asynchronous OUT path. Normal input suppresses
any remaining unsent fallback command. Guide acknowledgements, stable output
buffers, bounded retries, and disconnect cleanup still apply. This is a
workaround for these clones, not a full metadata or authentication driver.

**Board result, 2026-09-28: confirmed working.** The user reports that the
EG-C50700X now works through the same USB hub with the clone activation image.
This confirms the fix for the reported controller startup failure on this
setup.

The confirmed image is `firmwares/F1.2.1-xbox-clone/FIRMWARE.BIN`, also copied
to the repository root. It keeps F1.2.1 and the exact existing FPGA image.
All 18 Xbox runtime groups, shared-input/HID retry checks, PS4 checks, the
full Vitis build, and firmware payload checks pass. SHA-256:
`86d2476992232873e7504fcbc0452c1061e5dfab2a58e8191a0d87d7fab5c46f`.

After updating, reconnect the controller through the same hub, wait three
seconds, then move a stick and press a button. Capture the connection log
and `usb1 status`; look for `clone fallback starting`, `clone fallback sent`,
`clone=2/2`, and `Xbox One input active`. Clone progress counts completed
fallback commands; it stays partial if input makes the remaining work
unnecessary. The ordinary startup count remains `init=4/4`.

### F1.2.1 Hello startup fix

The diagnostic image received two identical 32-byte packets beginning
`02 20 01 1c`, with zero USB errors and no input reports. This is the
controller's Hello message. The driver ignored it and sent its Start command
before Hello, using sequence zero.

The driver now arms IN and waits for a valid primary-device Hello. It then
sends `05 20 01 01 00` (Start, sequence 1) and the existing three compatibility
commands. Host command sequences skip zero. Duplicate Hellos do not restart
startup or change an active output buffer. The firmware keeps the prior
transfer recovery fixes and packet diagnostics; `usb1 status` adds `hello=1`
when the handshake begins.

Microsoft permits Start as the Hello response for non-audio devices and
reserves sequence zero. See [Hello enumeration](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/09351525-aa34-4a00-ac36-510fcf2fb106)
and [Set Device State](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gipusb/8eaad00e-97e6-4ae0-86fa-471131649d70).
The board test confirmed the new startup order but still received no input;
see the clone activation test above.

The earlier test image is `firmwares/F1.2.1-xbox-hello/FIRMWARE.BIN`.
It keeps F1.2.1 and the exact existing FPGA image.
All 13 Xbox runtime test groups pass, including delayed/malformed Hello,
duplicate Hello during active output, and the reported `0x82/0x02` endpoints.
The Vitis build and firmware payload checks pass. SHA-256:
`2b8409e572e0dd099b97d1618d7fa19cb9218d65ebe3fcc038247b2d4d403fa2`.

### F1.2.1 transfer recovery fix

The next board test confirmed that the recovery image no longer freezes,
but the same controller still does not supply input. Its log ends after
interface 0 connects, with no transfer errors. That does not yet distinguish
a silent controller from packets the parser rejects.

The follow-up `usb1 status` shows Xbox startup at `4/4`, no Xbox transfer
errors, and its IN request pending. Two keyboard interfaces on device 4 have
more than 233,000 combined I/O errors (`-12`). Those counts show a wider USB
problem, but do not prove its cause. The status command now includes the
cached EHCI error token and remaining byte count to distinguish retry
exhaustion from missed split transactions on the next test.
With the keyboard removed and the controller reconnected into slot 0,
startup still reaches `4/4` with no Xbox errors or input reports. The old
HID error totals stop growing. Keyboard traffic is therefore not required
to reproduce the Xbox input failure; receive-packet diagnostics are still
needed to distinguish silence from ignored packets.
Failed HID completions now use the existing 10 ms retry timer. Successful
input still rearms at once, and a device can recover without being unplugged.
This limits the retry rate; it does not establish the cause of the I/O errors.

The follow-up diagnostic image prints endpoint sizes and intervals, the
first received packet (up to 18 bytes), and `startup sent` after all four
output commands complete. If no joystick report arrives within two seconds,
it prints one `waiting for input` snapshot. `usb1 status` also shows packet
and empty-transfer counts plus the last packet prefix. These messages do not
change startup, parsing, retries, or USB scheduling. They avoid repeated log
output while a controller remains idle.

The diagnostic test used `firmwares/F1.2.1-usb-input-recovery/FIRMWARE.BIN`.
The image remains F1.2.1 and reuses the same FPGA bitstream. Xbox runtime
checks, native HID retry checks, HID source checks, the Vitis rebuild and
firmware payload verification pass. Reconnect the controller, wait at least
two seconds, move a stick, then capture the connection log and `usb1 status`.
SHA-256: `67ec1f755824e1beab85e3c3c379e7801b08b85528ed69e8757423a94e590e15`.

The reported `045e:02ea` log reaches `Xbox One connected`, but never
`Xbox One input active`, then repeats error `-8` and control-transfer retries.
The unsupported audio/accessory interfaces are expected; the gamepad driver
has already claimed interface 0. The log does not establish why the first
USB transaction failed or whether Home/Turbo caused the failure.

The EHCI completion scanner previously treated sticky transaction-error bits
as a completed failure even while hardware was still retrying. It also called
all halted descriptors STALL (`-8`), including exhausted transport retries.
The scanner now waits for hardware completion and distinguishes a real STALL
from transport errors. This follows the EHCI completion rules and
[Linux's EHCI status handling](https://github.com/torvalds/linux/blob/master/drivers/usb/host/ehci-q.c).

Xbox endpoint recovery now makes one control request with a 100 ms timeout
per attempt. After eight consecutive failures on either endpoint, it stops
that controller's transfers and releases its buttons, axes and held Guide
state. Unplugging and reconnecting starts a fresh attempt. Other USB inputs
remain registered. Successful IN traffic cannot hide persistent OUT failures.
Error messages include the endpoint and startup step; `usb1 status` shows
`stopped=1` after recovery is exhausted. The startup packets remain unchanged.

The actual EHCI scanner passes 23 completion cases; the original code fails
the active-retry case. Xbox runtime checks cover permanent STALL/timeout/I/O
failures, bounded recovery, separate endpoint budgets, late callbacks and
reconnection. HID, PS4, shared input, ONE//e and vTW checks also pass. These
checks establish the software defects and their fixes; the controller still
needs a board test through the reported hub.

The earlier test image is `firmwares/F1.2.1-xbox-recovery/FIRMWARE.BIN`.
It keeps version F1.2.1 and the existing FPGA image
(setup slack +0.166 ns). The full Vitis rebuild and firmware payload checks
pass. SHA-256: `7e74f82f3f22a57229f758813e53e7d15e0352f9c0af72a427200d3bafe9ba01`.

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

The reported hardware log showed successful enumeration of `045e:02ea`,
followed by unsupported-interface errors on all three interfaces. That places
the failure at driver selection. The earlier `0bda:8153` log identifies a
Realtek USB Ethernet device in the hub path, not this controller.

After rebuilding the frontend, connection should print:

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

The Xbox and PS4 additions passed native report, transfer-lifecycle, slot,
menu, and input-release tests. The changed C sources also compiled for the
Zynq ARM target with `-Wall -Wextra -Werror`. The existing HID, ONE//e input,
vTW paddle, and joystick menu checks passed. Multi-controller tests cover all
48 axis sources, four different controllers at once, saved device settings,
partial reports, menu capture, and disconnect/reconnect. Both Apple input
paths pass. The new drivers and mapping still need an on-card input check.

## Firmware F1.2.1

F1.2.1 adds the Xbox One and PS4 USB inputs and per-paddle device selection.
It builds on the display resolution and timing changes on `main`. The
controller changes need a full Vitis rebuild; they reuse the same FPGA image
and exported hardware platform. The boot image version stays B1.2.0.

The delivery path is `firmwares/F1.2.1-multi-joystick/FIRMWARE.BIN`.
Its `firmware_manifest.txt` records the software commit, reused FPGA build,
component hashes, and image checks. Copy the image to the SD-card root as
`FIRMWARE.BIN` for the normal update process. After updating, use the raw and
mapped previews to check each controller, then test a game with TransWarp or
standalone ONE//e. A physical controller test remains pending.

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

The first board test of F1.1.5 failed during startup; see the result below.
Controller mapping and game tests remain pending.

The revised menu help and status passed the native joystick menu checks,
38 profile, USB binding, and standalone ONE//e checks, and ARM syntax checks.
Rendered previews show active, inactive, waiting, disconnected, and standalone
ONE//e states, all seven setting help blocks, and the USB tab help without
clipping or overlap. These checks preceded the firmware confirmation below.

### Rebuilt firmware with the revised help

`firmwares/F1.1.5-joystick-help/FIRMWARE.BIN` contains a full Vitis rebuild
from 2026-09-26 with the approved help and availability changes. It uses the
exact regenerated FPGA bitstream confirmed working below. FSBL and core-1
binaries match the previous working build; only the frontend binary changed.
The menu still reports F1.1.5. Timing remains +0.076 ns setup and +0.065 ns
hold. On 2026-09-26, the user confirmed that this rebuilt firmware is good.

The folder includes build logs, input binaries, source hashes, the source
patch, and the firmware manifest. Packaging verified the firmware role,
recovery flag, size limit, and payload CRC. SHA-256:
`089fca6841462005e0e6a9a0ca3909939d78eee3a5119168c92fb9f3170e4909`.

### F1.1.5 test image

`firmwares/F1.1.5/FIRMWARE.BIN` was built from clean source `d5b43743` on
2026-09-26. The HDL simulations, native input/UI checks, full Vitis build,
and firmware integrity checks passed. The final USB paddle simulation
checked 5,381,600 timer values and real CPU polling at all four speed modes.

The image has **+0.076 ns setup**, **+0.065 ns hold**, and **+0.265 ns pulse
width** slack under nominal constraints, with no failing timing endpoints
or routing errors. Further timing attempts did not improve this result.
This test image falls below the **+0.200 ns setup target**; it is not a
promoted timing reference.

Firmware SHA-256:
`e1ff775d834cc6176ef2119b75a7b9c332e54eef59b03bcdb3a3d2320d7678d7`.
The delivery folder contains `firmware_manifest.txt`. Reports, component
hashes, and build logs are archived under
`.timing_runs/20260926T134422Z-d5b43743-incremental/positive_trial`.

**Board result, 2026-09-26: failed.** This image produces a blank screen
with USB1 empty as well as with a hub, mouse, and joystick. The log reports
an invalid machine mode and corrupt reads from several FPGA register
blocks: the slot mask reads `0x00802000`, the boot status/timeout/handoff
all read `0x01200000`, and framebuffer base readback is `0x00004000` after
writing `0x3E000000`. The framebuffer check runs before joystick polling.
Do not use this image for controller validation until startup is fixed.
The firmware payload contains the intended bitstream; its FSBL and PS
clock initialization match the prior working F1.1.4 image.

A diagnostic image with the exact F1.1.5 ARM software and the previously
tested F1.1.4 FPGA image restored the display and Apple boot on the same
board. This isolates the failure to the new FPGA image. That working
fallback is `firmwares/F1.1.5-diagnostic-old-pl/FIRMWARE.BIN`, SHA-256
`ed17caac29a0cb8b4f3e618d997146032cab6c488edb83d182429325975203fb`.
It lacks physical vTW USB joystick hardware. The regenerated FPGA image
below fixes the reported startup failure.

The routed design passes 53 full-design MMIO checks, including the
joystick signature, framebuffer round-trip, every framebuffer data bit,
and the PS7 interface connections. Its read FIFO also passes 10,272
comparisons. These checks validate the logical design, not the programmed
configuration or board timing.

Regenerating the bitstream from the saved implementation changes actual
frame data compared with the shipped bitstream. The test image
`firmwares/F1.1.5-diagnostic-regenerated-pl/FIRMWARE.BIN` contains that
regenerated bitstream and the exact same ARM software. Its SHA-256 is
`92481bf4d12a6133ba27a3e21e05f9c26efd1e1caf4b609080be36f847e79025`.
It includes the new joystick hardware and retains the measured +0.076 ns
setup result. **Board result, 2026-09-26: the user confirms that the
regenerated image works and fixes the hardware failure.** Controller
mapping and game tests have not yet been reported.
Two fresh Vivado sessions produce byte-identical regenerated configuration
data. The fix required no HDL, synthesis, placement, routing, or ARM
software change: a fresh Vivado session reopened `candidate.dcp` and ran
`write_bitstream` again. The differing frame data and successful board
test point to a bitstream-generation state issue; the exact internal
Vivado cause has not been established. The investigation logs, bitstreams,
and test harness are archived in
`.timing_runs/F1.1.5-blank-boot-investigation-20260926`.

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
