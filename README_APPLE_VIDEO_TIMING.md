# Apple scanner timing

On a physical Apple host, `apple_timing_gen` advances once per
`ab_read.sss_en` strobe from the PHI0 sampler in `apple_bus_wrapper`.
The counter uses 65 cycles per line and 262 NTSC or 312 PAL lines per frame.
VBL starts at line 192. The same line and cycle feed capture timestamps,
MouseCard VBL, the HDMI activity pulse, and vTW scanner reads.

Apple RES# must neither stop this counter nor force it to frame zero.
The old `apple_top` connection seeded line 0, cycle 0 when RES# rose and
cleared the VBL phase lock while RES# stayed low. CTRL-RESET could therefore
shift both the scanner phase and the next VBL pulse.

The physical-host path now keeps its counter and phase lock through both
RES# edges. The first boot-ROM `CMD_VBL_START` after FPGA reset still
calibrates the counter to line 192, cycle 15. Later commands keep that phase.
The existing physical activity guard allows a fresh calibration after the
host clocks stop, so a host power cycle still works while the FPGA stays on.
Changing between physical PHI0 and the ONE//e virtual clock also allows a
fresh calibration. ONE//e keeps its existing frame-zero seed and calibration
reset on virtual CPU reset.

Run `python scripts/test_apple_timing_reset.py` for the reset regression.
It compiles the production PHI0/RES# sampler, activity detector, and timing
counter with the timing wiring and reset block extracted from `apple_top`.
It checks NTSC
and PAL timing through repeated resets, active-display and frame boundaries,
a reset held for more than one frame, reset with PHI0 stopped, and later ROM
commands. It also checks calibration after host clock loss, ONE//e reset
behavior, and return to physical PHI0.

## F1.0.4 validation history

The following results describe the earlier F1.0.4 build. The current
F1.0.6 release results follow below.

Simulation on 2026-09-06 passed 87,377 PHI0 cycles, including 37,661 cycles
with RES# low. The original reset-release code failed this regression; a
version that restored only the old calibration clearing also failed.
The boot-menu checks, PHI0 filter, standard detector, frame wrap, vTW video,
and ONE//e video/ROM boot tests passed.

The separate, unchanged ONE//e mode-safety bench emitted 11 failed checks
for selection and lockout despite its final PASS marker and zero exit code.
That suite does not count as a pass. Its log is
`build/onee_mode_safety_guard_sim/xsim.log`. The reset regression above
checks the activity detector used here with explicit fatal messages.

Both PL attempts (`20260906T145205Z-b3b54ecc-incremental` and
`20260906T150826Z-b3b54ecc-full`) finished at +0.128 ns setup slack,
+0.048 ns hold slack, and +0.265 ns pulse-width slack. The setup result
misses the required +0.200 ns margin, so the build refused hardware export.
The worst path runs from `physical_inh_dependent_q` to `a2fpga_dir_d` in
the unchanged Apple bus output logic. Reports remain under `.timing_runs/`.

At the user's request, Firmware F1.0.4 packages that exact full-run
bitstream. Its hardware description was exported without rerunning PL,
then the Vitis platform and both frontend CPU applications were rebuilt.
The original timing record keeps its failed +0.200 ns reserve result.
Boot B1.2.0 remains the boot-version label.

The image is `FIRMWARE.BIN` (4,316,652 bytes), with an archived copy and
`firmware_manifest.json` under `build/firmware-1.0.4/`. Its SHA-256 is
`15e869e65210b12c9e3016091ad637e66694468a339bb36757a448030e3c3668`.
The package audit checked the FSBL and frontend loadable bytes, the exact
PL configuration, the embedded CPU1 binary, version strings, recovery flag,
size limit, and payload CRC. The version and image-manifest source tests
also passed. The image has not been flashed.

Hardware check: calibrate once, then issue CTRL-RESET at several points in
the frame and compare the native VBL edge with the motherboard scanner.
The count should continue through reset without a phase step. No serial
ports were available during this change, so that physical check remains
pending.

## F1.0.6 release

F1.0.6 retains the scanner reset fix. The full build
`20260906T204205Z-b3b54ecc-full` passed the required +0.200 ns reserve:
nominal setup slack is +0.206 ns, hold slack +0.047 ns, and pulse-width
slack +0.265 ns. Routing and bus-skew checks passed, with no timing failures.
The final checks used nominal constraints after removing temporary margins.

The Vitis platform and both frontend CPU applications were rebuilt from
that build's XSA. The current `FIRMWARE.BIN` is F1.0.6, 4,290,220 bytes,
with SHA-256
`f0b64fd7ccdb7863951a6b02774e9a8837150e281df03e50f8a38b2da71a3e61`.
Its audit verified the bitstream/XSA hashes, embedded CPU1 image, version,
image manifest and partition layout. Evidence is in `build/f106/`.
The 79 Python regression launchers passed; 38 simulation logs cover reruns
after the final runtime edits. Physical hardware validation remains pending.
