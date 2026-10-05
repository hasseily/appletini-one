# Firmware F1.2.4

Branch: `codex/turbo-paging-dma`. Boot stays **B1.2.0**.

## Video and screenshots

F1.2.4 restores the **F1.2.2 Blur and monochrome Dot bleed** controls and
filter behavior. It removes the independent H/V light-space and
brightness-aware blending added in F1.2.3. Old F1.2.2 settings retain their
meaning; F1.2.3 settings migrate to the available legacy controls.

Screenshots again use the F1.2.2 sources and sizing rules. **A2** captures the
decoded Apple framebuffer with the legacy enlargement and scanline rules,
rather than the composed display. **OUTPUT SCREEN** captures the output
framebuffer. Neither path reproduces the FPGA pixel mask in the PNG.
The later asynchronous save, temporary-file protection and SD-sharing guards
remain in place.

Pixel masks, controller fixes, the independent glow/ghosting fixes and video
copy optimizations remain. Expensive effects still apply to the Apple image;
the bezel and UI keep their existing treatment. TURBO paging and AMEM copy/fill
retain the F1.2.2 improvements.

The Video tab places Scanlines and Pixel mask above Phosphor blur, followed
by Phosphor glow on the left and Phosphor ghosting on the right. Its value
fields share fixed columns, including full-width rows and the longer PAL
mode names. Keyboard focus follows the new row order; compact layouts keep
their single-column format.

## SSI263 filter frequency

Register 4 now shifts the speech tract's filter frequencies. It leaves the
source pitch, phoneme timing and request/IRQ behavior unchanged. The corrected
control uses `$80` (128) as the exact pre-FF response; the SC-01/Votrax path also
keeps its previous output.

All 256 values now select distinct, evenly spaced rates: `(128 + FF) / 256`.
`$00` selects **0.5x**, `$80` selects **1x**, and `$FF` selects **383/256x**
(about 1.496x). `$FF` remains audible. Reset still stores `$00`, so software
must write `$80` for the legacy response. This provisional mapping replaces
the first build's broad clamps; it is not the chip's divider law or a calibrated
analog model. See [the SSI263 implementation and test notes](docs/SSI263_FILTER_FREQUENCY.md).

The FPGA reuses its existing filter pipeline and MAC. The ARM rendering path
does no extra speech work. The corrected mapping measures at most 294 fabric clocks
per sample, within the roughly 2,778-clock budget. The fractional filter schedule
can add resampling artifacts away from the reference setting.

The duration reload calculation also uses an exact lookup to shorten a
critical FPGA path. Its values and same-cycle RATE-write behavior stay
unchanged.

## Validation

The corrected linear FF mapping passes speech simulation. Fresh FPGA signoff
and packaging are in progress; the existing root image is still the first,
clamped F1.2.4 build until the corrected image passes those gates.

Both ARM applications build successfully. Native and Cortex-A9 tests each
pass 704 exact F1.2.2 Blur/Dot comparisons, 321 retained-effect cases and 768
ghosting cases. Settings migration passes 8,192 combinations plus 4,800 real
load/save/reload cases. Screenshot tests pass 103 service cases and decode 17
PNG outputs; renderer/border checks pass 29,088 frames. All six menu layouts
and the retained mask controls pass their checks.

The corrected SSI263 test checks all 256 distinct rates, hot writes, source
and response timing, reset and SC-01 output. A separate comparison against
the actual F1.2.2 backend passes 24,584 exact PCM samples at FF128; FF127
fails as a negative control. The real Phasor card test passes 6,705 bus writes
and 3,072 audio frames, including the original demo's initialization and
both SSI sockets. The existing neutral waveform hash remains unchanged.
The duration lookup passes all 65,536 input-byte combinations.

The original Phasor demo's Pitch setting defaults to 232. Set it to **128**
for the old sound; values below and above 128 lower and raise the tract rate.

Historical signoff for the first clamped build passed with **+0.054 ns setup**, **+0.021 ns hold** and
**+0.265 ns pulse-width slack**. There are no failing timing endpoints,
unconstrained internal endpoints or routing errors. CDC, I/O placement and
DRC checks pass. The setup-slack requirement remains **+0.050 ns**; clock
rates and external timing limits remain unchanged.

The firmware image is `firmwares/F1.2.4-ssi-filter/FIRMWARE.BIN`, and remains archived for comparison. The archive includes build
inputs, source hashes, test evidence and timing reports. Hardware feedback on
that image reported a lower-sounding default and no useful response across
most of the FF range. Corrected-image validation and real-SSI calibration
remain pending.

Hardware checks for the corrected image after it is built:

- Compare Blur Off/Light/Medium/Strong and monochrome Dot bleed against
  F1.2.2 using the same image, resolution and size setting.
- Check Pixel mask on screen, and confirm screenshots omit its scanout
  pattern. Compare A2 screenshot dimensions and pixels with F1.2.2.
- Save screenshots while changing resolution and while using the menu;
  confirm completed PNGs open and existing files remain intact.
- Repeat French Touch DIX at 1360x768 and 1920x1080, with effects Off and
  legacy Blur enabled. Record normal playback plus uncapped frame rates.
- On each SSI263 socket, play a sustained vowel and a phrase at register 4
  values `$00`, `$40`, `$80`, `$C0` and `$FF`. Compare `$80` directly with
  the pre-FF firmware. Hold pitch, rate, articulation and amplitude constant:
  the tract should progress from darker to brighter while source pitch and
  request cadence stay fixed. Sweep 0–255 during speech; then check reset,
  amplitude zero and unchanged SC-01/Votrax playback.
