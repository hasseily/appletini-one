# Disk II drive sounds

The Disk II sound player mixes a looping motor recording with seek,
track-zero recalibration, and door recordings. The samples in `Assets/Sounds`
are mono 16-bit PCM at 48 kHz. `scripts/build_disk2_sound_assets.py` generates
the sample table and the HDL offsets from those WAV files.

## Reset seek sound

The user reported rough buzzing before the track-zero sound on Apple resets
with firmware F1.1.0, and confirmed that an RTL audio render reproduced the
sound heard on the card.

The boot ROM moves the head by a half-track every 19,690 CPU cycles, about
19.25 ms at 1.023 MHz. The old player cut each seek recording by head position
and played only about 9 ms for that movement. It then silenced the seek voice
for about 10 ms before the next step. Those repeated gaps chopped the audio
about 52 times per second. The normal and virtual Disk II paths produced the
same event timing.

The player now starts each seek at the recording offset for the head position
and keeps the sample cursor moving during repeated steps in the same direction.
Each movement refreshes a 25 ms timeout, which spans the boot ROM's step
interval. The seek voice stops when that timeout expires. An isolated step
therefore plays 25 ms of the recording. A seek that outlasts the recording
wraps within that recording. The WAV files and playback pitch do not change.

A direction change selects the other seek recording at the new head position.
Track-zero and door events replace seek playback. Track-zero recalibration
keeps its existing protection against retriggering. Zero-distance or clamped
movements do not start or extend a seek sound.

Apple RESET preserves the head position, so a reset can start with inward
seeking. A PL reset or disabling the controller clears the head position. A
boot that starts at track zero has no inward seek sequence before recalibration.

## Validation

Run the focused source checks and player simulation:

```powershell
python scripts/test_disk2_standard.py
python scripts/test_disk2_sound.py
```

The simulation runner uses Vivado's `xvlog`, `xelab`, and `xsim` tools from
`PATH`. It checks PCM playback, repeated and isolated seeks, recording wrap,
event replacement, reset, and memory-response timing. Generated simulation
files go under `build/`.

The diagnostic render used the production boot ROM's event timing and the
embedded PCM. The fixed player removed all pre-recalibration gaps while
leaving idle and the track-zero recording identical to the old player.
All 55,190 rendered track-zero samples matched the source recording plus the
idle mix. The player now silences the event voice at the end of the recording,
instead of repeating its last sample for one extra tick. The source WAVs also
matched the generated sample table and the original F1.1.0 package.

The user's listening check confirmed the original fault. After trying the
F1.1.0 sound test firmware with +0.132 ns setup slack, the user reported that
the sound was much better. That check applies to the tested image; later
placement and routing changes need their own check on the card.

Three fresh placements of the same sound-fix synthesis produced setup/hold
slack of +0.158/+0.050 ns, -0.035/+0.047 ns, and -0.007/+0.048 ns. Only the
first passed all timing checks. One further post-route optimization on a copy
of that placement did not improve it. The selected placement improves on the
tested image's +0.132/+0.008 ns, but remains below the usual +0.200 ns setup
reserve. No HDL or nominal timing constraints changed during these trials.

The selected placement passed pixel-clock timing at +0.309/+0.121 ns and all
19 DVI output setup/hold checks at +0.872/+1.618 ns. Its video clock waveforms,
output delays, and clock groups match the tested image. A changed source name
in the CDC report came from another input of the same video-standard selector;
the full registered-input, LUT, and synchronizer connections matched exactly.
The existing video suite also passed 21 RTL benches and 398,595 policy checks.
These checks do not replace a display test on the card with the new placement.

The package is `firmwares/FIRMWARE_F1.1.0_DISK2_SOUND_PLACEMENT.BIN`, with a
matching `_AUDIT.json` and `.sha256`. The root and `firmwares/FIRMWARE.BIN`
copies contain the same bytes. The earlier named sound test images remain
available. The new placement has not been flashed or checked on the card.

The September 25 timing trial removes the saturating adder before volume
scaling. Idle PCM is divided by four and event PCM by two, so their sum fits
in 16 signed bits. Final volume saturation remains. Simulation passed 35,199
exact stereo sample checks, including signed extremes and clipping; the
worst routed path into this player improved from +0.213 to +0.419 ns. The
whole design reached +0.172 ns, below the +0.200 ns margin requirement, so
this trial has no test firmware or new hardware listening result. See
[`docs/FABRIC_TIMING_MARGIN_PLAN.md`](docs/FABRIC_TIMING_MARGIN_PLAN.md) for
the build record and subsequent trials.
