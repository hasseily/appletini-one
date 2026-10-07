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

File browsers size the list from the current screen height, including the
title and footer. The old 17-row limit left unused space in compact menus.
For example, 1024x768 now fits 24 entries and 1280x1024 fits 34. The last page
includes earlier entries where needed to fill the list, including after a
resolution change. The item counter shows the visible range and total so
users can see when more entries lie above or below the list. Short directories
show only their actual entries.

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
python scripts/test_disk_browser.py
python scripts/test_disk_browser_layout.py
python scripts/test_display_output.py
python scripts/test_display_modes_rtl.py
python scripts/test_display_clock_wizard.py
```

The last test needs the generated project/IP files from
`vivado -mode batch -source scripts/create_project.tcl`.
