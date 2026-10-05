# F1.2.3 blend and mask hardware checks

**Historical F1.2.3 record.** F1.2.4 restores the F1.2.2 Phosphor blur and mono Dot bleed controls. The H/V brightness-based filter described below is removed. Pixel masks remain available. See [current filter notes](README_VIDEO_MONO.md).

Use the candidate image identified in `README_FIRMWARE_F1_2_3.md`. These checks
need a board; host and FPGA simulations do not establish frame rate or monitor
appearance. Keep the preceding `F1.2.3-v-black-protection` image for comparison.

## 1. Similar-brightness colors

Start with Idealized color, H/Glow/Ghosting/Scanlines Off and Pixel mask Off.
Run this Applesoft HGR target:

```basic
10 HGR
15 POKE 49234,0
20 FOR Y=16 TO 175
30 HCOLOR=1+4*(Y-2*INT(Y/2))
40 HPLOT 16,Y TO 263,Y
50 NEXT
```

Cycle V Off, Light, Medium, Strong. Off shows separate green/orange lines.
The enabled levels should progressively mix them, with Strong producing the
same gold mixture on both phases in the interior. Repeat in Color TV and PAL
Accurate TV. Their decoder colors may differ; the blend control follows the
same brightness rule. This change does not add a PAL chroma delay-line decoder.
PAL Accurate modes are offered only when the detected Apple source is 50 Hz.

## 2. Bright and dark edges; H/V consistency

Use `HGR: POKE 49234,0: HCOLOR=3: HPLOT 16,96 TO 263,96` for an isolated white horizontal
line. Test each V level with other effects Off. The line should retain its
bright center and gain a weak halo into black. Black should no longer act as
an abrupt on/off boundary. Repeat with HCOLOR 1 and 5.

Use `HGR: POKE 49234,0: HCOLOR=3: FOR Y=16 TO 175: HPLOT 140,Y TO 141,Y: NEXT`
and test H instead. The paired columns keep the line white; a single HGR
column can produce an artifact color. Compare
Light/Medium/Strong at 1x and 2x: enlargement should repeat the selected native
blend, without adding a second horizontal blur. Check white text and dim text
on black, colored edges, near-black ramps, and solid fills. Solid fills must
remain unchanged with H/V alone.

Then enable H+V together. Add Glow and Ghosting separately, and check that
trails decay without accumulating a wider blur each frame.

## 3. Masks, clipping, and UI

With other effects Off, cycle Pixel mask Off, Aperture grille, Shadow mask,
LCD grid over white and colored fills. Aperture uses repeating RGB columns;
Shadow shifts them on alternate rows; LCD dims every third row or column.
LCD intersections should receive one attenuation. These are three-output-pixel
patterns, so size changes must not enlarge the cells.

Check all six output resolutions at Max, 1x, and 2x where they fit. Pay special
attention to 1360x768 and 1200x800 with the Apple border enabled, and SHR at
1280x1024: partially off-screen borders must not disable the mask. The pattern
starts at the visible clipped Apple viewport. Toggle the border and its flood.
The decorative bezel and flood outside the viewport should remain unchanged.

Enable the format badge, debug overlay, disk activity, and both transient
notices. They must stay sharp and unmasked. Open and close the menu, switch
resolutions repeatedly, and reset: no mask may stick to the menu or a bezel.
At large menu sizes, verify H/V share a row, Scanlines/Pixel mask share a row,
and Glow is left of Ghosting. The compact menu puts those controls on separate,
adjacent rows in the same order, with Glow before Ghosting.
Repeat a mask and H/V check in monochrome; these controls must remain available.

## 4. Screenshots and saved settings

Save A2 and full-output screenshots with each mask and with H/V, scanlines,
glow, ghosting, and the Apple border enabled. Inspect PNGs at 100% magnification:
the full image and corresponding crop must agree on mask phase, dimming and
UI exclusions. The bezel must match its unprocessed appearance.
Use a static scene for this comparison. Wait for each save and its confirmation
notice to finish before the next capture, so frame or overlay changes do not
affect the comparison. Default keys are F12 for A2 and PrintScreen for full
output; SD sharing must be Off.

During a full 1080p save, change the mask, output resolution and size, then
open the menu. The pending PNG must retain its captured pixels, dimensions,
mask and exclusions. The next capture must use the new frame. A request during
the brief resolution transition may report `NO OUTPUT FRAME` until the first
new composite completes. Check input and audio response while saving.

Save settings, reboot, and reload a profile. Check H/V strengths and mask.
Loading an older profile without `video.pixel_mask` should select Off. An
explicit `video.blending.horizontal=OFF` must override old horizontal smoothing
or Blur/Dot entries regardless of key order.

## 5. Performance

Use the same French Touch DIX scene, size and effects on both firmware images.
Hide the debug overlay while measuring. At 1920x1080 PAL Accurate TV, the
reported black-protection baseline was **267 / 164 / 94 / 74 FPS** for
**Off / H / V / H+V**. It is a comparison point for this board and scene.

1. Run `:comp uncap on`. Wait at least one second after closing the menu for
   each setting, then sample the first FPS value (`apple_fps`) from `:status`
   several times. Measure all three H/V strengths separately
   and together at 1920x1080 and 1360x768; repeat Idealized and PAL Accurate TV.
   Include both blank/flat areas and busy text or graphics: H-only cost now
   depends on how much of each row has the same starting color.
2. Repeat Off and H+V with each Pixel mask. Mask selection should add no ARM
   pixel pass and should not materially change the measured frame rate.
3. Run `:comp stats` before and after equal measurement intervals. Record
   changes in published/Apple-drawn counters, scanout underruns and AXI errors.
   Its timing fields describe one recent frame, not a sustained FPS average.
4. Run `:comp uncap off` and play DIX normally. Check smooth motion and audio,
   then add Glow/Ghosting, disk access and USB input. Return to this paced mode
   for normal use. Record any visible stutter even if a single timing sample
   looks fast.

Record the image hash, output resolution, size, source/video mode, strengths,
FPS range and counter deltas for each result. Screenshots have separate
snapshot/SD costs; measure those pauses separately from normal filtering.

## 6. TURBO write-path regression

The timing change keeps the same write permission but changes how the FPGA
builds that logic. In TURBO, run DIX through disk loading and several scene
changes, then pause/resume and reset/reload it. Watch for corrupted graphics,
failed loads, hangs, or unexpected writes while paused. Repeat a disk load at
native speed. If you use RamWorks, run your usual banked-memory test in TURBO
and repeat a bank switch after pause/resume. Compare any failure with the
preceding image; the logic change should add no cycle or visible behavior.
