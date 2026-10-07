# SSI-263 host renderer

The host renderer turns SSI register traces into stereo WAV files. It is used
for listening checks and for comparisons with the native FPGA engine. It does
not run Vivado or replace a card test.

## Run

From the repository root:

```powershell
python scripts/render_ssi263.py --demo hello --engine all --compare-ff 231 --bundle
python scripts/render_ssi263.py --demo transitions --engine all --output build/ssi263_host/transitions
python scripts/render_ssi263_mb_audit.py
```

The output contains WAV files, the input trace, and a JSON report for each
model. Socket 0 is the left channel; socket 1 is the right channel. The
`hello` sequence is a hand-written register demo. `--bundle` also writes a
ZIP with a local listening page and rerender tool.

Python needs NumPy. A C++17 compiler is needed when the host C++ source
changes. The script uses `CXX`, `g++`, or `clang++`, then the bundled MinGW
compiler on this Windows machine.

The reference formant table and listening settings are under
`scripts/fixtures/ssi263_host/`. This data also supports the native engine
checks. The host model has several profiles for comparison; its analog
response and absolute output level have not been proven against production
SSI-263 silicon.

## Checks

```powershell
python scripts/test_ssi263_host_data.py
python scripts/test_ssi263_host_render.py
python scripts/test_ssi263_native_engine.py
```

The native engine test needs Verilator and checks FPGA PCM against the host
reference. See [the native engine](SSI263_NATIVE.md) for production wiring and
the full check list.
