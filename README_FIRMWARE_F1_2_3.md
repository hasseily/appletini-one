# Firmware F1.2.3 hardware validation

Branch: `codex/turbo-paging-dma`. Boot image version stays B1.2.0.
This firmware includes the earlier TURBO paging, video and input fixes.

The current candidate replaces exact-black protection with shared brightness-aware
**H blending / V blending** and adds **Pixel mask**. See the
[design, migration and validation notes](README_VIDEO_BLEND_MASK.md).
Use the [current hardware checklist](README_VIDEO_BLEND_MASK_TESTS.md) for this
candidate. The sections marked previous describe earlier images.

## Current luminance-aware build

Built **F1.2.3** on `codex/turbo-paging-dma`. The image is
[FIRMWARE.BIN](firmwares/F1.2.3-luma-grille/FIRMWARE.BIN).
Board validation remains pending; this image has not been promoted as known good.

- Nominal setup **+0.050 ns**, hold **+0.034 ns**, pulse width **+0.265 ns**.
  Pixel-domain setup is **+0.355 ns**; DVI setup is **+0.872 ns**.
  All timing failures and unconstrained internal endpoints are zero.
- Independent clock, CDC, reset-epoch, 19 output-register IOB, routing,
  bus-skew and design-rule checks pass. Bus-skew margin is **+5.852 ns**.
  Final user uncertainty is zero; clocks and external timing limits are unchanged.
- Image size: **4,439,404 bytes**. Payload size: **4,439,372 bytes**.
  Payload CRC32: **57946A06**. Firmware role and recovery capability verified.
- Image SHA-256:
  `efcbfccb7add17bf992ecb439ca8b8e1bf35cdbd160d8eb70440f49a9ad931ee`.

The build archive preserves the exact bitstream, ARM images, source snapshots,
test results, timing reports and build provenance. The repository-root
`FIRMWARE.BIN` contains the same image.

- H and V use the same Light/Medium/Strong weights and brightness guide.
  Similar-brightness colors receive the full mix. Dark neighbors taper to no
  dimming, while bright neighbors retain a small spill into dark pixels.
  H alone reuses constant-color row prefixes while keeping transition pixels
  in the filter; the shortcut produces the same output.
- Off, Aperture grille, Shadow mask and LCD grid run in FPGA scanout. They
  cover the visible Apple viewport and its enabled border, with UI exclusions.
  The decorative bezel and flood outside that viewport stay unchanged.
- Screenshots copy a completed output and reproduce its FPGA mask while saving.
  The captured crop coordinates, dimensions, stride and mask settings stay
  fixed even if the user changes settings during the save.
- H/V remain adjacent in the Video tab. Scanlines/Pixel mask and Glow/Ghosting
  form the next pairs, with Glow before Ghosting. Compact layouts use adjacent
  rows in that order. Expensive effects stay in the native Apple image.

Both Cortex-A9 builds pass. Native and ARM blend outputs match across the
integer-root, brightness-pair, color and compositor checks. Screenshot tests
pass 78 service cases with independent PNG decoding. Mask tests pass 5,898,240
values and 72 real viewport layouts; renderer/border checks pass 29,088 frames.
All six FPGA mask, output-mode, reader and restart simulations pass. These
tests establish arithmetic and integration behavior, not physical frame rates.
The prefix shortcut also passes 2,400 targeted cases and 494,400 pixels on each
backend. Two source-read traces confirm one read per original source pixel,
including the H-only prefix path with border, glow and ghosting.

The matched ARM instruction profile against the black-protection image shows
H+V using **15–19% fewer** instructions. H alone ranges from **67.4% fewer**
on uniform rows to **53.4% more** on busy rows across 49 paired cases. V Light
uses 1.5% fewer, Medium is nearly unchanged, and Strong uses **10.3% more**.
The new H path mixes in light space. Hardware FPS and worst-case frame times still need comparison;
the no-performance-loss requirement is not yet established on the board.

PAL Accurate retains its existing decoder; this change does not add a PAL
chroma delay line. The brightness rule and gamma-2 light estimate are practical
approximations, not a calibrated model of a named CRT.

### Timing compared with F1.2.1

The archived F1.2.1 Four Play image had **+0.181 ns** global setup slack and
**+0.207 ns** within the fabric clock domain. The later TURBO paging image
had **+0.049 ns**, which the user accepted for that image; the preceding
F1.2.3 image improved this to **+0.051 ns**. The new candidate still requires
at least **+0.050 ns**.

The two earlier designs used the same XDC constraints. Fabric and pixel
periods remain 7.499 ns and 6.733 ns. TURBO paging added the copy engine, its
bus integration and PSRAM burst support. Its accepted critical path runs
from copy-engine state to shadow BRAM write enable, with 81.9% of its data
delay in routing. This new path sets the measured fabric minimum;
placement changes elsewhere can also affect the final margin.

Four Play used 36,240 LUTs, 23,668 registers and 11,470 occupied slices. The
accepted TURBO design used 37,056 LUTs, 23,973 registers and 11,624 slices;
both used 110 BRAM tiles and seven DSPs. The new masks add FPGA logic, while
the H/V blend and screenshot software runs on the ARM cores. The current
DVI constraint change fixes output-register packing; it does not relax
clock periods or external timing limits.

### TURBO shadow-write permission

The current source gives TURBO shadow writes a local completion expression
instead of the shared OR of all CPU completion types. It keeps the same
pause, ownership-hold, reset/session, Disk II timing, pacing, and cache
invalidation guards. It adds no state or cycle. Fresh synthesis confirms that
the shared CPU-enable driver is absent from the shadow write-enable logic.
The retirement guards now feed a separate local write path. The final fresh
layout, including a focused physical refinement, meets **+0.050 ns** setup.

Run `python scripts/test_vtw_shadow_write_guards.py` with XSim on `PATH`.
It instruments a temporary copy of the current core and compares each write
permission with the original expression. The focused run passed 550,479
clock-edge checks, observing all 18 legal states and four configured speeds
in aggregate. Real CPU programs cover parked writes, pause, hold/flush/release,
reset/session loss, mapping and policy changes, adjacent host writes, and
status/video barriers. A second temporary copy removes the pause guard; the
oracle correctly rejects it. Evidence is in
`build/vtw_shadow_write_guards/results.json` and its adjacent logs.

The earlier isolated qualification also passed 9,093,828 equation checks and
the seven focused TURBO benches, shadow-wide, copy-engine, and ONE//e
safety/runtime/bus tests. Its evidence remains in
`build/vtw_shadow_write_fallback/qualification.json`. These checks do not
claim every state/speed combination, illegal physical FSM-state equivalence,
or board timing. The independent routed checks and packaged image are recorded
above; hardware behavior still needs validation.

## Previous black-protection build result

The black-protection test image is archived in
`firmwares/F1.2.3-v-black-protection/`. The preceding image remains in
`firmwares/F1.2.3-effects-perf/`.
All identify as F1.2.3; use the image hash to distinguish them.

Image size: **4,433,772 bytes**. Firmware role, recovery capability and
payload CRC32 **E03E2A60** pass verification.

SHA-256: `8b6123e4591f8aace8a8374655bf082600d925d0ab6c5b46977a1f5c2e2df26f`

Only the V pixel helper and its menu help differ from the preceding image's
production sources. The exhaustive channel-math audit passes all 50,331,648
triples and all 65,026 ARM square-root inputs. The updated help fits all six
menu resolutions; 302,400 config/profile migration cases and all 14 video
configuration checks pass.

Native and ARM runs each pass 277,440 row samples, 4,866 compositor cases,
192 Off checks and 72 border checks. Black-specific coverage adds 1,152 row
cases (387,200 pixel samples) and 192 exact image comparisons, covering
alpha, one-code dark colors, zero channels, null neighbors, tails, scales,
captured borders and combined effects. Outputs match between both backends.
An independent reference reproduces all 81 old Strong golden images; the
new behavior changes eight of those images only as the black rule requires.
ARM memory checks confirm that the compositor still reads each source frame
once. Reports and reproduction scripts are in the firmware archive.

The unchanged FPGA image has **+0.051 ns setup slack**, exceeding the
**+0.050 ns** target. Hold slack is **+0.056 ns**, pulse-width slack
**+0.265 ns**; all have zero failing endpoints. Route, bus-skew and
constraint checks pass. This software revision reuses that verified image.
Both Cortex-A9 application builds pass; the build regenerates the embedded
CPU1 payload and relinks the frontend.

After this black-protection image, the user reported uncapped 1920x1080 PAL
Accurate TV DIX rates of **267 / 164 / 94 / 74 FPS** for **Off / H / V / H+V**.
The user found the appearance better, but exact-black exclusion too abrupt.
Strengths were not restated; these observations do not complete the full
hardware checklist. They supersede the preceding image's 85 FPS V result
for comparison with the new luminance-aware candidate.

The preceding effects-performance image has SHA-256
`08866fee18233d040ce2d07bb882366453ba9661dfaa076885f4aa08c7fb88e4`,
size 4,433,068 bytes and payload CRC32 0BEF15E7. Its archived tests include
28,800 native and 28,800 ARM row-copy checks, exact H math, and 39 paired
whole-frame comparisons. Those copy and H helpers remain unchanged.

## Previous V black-protection model

The user reported on hardware that the old V smoothing looked weak even at
Strong, while the CRT checkbox blended colors but made the image too fuzzy.
This revision replaces both controls with **V blending: Off/Light/Medium/Strong**,
beside H smoothing. The old V algorithm affected only inserted output rows;
the new control blends the actual decoded rows at every output scale.
Phosphor glow and Phosphor ghosting share a row, with Glow
on the left and Ghosting on the right.

| V setting | Previous row | Current row | Next row |
| --- | ---: | ---: | ---: |
| Off | 0% | 100% | 0% |
| Light | 6.25% | 87.5% | 6.25% |
| Medium | 12.5% | 75% | 12.5% |
| Strong | 25% | 50% | 25% |

Weights apply to approximate emitted light, not a plain encoded-RGB average.
The black-protection candidate keeps exact RGB black pixels black and ignores
black neighbors so they cannot dim a colored or white pixel. This applies
to Light, Medium and Strong without a new setting. Nonblack neighbors still
mix with the weights above; uniform colors retain their exact brightness.
Strong keeps the earlier CRT On mixing where all three pixels are nonblack.
Light and Medium mix less. Compare all levels on hardware to choose the
amount of color mixing.

V works in all six color modes, monochrome, legacy, woven and SHR output.
It uses the same source-row footprint at 1x/2x/4x; enlargement repeats the
filtered row. Edges repeat the nearest picture sample, and captured border
and picture use separate neighbors. UI and bezel stay sharp. Ghosting, H
and glow run before V, so black protection uses their result, not the
original decoded pixel. A halo or trail can make that pixel nonblack and
eligible for V mixing. Spatial effects never feed back into ghost history.

Color TV and PAL Accurate TV no longer add an automatic vertical average.
V is the sole vertical-blending control: **Off means Off in every mode**.
Their decoder/color behavior remains. PAL Accurate still uses NTSC-derived
color tables and does not implement a PAL chroma delay-line decoder.

New settings save `video.blending.vertical=OFF/LIGHT/MEDIUM/STRONG`.
An explicit new key wins regardless of key order. Without it, old CRT On
maps to Strong; old CRT Off preserves the old V/Blur level. `IDEALIZED_MIX`
maps to Idealized + V Strong unless an explicit old CRT Off overrides it.
Global settings and profiles use the same rules. H keeps its existing key.

The filter uses corrected RMS as a fast gamma-2.2 approximation. After black
selection, the channel kernel stays unchanged. Exhaustive checks of all
16,777,216 triples per enabled level bound its error to 4/255 for Light and
Medium, and 3/255 for Strong. Constant colors stay exact; ARM and scalar
square roots match for every valid input. Reproduce the math audit with
`python scripts/test_crt_blend_math.py --neon`.

This is a beam-width approximation, not a calibration for a named tube.
Period studies describe a Gaussian beam whose width depends on focus and
drive: [1979 CRT study](https://opg.optica.org/ao/abstract.cfm?uri=ao-18-12-2033),
[1981 CRT measurements](https://www.sciencedirect.com/science/article/pii/014193828190158X).
PAL color-delay processing is a separate decoder effect; see
[Philips' 1968 PAL delay-line paper](https://pearl-hifi.com/06_Lit_Archive/02_PEARL_Arch/Vol_16/Sec_53/Philips_Tech_Review/PTechReview-29-1968-243.pdf).

## Other changes in the previous F1.2.3 image

- H smoothing remains Off/Light/Medium/Strong, with native horizontal
  filtering and centered 2x interpolation. H and V are adjacent in the UI.
- Glow uses its own fixed native 3x3 kernel. Ghosting handles the 4/32/128
  channel thresholds correctly. Neither effect changes V's weights.
- With V enabled, scanlines dim the lower half to 75/50/25% at 2x and 4x.
  With V Off, the earlier 4x black-row pattern remains. Native 1x never
  blanks rows. Old Blur and Dot bleed settings still migrate.
- Screenshots snapshot the completed output. **PRTSCR A2** crops the visible
  Apple area, including its effects and border; **OUTPUT SCREEN** retains the
  whole selected output resolution. PNG writing proceeds two rows per poll.
  Exclusive temporary files and collision-safe publication prevent overwrites
  and incomplete final PNGs. SD-sharing handoff cancels an active save.
- Generic HID descriptors retain sparse usages and independent report-ID
  offsets. Up to 32 buttons have distinct menu/system bindings and preview
  state. Partial reports preserve other held buttons. Unbound LB/RB navigate
  tabs, with both-held cancellation and learning/calibration guards.
  Apple PB0..PB2 and slot-2 SNES layouts keep their existing mappings.

The filter and HID work adapts Multitini One changes through `3747bc8`
(2026-10-02), especially axis smoothing `7d73b12` and descriptor fixes
`1c4f8be`. Its complete FPGA video frame pipeline is not part of this port.

## Black-protection instruction cost

The paired ARM run compares this image's sources with the archived
effects-performance sources. It uses the archive's actual memcpy code and
a centered 560x192 image at 2x width, 4x height on 1920x1080 output.

| Effects | Effects-performance image | Black protection | Added instructions |
| --- | ---: | ---: | ---: |
| Off | 447,842 | 447,842 | 0% |
| V Light | 3,026,474 | 3,402,410 | 12.42% |
| V Medium | 2,985,770 | 3,348,266 | 12.14% |
| V Strong | 2,675,690 | 3,024,938 | 13.05% |
| H Light + V Light | 4,904,042 | 5,186,474 | 5.76% |
| H Strong + V Strong | 4,513,706 | 4,876,394 | 8.04% |

All nonblack comparison images match the preceding output byte for byte.
The black-row image changes as intended. The masks add work in the existing
row pass, without another framebuffer pass or buffer. These counts are
instructions, not cycles or FPS; hardware smoothness and rates need testing.
The complete compositor cost is higher than the earlier isolated-kernel
estimate. See `validation/crt_black_protection_test/matched_cost.json` in
the new firmware archive.

## Previous effects performance revision

The user confirmed that graded V blending looks right, then measured these
uncapped rates in the French Touch DIX demo on the preceding
`F1.2.3-v-blending` image:

| Effects | 1360x768 | 1920x1080 |
| --- | ---: | ---: |
| Off | 303 FPS | 303 FPS |
| V alone, any strength | 60 FPS | 60 FPS |
| H alone, any strength | 81 FPS | 81 FPS |
| Glow alone | 103 FPS | 103 FPS |
| Ghosting alone | 129 FPS | 129 FPS |
| H + V | 43 FPS | 22 FPS |

These are user-reported observations, not automated board measurements.
The earlier report of V falling below 50 FPS was withdrawn before uncapped
measurement. The output mode, source format, Size multiplier and full UART
timings should accompany the next comparison.

The later UART snapshots, both in Legacy/Max with `uncap=1`, reported
`total/ui/apple` of **5721/3/5717 us** at 1360x768 and **5735/3/5731 us**
at 1920x1080. The user confirmed both H and V were enabled and read the first
FPS field in `:status`, `apple_fps`, without using the on-screen overlay.
That field counts completed Apple-image draws over elapsed wall time, sampled
at intervals of at least half a second. It includes repeated draws when uncapped.
The 5.7 ms values describe only the last compositor call and exclude foreground
work between calls; they do not establish sustained 175 FPS or invalidate the
22/43 FPS averages. The gap between these measurements remains unexplained.
Debug-overlay drawing does not explain this user's measurements.
Both snapshots reported 56,160 cumulative scanout underruns and zero AXI read
errors. Equal cumulative counts alone do not show an active underrun problem.

After the optimized image was supplied, the user reported these new uncapped
`apple_fps` results at **1920x1080, PAL Accurate TV**:

| Effects | Reported rate | Average interval (1000 / FPS) |
| --- | ---: | ---: |
| Off | 267 FPS | 3.75 ms |
| H | 164 FPS | 6.10 ms |
| V | 85 FPS | 11.76 ms |
| H + V | 74 FPS | 13.51 ms |

H+V exceeds both 50 and 60 draws per second in this test. Its average
interval leaves about 6.5 ms within a 50 Hz frame period, or 3.2 ms at 60 Hz.
The user then confirmed that Idealized gives the same rates. Color mode is
not a material factor in these measurements: H and V share the same filter
path after color decoding. Finally, the user confirmed that **DIX plays
smoothly with `comp uncap off`**. This validates the reported DIX stuttering
fix under normal pacing. H/V strengths were not restated with the new results;
worst-case frame times and other workloads still need hardware checks.

This revision preserves the same pixels and effect strengths while reducing
work in three places:

- Video row copies use explicit 32-byte NEON load/store groups. The linked
  library's aligned memcpy used separate 8-byte VFP transfers; the earlier
  instruction harness substituted a wider copy and missed this difference.
- H filtering and centered enlargement use exact byte averages in place of
  packed-channel multiplication. V removes a redundant reciprocal-square-root
  refinement; integer corrections still produce the same rounded roots.
- The compositor retains one packed V row for all repeated output rows and
  applies scanline attenuation when writing them. It no longer stores two
  identical rows. Only H enlargement splits rows at horizontal picture/border
  boundaries; V can process the complete row with fewer scalar tails.

The new paired performance harness executes the previous firmware's actual
memcpy and each version's own framebuffer code. Both use the same compiler
settings and centered 1920-pixel output stride. For a 560x192 source enlarged
to 1120x768, the executed ARM instruction counts are:

| Effects | Previous image | This revision | Fewer instructions |
| --- | ---: | ---: | ---: |
| Off | 745,826 | 447,842 | 40.0% |
| V Light | 4,158,891 | 3,026,474 | 27.2% |
| V Medium | 3,957,291 | 2,985,770 | 24.6% |
| V Strong | 3,660,651 | 2,675,690 | 26.9% |
| H Light | 4,293,852 | 2,305,490 | 46.3% |
| H Strong | 4,293,852 | 2,278,610 | 46.9% |
| Glow Medium | 3,343,071 | 1,997,529 | 40.2% |
| Ghosting Medium | 1,222,059 | 852,833 | 30.2% |
| H Light + V Light | 7,616,811 | 4,904,042 | 35.6% |
| H Strong + V Light | 7,616,811 | 4,877,162 | 36.0% |

All paired output hashes match. These instruction and transfer counts are
not cycles or FPS. The hardware rates above are separate user observations;
broader hardware checks and worst-case frame times remain pending.

## Before testing

Keep the preceding `F1.2.3-effects-perf` image and a copy of
`0:/appletini_cfg.txt` for comparison.
After updating, check that the About screen and UART report **F1.2.3**.
Start with H smoothing, V blending, scanlines, glow and ghosting Off. Set Size multiplier to Max.
Record the display model, resolution, Apple/ONE//e mode and PAL/NTSC cadence.

Use normal text/HGR/DHGR plus SHR and woven legacy images. Repository examples
include `software/legacy_demo_images/finder.hgri`, `face.dhri`, `playfield.dhr`,
and `software/shr4_demo_images/eye320.shr4`, `fluidart.shr4i`, `beach.3200`.
Load them with the matching existing demo/viewer; their extensions are not
interchangeable.

## Previous black-protection checks

| Test | Steps | Expected result |
| --- | --- | --- |
| V blending strengths | With other effects Off, run the green/orange program below. In Idealized cycle V Off/Light/Medium/Strong at 1024x768 and 1920x1080; repeat in all color modes. | Off keeps separate rows; Light and Medium progressively reduce their contrast; Strong retains the previous mixing in the nonblack interior. Black outside the pattern stays black. |
| Black protection and boundaries | With H/Glow/Ghosting/Scanlines Off, show isolated white and colored lines, then alternating black/white rows; cycle every V level in mixed text, DHGR80, mono, SHR and A2Li weave/page-merge. Enable a bright colored border. | Exact black stays black; black neighbors do not dim the white or colored line. Other nonblack neighbors still mix; solid fills stay exact. The border cannot bleed into the picture; no stale rows appear at boundaries. |
| Video controls | Navigate the Video tab at 1024x768 and 1920x1080; save V Medium in a profile and reboot. | H smoothing and V blending are adjacent. Glow and Ghosting share a row, with Glow on the left. There is no CRT checkbox. Focus follows visible order. V remains available in Mono and survives reload. |
| Axis independence | In Idealized or Composite Monitor, show vertical strokes with V Off and cycle H. Then show adjacent nonblack horizontal strokes with H Off and cycle V. | H mixes columns; V mixes nonblack pixels in neighboring rows at every scale. Light and Medium mix less than Strong, with no movement or resizing. |
| TV control | Set H/V/Glow/Ghosting/Scanlines Off. Compare Composite Monitor with Color TV, then PAL Accurate Composite with PAL Accurate TV. Cycle V in each mode. | V Off disables vertical blending, including in TV modes. All three levels apply normally. No hidden automatic TV row average remains. Decoder hues may still differ. |
| 1x and 2x | Repeat at Size multiplier 1x and 2x where it fits. Use SHR at 1024x768 or 1360x768 for native 1x; at 1280x1024 or 1920x1080 for 2x. | V applies the same source-row weights at every scale; it no longer disappears at native 1x. H retains native smoothing plus centered interpolation at 2x width. |
| All output modes | Switch through 1024x768, 1200x800, 1280x1024, 1360x768, 1680x1050 and 1920x1080. Repeat Max/1x/2x. | Stable sync, correct centering and clipping. Size choice survives a mode where 2x cannot fit. Native smoothing strength does not change merely because the output mode changes. |
| Scanline levels | On legacy video compare all levels at 1024x768 (normally 2x height) and 1920x1080 (normally 4x). Repeat with V Off/Light/Medium/Strong, then SHR/woven at 1x/2x. | With V enabled, the lower half retains 75/50/25% intensity at both 2x and 4x. V Off keeps the old 4x black-row pattern. At 1x, scanlines preserve every row. |
| Tints and edges | Test White/Green/Amber, Video-7 forced mono, color DHGR and SHR. Enable the IIgs border and cycle its color; test with and without a bezel. | Solid fills retain their color and brightness with smoothing alone. Picture samples do not pull colored border pixels inward. UI and bezel remain sharp. No stale rows at the top, bottom or border transitions. |
| Glow | Set Glow Medium and Ghosting Off. Toggle H and V separately over a bright point/line on black. Then cycle Glow levels. | H/V do not change the kernel used to make the glow halo. Glow can make pixels nonblack before V, allowing V to mix them. Glow strength changes added light. Enlargement scales the halo with the picture. |
| Ghosting | Move bright and medium-gray objects over black. Cycle ghosting levels. Change mono tint, video format and output resolution while trails exist. | Trails decay smoothly; Light clears sooner than Medium/Strong. Medium-gray trails do not linger in the wrong decay band. No old-tint trail or stale image after mode changes. H/V/glow do not spread into later history. |
| Config and profiles | Load an old profile with CRT On: expect V Strong. Load old CRT Off/V Medium: expect V Medium. Save V Light, reboot and reload. Test old IDEALIZED_MIX and an explicit new Off. | One V strength persists as video.blending.vertical. New explicit values win all key orders. Legacy explicit CRT Off suppresses IDEALIZED_MIX, while old V/Blur supplies the fallback. Old H/Dot migration remains. |
| Screenshot content | Capture A2 and full output during animation, first legacy, then woven legacy and SHR. Repeat with border, smoothing, scanlines, glow and ghosting enabled. While a full PNG saves, change output resolution/Size multiplier and open/close the menu. Capture again after completion. | PNG opens and matches the captured displayed frame. A2 has the Apple crop; output has the full selected dimensions. The pending PNG keeps its original pixels and dimensions; the next capture uses the new geometry. No later-frame tearing, wrong crop or stale resolution. |
| Screenshot responsiveness | Save full 1920x1080 while moving a controller and playing animation/audio. Trigger a second capture while busy, then capture again after completion within the same RTC second if possible. | Input/rendering resume between write batches. Busy gives visible feedback. Completed captures have distinct names; earlier files stay intact. A brief snapshot/storage handoff pause is possible and should be measured. |
| SD failures/sharing | Try a full or write-protected test SD card, and removal during saving. Try a screenshot in USB SD sharing and FTP SD sharing. Enter sharing during a save. | Failure is visible and logged. No incomplete final `.png`. If cleanup fails, UART names the retained `.part`. Sharing rejects capture with `STOP SD SHARING FIRST`; entering sharing cancels pending local writing. |
| Controller extras | In USB bindings, learn stick clicks, Guide/PS and DS4 triggers/touchpad separately. Save/reboot; bind an extra button to a screenshot or TURBO speed action. | Each button has its own Gamepad number and triggers only its assigned action. Holding Guide while moving sticks or pressing another button retains Guide until released. See `README_USB_JOYSTICK.md` for exact indices. |
| Shoulders and guards | In the main menu press LB, RB, both together, then reverse direction without releasing both. Repeat during binding learn and joystick calibration. Close the menu and play. | Unbound LB/RB select previous/next tabs; both cancel tab movement. Learning captures the button without leaving the row. Calibration/gameplay do not navigate tabs. An explicit shoulder binding takes precedence. |
| HID compatibility | Test a generic pad, DS4, Xbox One and two pads at once. If available, test a device with separate input report IDs and buttons above 8. Unplug one while holding a button. | Axis mappings remain correct, no held buttons vanish on unrelated reports, and disconnect releases only that device. PB0..PB2 and 4Play/SNES MAX retain their layouts. |
| Mixed input holds | Hold a mouse button while entering and leaving pad binding learning. On a composite pad/keyboard device, hold a pad menu action while sending unrelated keyboard reports. | Learning does not turn the held mouse button into a new press. Keyboard packets do not cancel the held pad action. Each real release occurs once. |
| TURBO and bus checks | Run the same large TURBO paging workload used for F1.2.2. Exercise AMEM copy/fill, Disk II, SmartPort, USB input and reset while video effects are enabled. | No new paging stalls, corrupt copies, disk timeouts or stuck input. Boot, normal Apple mode and ONE//e still work as before. |

For a simple HGR stroke target, enter this Applesoft program. Vertical strokes
isolate horizontal smoothing; swap the HPLOT coordinates for horizontal strokes.

```basic
10 HGR: HCOLOR=3
20 FOR X=16 TO 175 STEP 16
30 HPLOT X,16 TO X,175
40 NEXT
```

For horizontal strokes use `30 HPLOT 16,X TO 263,X`.
Use Idealized or RGB color mode to inspect filter shape without changing the
native composite decoder's artifact colors.

For alternating green/orange HGR lines, use:

```basic
10 HGR
20 FOR Y=16 TO 175
30 HCOLOR=1+4*(Y-2*INT(Y/2))
40 HPLOT 16,Y TO 263,Y
50 NEXT
```

With H/Glow/Ghosting/Scanlines Off, cycle V Off, Light, Medium and Strong.
Off should leave distinct green/orange rows. Light and Medium should reduce
the row contrast while preserving more detail. Strong should match the
previous CRT On mixing in the nonblack interior; surrounding black should
stay black. Compare all levels at 1024x768 and 1920x1080 and in TV
modes. Scanlines deliberately darken the lower half; test them separately.

For a white-line target use `HCOLOR=3` on one horizontal row, with black
above and below. With H/Glow/Ghosting/Scanlines Off, V should keep the line
at full brightness and leave the surrounding black untouched at every
strength. Repeat with a colored line. Then add a different nonblack color
on a neighboring row and confirm that V still mixes the two colors.

## Performance acceptance

Use the same animation and settings on the preceding effects-performance image
and this black-protection image. Measure with the debug
overlay hidden first; opening the overlay changes the workload. Leave normal
vblank pacing enabled (`comp uncap off`). On the control UART, run `comp stats`
several times during each test and record `total`, `apple`, published-frame and
Apple-drawn counters, scanout underruns and AXI read errors. Compare counter
deltas over the same interval, not just one instantaneous frame or `skipped`.

Compare **V Off/Light/Medium/Strong** in the same color mode and busy HGR
animation, first with all other effects Off and then with H/glow/ghosting
enabled. Require the same source-frame cadence and no new backlog or stalls.

Check both the normal workload and the worst combination: 1920x1080, SHR or
captured legacy border, H/V/Glow/Ghosting Strong, with USB and SD traffic.
Repeat at 50 Hz and 60 Hz if both sources are available. Frame budgets are
20 ms and about 16.67 ms. Require no new missed fresh frames, scanout underruns,
AXI errors, visible stalls or storage timeouts. Measure screenshot pauses
separately; SD-call and USB-disconnect latency depend on the board and card.

Host and emulated-NEON tests establish arithmetic, clipping and ownership
behavior. They do not establish physical Cortex-A9 frame-time equivalence.
Hardware results are pending; record observations below rather than marking
this image hardware-validated before these checks run.

| Board/display/source | Test and settings | Result / UART evidence |
| --- | --- | --- |
| User board/display not specified | Previous `F1.2.3-effects-perf` image: French Touch DIX, tested H+V configuration, normal pacing (`comp uncap off`) | User confirms smooth playback. Uncapped 1080p H+V reports 74 FPS in PAL Accurate TV and Idealized. |
| User board/display not specified | Black-protection image: French Touch DIX, uncapped 1920x1080 PAL Accurate TV; strengths not restated | Off/H/V/H+V: 267/164/94/74 FPS. Appearance improved, but the black cutoff looked too abrupt. Remaining checks are pending. |
