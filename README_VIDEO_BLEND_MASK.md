# H/V blending and pixel masks

**Historical F1.2.3 record.** F1.2.4 restores the F1.2.2 Phosphor blur and mono Dot bleed controls. The H/V brightness-based filter described below is removed. Pixel masks remain available. See [current filter notes](README_VIDEO_MONO.md).

This F1.2.3 candidate gives **H blending** and **V blending** the same
brightness protection and adds a separate **Pixel mask** control. Firmware
build and hardware acceptance results must be recorded with the image hash;
this note does not claim board frame rates.

## Blending

H mixes neighboring columns; V mixes neighboring rows. Both offer Off,
Light, Medium and Strong. Before brightness protection, each neighbor has
weight 6.25%, 12.5% or 25%, respectively. The remaining weight belongs to the
current pixel. Enlargement repeats the filtered native pixels; it does not
add another horizontal interpolation step.

Pixels with similar brightness use the full selected weight. As brightness
differs, the filter reduces each neighbor's contribution. At a large
difference, a darker neighbor stops dimming the current pixel; a brighter
neighbor retains one eighth of its normal contribution. This permits mild
light spill into dark pixels. It replaces the preceding test image's exact
black exclusion. Uniform colors stay unchanged.

The shared rule uses the full weight through a brightness difference of
16, then tapers until a difference of 80. These are image-processing choices,
not a measured calibration for a particular CRT. Color mixing, near-black
edges and text need visual comparison on the board.

The controls work in color and monochrome, legacy, woven and SHR output.
Their native footprint does not grow when output resolution changes.
Ghosting feeds H/V; spatial processing does not feed back into ghost history.
Glow uses a separate halo and follows H/V. TV decoder modes do not add
automatic V blending. PAL decoder behavior remains separate.

Both axes choose weights from the same brightness guide, taken after
ghosting and before either spatial blend. H runs before V in light space,
with one conversion back to RGB after both. The light estimate uses gamma 2
(`ceil(channel squared / 2)`), rather than the earlier gamma-2.2 correction.
The integer square root rounds to the nearest channel value; this keeps
uniform colors exact. Glow adds its separate halo after that conversion.
Sampled comparisons with gamma 2.2 differ by up to 9 channel levels out of
255. The native and ARM paths agree on the chosen integer model; this is a
deliberate color/performance trade-off, not a claim of exact CRT response.

H alone reuses a proven constant-color prefix from the cached source row. It
keeps the boundary pixel in the filter and aligns partial prefixes to groups
of eight pixels. This produces the same pixels while reducing work on flat
rows and rows with a long blank start. It adds no reads of the original source
frame beyond the existing cache load.

## Pixel mask

| Choice | Output-pixel pattern |
| --- | --- |
| Off | No mask |
| Aperture grille | RGB columns repeat; each column retains its selected channel and dims the other two |
| Shadow mask | The RGB columns shift one position on alternate rows |
| LCD grid | Every third row or column is dimmed; intersections are not dimmed twice |

The mask uses the Multitini pattern: a dimmed 8-bit channel becomes
`value - (value >> 2)`, then converts back to RGB565. Strength is fixed.
Masks follow blending and scanlines, so they do not become filter neighbors
or enter ghost history. They cover the Apple picture and enabled Apple
border. Decorative bezels, menus and status overlays stay unchanged.

Patterns use output coordinates relative to the visible, clipped Apple viewport, including
when a screenshot crops that viewport. Their pitch remains three output
pixels at 1x and enlarged sizes across all six resolutions. Changing output
resolution can change their apparent physical size on a monitor; source
enlargement does not enlarge the mask cells.

The FPGA applies the mask during scanout. It adds no ARM pixel pass or DDR
framebuffer traffic. Software attaches the mask, viewport and UI exclusions
to each completed output slot; the FPGA takes them with the frame it latches.
The bezel path and the native-image scope of the expensive effects stay intact.

## Screenshots

A2 and full-output captures copy the completed compositor image. Saving the
PNG reproduces the FPGA's RGB565 mask rounding with the captured settings,
visible viewport and UI exclusions. A crop retains its original output
coordinates, so it matches the corresponding part of a full capture.

PNG saving runs in batches of two rows. Its copied pixels and metadata stay
fixed if profiles, mask, size or resolution change while it saves. The capture
uses the completed slot's dimensions and stride. During a resolution switch,
capture waits for a completed frame by returning `NO OUTPUT FRAME`; it does
not interpret an old slot with the new stride. Snapshot copying and SD I/O
still have their existing costs.

## Menu and saved settings

The Video tab has twenty controls. H/V share a row, Scanlines/Pixel mask
share the next row, and Glow/Ghosting share the next, with Glow on the left.
The compact menu follows the same reading and focus order. Pixel mask is
available in monochrome as well as color.

Global settings and profiles save:

```ini
video.blending.horizontal=OFF
video.blending.vertical=OFF
video.pixel_mask=OFF
```

H and V accept `OFF`, `LIGHT`, `MEDIUM`, `STRONG`. Pixel mask accepts `OFF`,
`APERTURE`, `SHADOW`, `LCD`. Mask defaults to Off, including when loading an
older profile without the key. Invalid mask text also selects Off.

An explicit `video.blending.horizontal` wins over old
`video.smoothing.horizontal`, Blur and Dot bleed keys in either file order,
including when the new value is Off. Without it, the old horizontal key has
priority over the earlier Blur/Dot migration. The stored strength survives
migration, but the revised blend can look different. V retains its existing
new-key/legacy-CRT precedence. Configuration version remains 123.

## Validation

The native menu harness covers 302,400 existing V migration cases, 57,600 H
key-order/load/save/reload cases and 288 independent mask/global/profile
cases. It checks defaults, invalid mask values, all four mask choices,
runtime callback application and all six resolution layouts. The 1024x768
and 1920x1080 previews have been visually checked for clipping and overlap.

Run `python scripts/test_resolution_config_menu.py`,
`python scripts/test_video_output_config_menu.py`,
`python scripts/test_video_smoothing_config.py`,
`python scripts/test_config_profiles.py` and
`python scripts/test_vidhd_shr.py` for the menu and persistence checks.
Pixel arithmetic, RTL, screenshots and timing need their separate tests.

Native and ARM blend tests each pass 196,608 brightness pairs, 65,027 integer
roots, 39,849 raw samples and 818 integration cases. A further 2,400 prefix
cases per backend cover 494,400 pixels, including alpha changes, every short-row
transition position, vector/tail boundaries, both output forms and guard words.
Their output digests match. Both source-read traces read 551,936 bytes exactly
once, including a long-prefix H-only case with border, glow and ghosting.

`scripts/test_video_pixel_mask.py` passes 5,898,240 mask values and 72 actual
layout combinations, including off-screen borders, mask phase, UI clipping,
bank ownership and mode-transition rejection. `scripts/test_screenshot_service.py`
passes 78 state-machine cases and independently decodes full/cropped masked
PNGs, including immutable mask settings and frozen output stride.

The [hardware checklist](README_VIDEO_BLEND_MASK_TESTS.md) gives exact patterns,
settings, commands and the preceding DIX measurements for comparison.

The matched ARM instruction profile at native 560x192 enlarged 2x4 uses the
preceding black-protection image as its baseline. It includes the real
archived row-copy routine. Off is unchanged. H+V uses 15–19% fewer instructions;
V Light uses 1.5% fewer, Medium is nearly unchanged, and Strong uses 10.3% more.
H alone depends on the image: the 49 paired cases range from 67.4% fewer
instructions on uniform rows to 53.4% more on busy rows. Synthetic text at
H Strong uses 48.9% more. Its new light-space math costs more than the old
encoded-RGB H filter; the exact prefix shortcut recovers work on flat areas.
Counts do not establish cycles or FPS: **unchanged H-only performance has not
been established**, and requires the DIX comparison.

On hardware, compare adjacent colors of similar brightness, white/color
lines against black, near-black ramps, moving text and combined H/V effects.
Check mask phase after mode and size changes, screenshot/display agreement,
Apple border coverage and clean UI/bezel exclusions. Record frame times and
underrun deltas with masks Off and each mask enabled; instruction counts alone
do not establish frame rate.
