# Monochrome dot bleed

Legacy mono output shapes each dot during the compositor's 2x row
expansion, like the spot of a CRT. This reduces the thick dark gaps seen
in dithered HGR images. The Video tab row "Dot bleed" selects Off, Light,
Medium, or Strong. Light is the default. The row is active only with
Monochrome output; with Color output it shows dimmed and ignores input.

Shaping runs on complete mono frames, including green, amber, white, and
Video-7 automatic white mono. It supports normal legacy video and the
384-row legacy weave. SHR keeps its existing rendering path.

Each source dot produces two brightness samples, one per output column.
All weights sum to one, so solid fills keep their brightness. Row ends
repeat the end dot and never sample a colored border.

```
Light   left = (previous + 3*current) / 4
        right = (3*current + next) / 4
Medium  left = (10*previous + 20*current + 2*next) / 32
        right = (2*previous + 20*current + 10*next) / 32
Strong  left = (2*previous2 + 10*previous + 15*current + 5*next) / 32
        right = (5*previous + 15*current + 10*next + 2*next2) / 32
```

For an HGR bit pattern of on, on, off, off, on, on (each bit is two source
dots), the twelve output columns read:

```
Off     255 255 255 255   0   0   0   0 255 255 255 255
Light   255 255 255 191  64   0   0  64 191 255 255 255
Medium  255 255 239 175  80  16  16  80 175 239 255 255
Strong  255 239 215 159  96  56  56  96 159 215 239 255
```

Light keeps single dots sharp and softens only the gap edges. Medium and
Strong widen the spot, so gaps fill more but 80-column text loses
contrast. For alternating single dots, the on and off levels are 191/64
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

CRT appearance and added render time still need measurement on the board.
The existing `g_compositor_last_apple_us` counter measures the Apple blit
including this stage; compare the same static image and effects at each
level. Medium and Strong cost more than Light per row.
