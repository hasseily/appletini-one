# Disk II drive sounds

The Disk II sound player mixes a looping motor recording with seek,
track-zero recalibration, and door recordings. The samples in `Assets/Sounds`
are mono 16-bit PCM at 48 kHz. `scripts/build_disk2_sound_assets.py` generates
the sample table and the HDL offsets from those WAV files.

## Reset seek sound

The player starts each seek at the recording offset for the head position
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
