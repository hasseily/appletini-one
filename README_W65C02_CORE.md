# W65C02 SystemVerilog Core

`hdl/apple/w65c02_core.sv` is a synthesizable, cycle-stepped WDC
W65C02S-compatible processor core. It implements the complete 256-entry opcode
map, including CMOS instructions, RMB/SMB, BBR/BBS, WAI, STP, and the
W65C02S reserved NOP encodings.

The core is currently a standalone verified module. It is not yet instantiated
by `apple_top.sv` or registered in `hdl/hdl_sources.txt`.

## Interface

One external bus cycle completes on a rising `clk` edge when both `enable` and
`ready` are high. Bus outputs remain stable during a stall.

- `addr`, `data_in`, `data_out`, and `rwb` are the 16-bit address and 8-bit data
  bus.
- `sync`, `vpb_n`, and `mlb_n` expose the W65C02S instruction-fetch, vector-pull,
  and memory-lock indications.
- `irq_n`, `nmi_n`, and `so_n` implement the active-low processor inputs.
- `waiting`, `stopped`, and `instruction_done` expose useful integration state.
- The read-only `debug_*` outputs expose architectural state.
- Set `DEBUG_STATE_LOAD=0` in hardware. The `debug_load` and `debug_*_in` ports
  exist only to initialize individual simulator vectors and synthesize away.

For Appletini integration, clock the core from an existing PL clock and use
`enable` as its CPU-cycle clock enable. That avoids adding a clock domain or a
new BUFG/MMCM. A dual-port block-RAM shadow is the natural memory attachment:
one port for the core and one for the existing Apple-bus/PS-side update path.

## Verification

The fast harness combines directed pin/control checks with the MIT-licensed
[SingleStepTests WDC 65C02 corpus](https://github.com/SingleStepTests/65x02).
The corpus remains outside this repository because it is approximately 1 GB.

```powershell
git clone --depth 1 https://github.com/SingleStepTests/65x02 C:\tmp\65x02-tests
python scripts\test_w65c02_core.py --opcodes all --limit 0
```

The full run at corpus revision
`2f6980a2d95757486c7bee24355c360e40e2a224` passed:

- 2,540,000 architectural, memory, and exact bus-cycle vectors.
- 254 populated opcode files; WAI (`CB`) and STP (`DB`) files are empty upstream.
- 266 directed checks covering reset, IRQ/NMI priority and edge timing, SO,
  RDY/enable stalls, VPB, MLB, WAI, and STP.
- Progress is printed every 250,000 vectors, and the testbench stops on the
  first mismatch.

Program-level verification uses the GPL-3.0
[Klaus Dormann suite](https://github.com/Klaus2m5/6502_65C02_functional_tests).
Its sources and binaries also remain outside this repository.

```powershell
git clone --depth 1 https://github.com/Klaus2m5/6502_65C02_functional_tests C:\tmp\klaus-65c02-tests
python scripts\test_w65c02_klaus.py
```

At revision `7954e2dbb49c469ea286070bf46cdd71aeb29e4b`, all configured suites passed:

| Suite | Cycles | Instructions |
| --- | ---: | ---: |
| Functional | 96,561,327 | 30,646,179 |
| 65C02 extended opcodes | 66,907,075 | 21,986,988 |
| CMOS decimal, including invalid BCD | 48,776,483 | 15,250,620 |
| IRQ/NMI feedback | 3,057 | 1,068 |

The Klaus harness prints a heartbeat every 10 million cycles and rejects any
trap message, missing pass marker, or cycle-limit timeout.

## Vivado Results

Run the standalone implementation estimate with:

```powershell
vivado -mode batch -source scripts\synth_w65c02_core.tcl
```

Vivado 2025.2 placed and fully routed the core for `xc7z020clg484-2` at
133.333 MHz:

| Resource | Core | Device | Device use |
| --- | ---: | ---: | ---: |
| Slice LUTs | 1,010 | 53,200 | 1.90% |
| Slice registers | 157 | 106,400 | 0.15% |
| Slices | 305 | 13,300 | 2.29% |
| CARRY4 | 48 | - | - |
| Block RAM tiles | 0 | 140 | 0% |
| DSPs | 0 | 220 | 0% |

At a 7.500 ns constraint, routed timing is WNS `+0.631 ns`, WHS `+0.234 ns`,
and WPWS `+3.250 ns`, with zero failing endpoints and zero routing errors.
This is an out-of-context estimate; Vivado warns that `HD.CLK_SRC` is unknown,
so only a fresh integrated implementation can provide top-level timing signoff.

The last existing Appletini implementation used 25,709 LUTs (48.33%), 16,178
registers (15.20%), 8,209 slices (61.72%), 36 block-RAM tiles (25.71%), and 6
DSPs (2.73%). A simple additive estimate with this core is 26,719 LUTs (50.22%)
and 16,335 registers (15.35%). Slice counts are not safely additive because
packing changes during integration; the naive sum is 8,514 slices (64.02%).

The existing design also has 104 free block-RAM tiles, 214 free DSPs, 23 free
BUFGs, two free MMCMs, and four free PLLs. A 172 KiB byte-wide shadow memory
would consume approximately 43 RAMB36 tiles, bringing total BRAM use to 79/140
(56.43%) and leaving 61 tiles. The core itself needs no BRAM, DSP, or new clock
primitive.

The last top-level route met timing, but its WNS was only `+0.081 ns`. Treat
placement and integrated timing as the limiting resources even though raw LUT,
register, and BRAM capacity remain comfortable.
