# Monochrome dot bleed

Legacy mono output shapes each dot during the compositor's 2x row
expansion. This reduces the thick dark gaps seen in dithered HGR images.
The Video tab control "Dot bleed" selects Off, Light, Medium, or Strong.
Light is the default. It shares the "Color mode" row (labelled "Mono color"
in Monochrome) and appears only with Monochrome output. With Color output,
navigation skips this hidden control.

Shaping runs on complete mono frames, including green, amber, white, and
Video-7 automatic white mono. It supports normal legacy video and the
384-row legacy weave. SHR keeps its existing rendering path.

Each source dot produces two brightness samples, one per output column.
All weights sum to one, so solid fills keep their brightness. Row ends
repeat the end dot and never sample a colored border.

```
Light   left = (previous + 3*current) / 4
        right = (3*current + next) / 4
Medium  left = (12*previous + 20*current) / 32
        right = (20*current + 12*next) / 32
Strong  left = (14*previous + 17*current + next) / 32
        right = (previous + 17*current + 14*next) / 32
```

For an HGR bit pattern of on, off, on (each bit is two source dots), the
twelve output columns read:

```
Off     255 255 255 255   0   0   0   0 255 255 255 255
Light   255 255 255 191  64   0   0  64 191 255 255 255
Medium  255 255 255 159  96   0   0  96 159 255 255 255
Strong  255 255 247 143 112   8   8 112 143 247 255 255
```

The gap edges brighten from 64 to 96 to 112 as bleed increases. Light and
Medium keep the two center columns black; Strong raises them to 8, about
3% brightness. The values above precede RGB565 quantization and assume no
other effects. For alternating single dots, the on and off levels are 191/64
(Light), 159/96 (Medium), and 135/120 (Strong).

The filter reads the strongest channel of the renderer's tinted pixels,
then uses a 512-byte table to tint and pack each result to RGB565. The
table rebuilds only when the mono tint changes. Light uses no
multiplication; Medium and Strong use small constant multipliers.

The compositor shapes each cached source row once and reuses the expanded
row for its two or four output scanlines. Normal HGR and DHGR both produce
1120 x 192 shaped samples per frame; a legacy weave produces 1120 x 384.
There is no extra framebuffer pass, frame-sized buffer, or output readback.
The colored border uses the existing RGB expansion. Blur, glow, and ghosting
still work; shaping follows those effects and never enters ghosting history.
On ARM, the mono glow pass uses NEON to add eight pixels at a time, with a
scalar tail for shorter rows. It keeps 8-bit channels in the cached scratch
row so dot bleed can filter neighboring samples before RGB565 conversion.
The dot-bleed filter itself stays scalar; color glow keeps its combined
NEON glow, RGB565 conversion, and pixel-doubling loop.
With Dot bleed Off and no other effect, color and mono frames both stay on
the plain fast path.

The renderer publishes the mono flag and tint with the frame's slot and
format. A change between mono/color or between tints during capture clears
the flag for that frame. CPU0 therefore cannot reshape color pixels using
a newer menu setting. The next complete mono frame enables shaping again.

The setting persists as `video.dot.bleed` (OFF, LIGHT, MEDIUM, STRONG).
A config without the key, or with unknown text, reads as Light. The debug
overlay's Video panel shows the level after the mono tint.

For comparison with a CRT, start with Phosphor blur and glow off to isolate
this change. Pixel centers, image size, HGR half-dot delays, and vertical
scanline spacing stay fixed.

Validation on 2026-09-11:

- `python scripts/test_video_mono.py` compiles the production shaper, row
  compositor and RGB565 blits on the host. It checks the four profiles
  above, solid fills, row bounds, tint peaks, colored borders, both
  vertical scales, all scanline strengths, the Off fast path, and all
  blur/glow/ghosting combinations at Light and Strong.
- `python scripts/test_video_output_config_menu.py` checks the menu row,
  its Monochrome-only guard, persistence, help, and the frontend wiring.
- The host render harness passes, including mono frame tags, Video-7 mono,
  a mid-frame mono/color change, and SHR exclusion.
- The CPU0 frontend builds in Vitis 2025.2 with no new warnings. CPU1 is
  unchanged by the menu row; the renderer already publishes the mono tag.

Validation on 2026-09-12 for the NEON mono glow pass:

- `python scripts/test_video_mono_neon.py` compiles the production row
  compositor for Cortex-A9 and runs 2,160 cases in Unicorn. It compares the
  8-bit scratch row and final RGB565 pixels with a scalar reference across
  all blur, glow, tint, and dot-bleed levels with glow and bleed enabled.
  Widths around vector boundaries and full display widths cover scalar
  tails, colored borders, saturation, alpha, and row bounds. The script
  lists its ARM compiler and Unicorn dependencies.
- The existing mono, video-output, border, and VidHD/SHR tests pass. The
  CPU0 frontend compiles and links in Vitis 2025.2 with no new warnings.

Validation on 2026-09-13 for the revised Medium and Strong profiles:

- The host mono test checks the exact brightness samples above before
  RGB565 quantization, as well as the packed pixels and compositor paths.
  Light and Medium keep the two center columns black; Strong produces 8.
- All 2,160 Cortex-A9/Unicorn cases pass with the revised scalar reference.
  The video-output menu and compositor border tests also pass.
- Host checks of the shared color/bleed row pass in both output modes,
  including control positions, focus, and navigation past hidden dot bleed.
- No firmware rebuild or board test was run for this profile change.

CRT appearance and added render time still need measurement on the board.
The existing `g_compositor_last_apple_us` counter measures the Apple blit
including this stage; compare the same static image and effects at each
level. Medium and Strong cost more than Light per row.
