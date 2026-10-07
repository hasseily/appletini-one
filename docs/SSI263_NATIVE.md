# Native SSI-263 engine

The production Phasor path uses two independent native SSI-263 engines. The
source list is `hdl/hdl_sources.txt`. The Apple bus wrapper accepts SSI register
writes and preserves D7, IRQ, ACK, reset, and register aliases. Each socket
has its own audio and response state.

## Blocks

| Block | Job |
| --- | --- |
| `ssi263_parameter_rom` | Read the active 512-byte phone table |
| `ssi263_native_controller` | Scan ROM slots and update phone controls |
| `ssi263_native_pitch` | Set pitch and inflection |
| `ssi263_native_source` | Form voice and noise events |
| `ssi263_native_tract` | Run the five formant model |
| `ssi263_native_engine` | Schedule events and 48 kHz audio samples |
| `ssi263_response_timing` | Drive D7 and IRQ response timing |
| `ssi263_stereo_mixer` | Apply volume and independent pan |

All blocks use the fabric clock. `xck_ce` is a clock enable. The Phasor path
divides raw Q3 by two. The engine returns a sample with `audio_valid`; callers
must use that pulse rather than assume the sample is ready at `audio_tick`.
An event that misses its clock budget sets a fault. The ROM byte layout is in
[the ROM format](SSI263_SC02_ROM_FORMAT.md).

The host comparison data lives under `scripts/fixtures/ssi263_host/`. Its
formant table supports listening comparisons and native engine tests; Vivado
does not compile that table. See [the host renderer](SSI263_HOST_RENDERER.md)
for its commands.

## Checks

Run source and trace checks before a hardware build:

```powershell
python scripts/test_phasor_card.py
python scripts/test_ssi263_host_data.py
python scripts/test_ssi263_calibration_replay.py
python scripts/test_ssi263_calibration_replay.py --rtl
```

The focused `scripts/test_ssi263_native_*.py` checks cover the controller,
pitch, source, tract, engine, and bus integration. They need Verilator; on
Windows they use Ubuntu WSL. The RTL replay needs Vivado simulation tools.
Run a full Vivado and Vitis build, then check audio, D7, IRQ, reset, and both
sockets on the card after a production change.
