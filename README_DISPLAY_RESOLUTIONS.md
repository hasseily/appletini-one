# Output resolutions

Choose **Video > Output resolution**. Changes apply without restarting and
save in the normal settings and profiles as `video.resolution=1920x1080`
(replace the value with one of the sizes below). Missing or invalid values
use 1920x1080. The original list keeps the largest requested size for each
aspect ratio; 1360x768 is also available as a lower-resolution widescreen option.
The selector cycles in the table's order, with 1360x768 after 1280x1024 and
before 1680x1050. This UI order keeps the hardware mode IDs unchanged.

**Output resolution** and **Size multiplier** appear at the top of the Video
tab, above **Video output**. Size multiplier sits beside Output resolution
(on the next row in compact menus). **Max** is the default and uses the
largest integer scale that fits.
Choose **1x** to reduce the picture, or **2x** where the output supports it.
The control scales the Apple picture, its borders, and its overlays, and
keeps them centered. Settings and profiles save it as
`video.size_multiplier=max`, `1`, or `2`; missing or invalid values use Max.

The table shows the default Max sizes. Each video mode limits the chosen
multiplier to what fits: at 1200x800, 2x gives legacy video 2x and SHR 1x.
The saved choice remains available when changing resolutions or video modes.

| Output | Aspect ratio | Legacy Apple picture | SHR picture |
| --- | --- | --- | --- |
| 1024x768 | 4:3 | 560x384 (1x) | 640x400 (1x) |
| 1200x800 | 3:2 | 1120x768 (2x) | 640x400 (1x) |
| 1280x1024 | 5:4 | 1120x768 (2x) | 1280x800 (2x) |
| 1360x768 | 85:48 | 1120x768 (2x) | 640x400 (1x) |
| 1680x1050 | 16:10 | 1120x768 (2x) | 1280x800 (2x) |
| 1920x1080 | 16:9 | 1120x768 (2x) | 1280x800 (2x) |

With Max, the compositor finds the largest integer scale that fits the active
picture, then centers it. It preserves the legacy 560x192 source's doubled line height.
Borders use the same scale and clip at the screen edges. They do not reduce
the picture's scale. At 1200x800, the legacy border clips; SHR stays at 1x
because its 2x width would be 1280 pixels.

Each output slot restores its full background when changing the multiplier
or switching between legacy video and SHR. This also handles the smaller SHR
picture at 1200x800. The restore uses the claimed frame's mode, so a new
renderer frame cannot change the scale between clearing the background and
drawing the picture.

The three output slots keep their 4 MiB spacing and RGB565 format. All six
surfaces fit. Each mode uses a packed stride of width times two bytes.

## Menus and bezels

Smaller outputs use a compact menu with readable 2x text, wrapped help, and
scrolling that keeps the selected row in view. A row at the top shows all 15
tabs with short labels and highlights the selected tab; its full name appears
in the heading. The larger outputs keep the
sidebar layout. Tab/Delete changes tabs; normal navigation and actions still
apply. File browsers and text readers use the available screen space.

Supply a bezel PNG for the selected resolution. Appletini draws it at its
native size. A bezel with another width or too much height leaves a plain
background. Existing partial-height banners still work when their width
matches; Appletini fills the space below them. The built-in banner is 1920
pixels wide and therefore appears only at the default resolution.

Output screenshots use the current output width and height. Apple-only
screenshots keep their existing source capture format.

## Clock and frame changes

The PL drains outstanding framebuffer reads before changing clocks. The PS
prepares a blank unused slot and stages its address with the requested mode.
The PL changes the pixel clock through the Clocking Wizard, waits for lock,
and commits the new dimensions and first-frame address together. A failed
clock change resets the private clock-control link, then programs the 1080p
clock again. A second failure leaves scanout held. Firmware keeps composition
paused if scanout remains held or unlocked.

| Mode | Pixel clock | 60 Hz horizontal total | 60 Hz vertical total |
| --- | --- | --- | --- |
| 1024x768 | 65 MHz | 1344 | 806 |
| 1200x800 | 67.5 MHz | 1360 | 828 |
| 1280x1024 | 108 MHz | 1688 | 1066 |
| 1360x768 | 85.5 MHz | 1792 | 795 |
| 1680x1050 | 119 MHz | 1840 | 1080 |
| 1920x1080 | 148.5 MHz | 2200 | 1125 |

The lower modes also have 50 Hz blanking presets. The default 1080p mode keeps
its existing Apple 50/60 Hz timing policy. The display must accept the selected
timing; 1200x800 and the lower modes' 50 Hz timings need particular care.
This implementation does not select modes from EDID.

1360x768 uses the 60 Hz DMT timing (positive H/V sync), as listed in the
[Linux DRM DMT table](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/drm_edid.c).
Its 50 Hz preset keeps the same pixel clock and uses 954 total lines.
The new mode uses ID 5; IDs 0 through 4 and the 1080p default stay unchanged.
At this height legacy video fits at 2x, with the top and bottom borders
clipped, while SHR uses 1x because a doubled SHR picture needs 800 lines.

The frontend needs the updated PL for mode switching. With an older PL, it
retains 1080p and reports that other resolutions could not be applied.

## Validation

Native regressions cover all six scales, clipped borders, scanlines, effects,
overlay coordinates, framebuffer bounds, menu navigation, settings, and the
mode-switch driver's success and failure paths. RTL simulations cover all six
timings at 50/60 Hz, clock-switch sequencing and recovery, and framebuffer
drain/restart, including the partial final burst at 1680x1050. The tests also
check that the PS and PL mode tables agree.

The generated AMD Clocking Wizard and MMCM simulation passes all six clock
rates and a late clock-change failure followed by recovery to 148.5 MHz.
The complete Vitis build passes for both frontend cores, the bootloader,
FSBL, and BSPs.

Run the focused regressions with:

```text
python scripts/test_display_scaling.py
python scripts/test_display_mode_transition.py
python scripts/test_resolution_config_menu.py
python scripts/test_display_output.py
python scripts/test_display_modes_rtl.py
python scripts/test_display_clock_wizard.py
```

The last test needs the generated project/IP files from
`vivado -mode batch -source scripts/create_project.tcl`.

The first board test found stale pixels when SHR reduced the scale, missing
tab navigation in compact menus, and missing IIgs borders in graphics modes.
The fixes below still need another board test.

The compositor now supplies the IIgs border from the claimed frame's border
color for interlaced and flip-merged legacy graphics, which do not contain
captured border pixels. Ordinary legacy video keeps its captured border,
including color changes within the frame. SHR keeps its own border geometry.

## F1.2.0 test firmware

`firmwares/F1.2.0-resolutions/FIRMWARE.BIN` contains the 2026-09-26 test build.
The menu and UART report **F1.2.0**. It includes the new FPGA design and a
full Vitis rebuild against that design's exported hardware platform.

The completed implementation has **+0.017 ns setup**, **+0.060 ns hold**, and
**+0.265 ns pulse-width** slack, with no failing timing endpoints or route
errors. The user requested this test image after reviewing those margins.
It is below the +0.200 ns release target and is not a promoted timing reference.

Packaging verified the firmware role, recovery flag and code, slot size,
payload CRC, version label, and embedded core-1 image. The image is 4,353,260
bytes. Copy it to the SD-card root as `FIRMWARE.BIN` for the normal update
process, then choose **Video > Output resolution**. Hardware testing remains
pending.

SHA-256: `2bdf7d334cc783d7e8221021f4e16d9371f30a27b6a37d09193d2270da9a2474`.
The delivery folder includes input binaries, source snapshots, build logs,
timing reports, and `firmware_manifest.txt`.

## F1.2.0 fixes test firmware

`firmwares/F1.2.0-resolutions-fixes/FIRMWARE.BIN` is the 2026-09-27 rebuild
with full background restoration on legacy/SHR transitions, all tabs visible
in compact menus, and IIgs borders for interlaced and flip-merged graphics.
It keeps version F1.2.0 and the same FPGA image and timing margins as above.

Native tests check both transition directions in all three output slots,
all five resolutions, bezel and border variants, and a newer published frame
arriving after the claim. Pixel tests cover captured and synthesized borders,
scanlines, effects, and framebuffer bounds. Menu tests check all 15 selected
tabs and text bounds; the 1024x768 render was also checked by eye. These fixes
still need a board test.

The full Vitis build and boot-image checks passed. Packaging verified the
firmware role, recovery code, embedded core-1 image, version, and payload CRC.
The image is 4,354,220 bytes; the root `FIRMWARE.BIN` is an identical copy.

SHA-256: `6d706fa4c9d7fe4500a5b2db28613b6ad6bcd999e42fbfb93d1890eba3767e97`.

## F1.2.0 size multiplier test firmware

`firmwares/F1.2.0-size-multiplier/FIRMWARE.BIN` adds the saved Size multiplier
control. It keeps version F1.2.0 and the same FPGA image. Changing the size
updates the compositor without changing the display clock or resolution.

Native tests cover Max, 1x, and 2x at all five resolutions, with borders,
scanlines, effects, and text overlays. Tests also cover size changes in all
three output buffers while the Apple video mode stays the same, including
old overlay cleanup. Menu renders at 1024x768 and 1680x1050 were checked by
eye. The new control still needs a board test.

The full Vitis build and firmware checks passed. The image is 4,355,820 bytes;
the root `FIRMWARE.BIN` is an identical copy. Previous test images remain in
their own folders.

SHA-256: `0b2d5f19c92366521869aa698a049a988329192991fb002aa43cf76f3736ad65`.

## Border capture phase fixes

The border investigation found separate bugs with nonzero capture phase
settings. Negative offsets could omit pixels at the top border and publish
before its last cycles arrived. Positive PAL offsets replayed two frame-edge
cycles at the wrong positions, so PAL rejected incomplete frames. Large
positive offsets also lost line-0 left-border colors before the next writer
slot opened. The renderer now keeps the adjusted coordinates and captured
`$C034` colors across those boundaries.

`python scripts/test_renderer_border_pipeline.py` passes 6,060 compositor
checks plus source capture and raster-color checks. The replay covers 262-
and 312-line capture, phase limits and frame-edge offsets, and PAL work
deferred across later frames. It also changes `$C034` on individual cycles
to check color placement and publication order. These fixes do not establish
the cause of the reported graphics-only black border; that hardware symptom
still needs reproduction.

The existing `shadow a2li` UART diagnostic now includes border enable,
configured default color, current `$C034` color, and clean/PAL phase offsets.
Phase values use signed 8-bit hexadecimal (`C0` means -64, `FF` means -1,
and `3F` means +63).

`firmwares/F1.2.0-border-fix/FIRMWARE.BIN` contains these fixes after a full
Vitis rebuild. It retains the size multiplier, version F1.2.0 and the
previous FPGA image. The firmware role, recovery support, embedded core-1
image, boot partitions and payload CRC all pass verification. The image is
4,372,908 bytes and matches the current root `FIRMWARE.BIN`; earlier images
remain in their named folders. No hardware test or flash operation ran.

SHA-256: `55ae5854001797a8e01bc92234f662446eb045bb74a9244e1f097a45f8147eaf`.

The final ARM Cortex-A9 NEON replay passes 150 frames. A separate existing
issue remains at clean/NTSC phase offsets of +24 or higher: the first active
row can stay black. It occurs in both the previous and current renderer and
does not explain the whole-border report.

## F1.2.0 timing test firmware

`firmwares/F1.2.0-timing/FIRMWARE.BIN` combines the border capture phase fixes,
output resolutions and saved size multiplier with the simplified FPGA build.
The fresh full build meets the requested global setup target: **+0.163 ns**
setup, +0.027 ns hold and +0.265 ns pulse width. All 32 Apple/Gray-pointer
bounds and route checks pass. Build time falls from 25m44s to 17m33s; see
`README_VIVADO_RUNTIME_AUDIT.md` for the measurements and retained constraints.

The full Vitis rebuild and firmware checks pass, including the exact XSA and
bitstream, source hashes, embedded CPU1 image, recovery support and payload
CRC. The image is **4,391,788 bytes** and matches the current root
`FIRMWARE.BIN`. Earlier test images remain in their named folders.

SHA-256: `ee9a4a5fa55a2144fd0fd69cc4882022f994f63b39918bd7f09fd4e9f5fc4c07`.

The user tested this image and reported that it works well. This confirms
the reported board test, not a full hardware test suite. The build has not
been promoted as a known-good timing reference.

## F1.2.0 Video menu update

The Output resolution and Size multiplier controls now appear first in the
Video tab, above Video output. Keyboard focus and help follow the same order
in the full and compact menus. The firmware version remains F1.2.0.

The native menu checks pass for all five resolutions, including focus,
scrolling, text bounds and framebuffer guards. The 1024x768 and 1680x1050
previews were also checked by eye. This UI change uses the same FPGA image
as the timing test firmware above.

`firmwares/F1.2.0-video-menu/FIRMWARE.BIN` contains the full Vitis rebuild.
The firmware version, recovery support, embedded CPU1 image, source hashes,
XSA/bitstream and payload CRC all pass verification. The image is 4,391,788
bytes and matches the root `FIRMWARE.BIN`. The earlier images remain in
their named folders. This menu update has not yet been tested on the board.

SHA-256: `ccbaebdac64f9c17bc70f47ee81e055e4ed22dcb5a9b55beb2ddaaa27bd98fcb`.

## F1.2.0 with 1360x768

`firmwares/F1.2.0-1360x768/FIRMWARE.BIN` adds 1360x768 as the sixth output.
It keeps the existing mode IDs, 1920x1080 default and firmware version F1.2.0.
The new mode uses an 85.5 MHz pixel clock. Max gives legacy video 2x and SHR
1x; legacy borders clip at the top and bottom at this resolution.

Fresh synthesis and incremental implementation from the prior passing
checkpoint give **+0.166 ns setup**, +0.027 ns hold and +0.265 ns pulse width.
All 32 Apple/Gray bounds, bus-skew and route checks pass. See
`README_VIVADO_RUNTIME_AUDIT.md` for the reuse and runtime measurements.

Native scaling, mode-transition, output, framebuffer and menu checks pass for
all six sizes. RTL tests pass for 50/60 Hz timing, clock control and recovery,
and framebuffer drain/restart behavior. The generated Clocking Wizard model
passes all six pixel clocks. The 1360x768 menu preview was checked by eye.

The full Vitis rebuild and firmware checks pass, including source hashes,
XSA/bitstream, both CPU images, version, recovery support, four boot partitions
and payload CRC. The image is **4,391,980 bytes** and matches the root
`FIRMWARE.BIN`. Earlier firmware images remain in their named folders. This
image still needs a board test; no flash operation ran.

SHA-256: `045c26df0e73bcce6ab9250eb822cb5cba45c83e9cc6da0b0cc725d7445cdf14`.

The follow-up `firmwares/F1.2.0-1360-ui-order/FIRMWARE.BIN` places 1360x768
after 1280x1024 and before 1680x1050 in the selector. The menu test passes
forward and reverse cycling, including wraparound. This Vitis rebuild keeps
F1.2.0 and the exact FPGA image above, so its timing stays unchanged.
Firmware verification passes; the 4,392,108-byte image matches the root
`FIRMWARE.BIN` and still needs a board test.

SHA-256: `514aa7ba014cf09b2fb966047c977b08b64ca1a2d3e3f3c97c5393d4f6bf3f24`.

On 2026-09-28, the user confirmed that the display resolution and timing
changes work on their board and requested that they be committed to `main`.
This records that board report; it does not change the archived build
manifests or promote a timing reference.
