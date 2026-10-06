# House of the Rising Sun listening preview

## Physical comparison and balance trial

On 2026-10-07 the user accepted the balance trial as our listening reference.
Use `--reference balanced` with the song renderer to reproduce that balance;
see [the pinned settings and commands](SSI263_HOST_RENDERER.md#accepted-balanced-reference-2026-10-07).
Further experiments must keep this reference available for comparison.

The tester's physical `risingsun.mp3` is now available. The comparison at
`build/ssi263_host/song_hardware_20261006/listen.html` has the full real song,
the frozen native render and a separate voice/noise-balance trial, plus eight
matched excerpts and model stems. All clips keep their original levels and
sample rates. One instrumental time map selects the hardware excerpts; there
is no per-attack alignment or gain fit.

The trial keeps ideal voiced level and the AY backing while reducing noise
relative to voice by 18.06 dB. It changes no envelope, pitch, ROM or filter.
Calibration measurements improve markedly for S and voiced TH, but some
fricatives become slightly quiet and spectral differences remain. Firmware and
default model parameters stay unchanged. See the [attack review](SSI263_ATTACK_AUDIT.md)
for the source-gain history, prototype status, measurements and limits.

## Render the original checkpoint

`scripts/render_ssi263_song.py` combines the current native SSI host model with
the song's original four-chip AY RTL accompaniment. It leaves the song score
and the source repository unchanged. No Vivado build or hardware is required.

The song lives in the `music/house_of_the_rising_sun` directory of
`appletini-software`, on `codex/physical-phasor-ssi`. The current preview reads
the existing `appletini-software-ssi-calibration` worktree at commit
`85bd75cf9604ad9ebef688fb42890e21bbfd2478`.

If that worktree is not present, create a separate checkout of the source
snapshot without switching the main software checkout:

```powershell
git -C ../appletini-software worktree add --detach ../appletini-software-song-preview 85bd75cf9604ad9ebef688fb42890e21bbfd2478
python scripts/render_ssi263_song.py --song ../appletini-software-song-preview/music/house_of_the_rising_sun
```

From this repository, run:

```powershell
python scripts/render_ssi263_song.py --song ../appletini-software-ssi-calibration/music/house_of_the_rising_sun
```

On Windows, the AY renderer uses Python 3, Verilator and a C++ compiler in the
`Ubuntu` WSL distribution. Use `--wsl-distro` to select another distribution.
The native SSI renderer uses the normal local C++ build. Python needs NumPy on
the host; WSL does not need NumPy. `ffmpeg` supplies the MP3 encoder.

The output directory is `build/ssi263_host/house_rising_sun`. Open `listen.html`
for the full MP3, stereo vocal stem and AY backing. The directory also contains
the full 48 kHz stereo WAV, source score, compiled PHS1 stream, SSI register
trace and reports with source and audio hashes.

## Signal and timing choices

The preview uses the song framework's `physical-ssi263` compiler profile and
PAL clock by default. This retains its composition, phonemes, musical timing,
amplitudes and AY arrangement. It translates the authored pitch and filter
controls into physical SSI register values. In particular, this song uses
FF 229–231 rather than the old Appletini preview's near-128 values. Those bytes
would select a much lower filter clock in the native model. `--region ntsc`
selects the other regional clock and tuning.

Both SSI sockets receive the original duplicated vocal. All register writes
retain their order and nominal 100 Hz score timestamps, with less than one
effective XCK tick of rounding. The render does not add the Apple II player's
bus execution time. It checks the compiled PHS1 stream by decoding it before
rendering.

The backing runs the original song framework's Verilator AY driver, with its
AY8913 volume mode, default channel pan and mixer scaling. The script reads
and verifies the AY and mixer sources from the framework's pinned firmware
commit. It does not run the old speech renderer or a ROM generator.

The final mix adds the current native SSI output at its existing fixed gain
to the original AY mix. No normalization, EQ or attack adjustment is applied.
The script checks the stem lengths and refuses to export a clipped mix.

This is a model listening preview, not a recreation of the card's analog
output. Voice-to-AY balance is not yet calibrated against a physical Phasor.
The native model's known prototype conflicts, cold-start assumption and
provisional envelope timing still apply; see `SSI263_HOST_RENDERER.md`.
Firmware remains **F1.2.5-d1**.

The checkpoint render is 102.32 seconds (4,911,360 stereo frames). It preserves
3,598 SSI writes and 5,794 AY writes. The mixed PCM peak is 16,904, with no
clipped samples. The MP3 uses the same high-quality VBR encoder setting as the
original song preview.
