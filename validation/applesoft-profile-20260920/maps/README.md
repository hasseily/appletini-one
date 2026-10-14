# Applesoft profile address map

`applesoft_enhanced_map.json` assigns every 16-bit PC to exactly one half-open
interval. It also lists named entry points for instruction-entry counters and
the RUN, prompt, keyboard, and random-seed landmarks used by a harness.
`applesoft_enhanced_intervals.tsv` is the same interval table in a readable form.

Regenerate with Python 3, from any working directory:

```powershell
python C:/Users/hasse/source/repos/hasseily/appletini-one/validation/applesoft-profile-20260920/maps/build_applesoft_map.py
```

The generator uses the standard library and writes only in this folder. It
checks SHA-256 for both inputs before generating the map. The bundled 16 KiB
`docs/Apple2e_Enhanced.rom` has SHA-256
`aab38a03ca8deabbb2f868733148c2efd6f655a59cd9c5d058ef3e0b7aa86a1a`.

The saved source is the [Applesoft disassembly](https://6502disassembly.com/a2-rom/Applesoft.html),
derived from [Bob Sander-Cederlof's original listing](https://www.txbobsc.com/scsc/scdocumentor/).
The listing describes the II+ ROM. The generator compares every displayed byte
against this repository's Enhanced IIe ROM: 9,745 of 9,924 bytes match; 179 differ.
Truncated data directives leave some bytes unavailable for this comparison.

The Enhanced ROM reuses `$F775-$F7D8` for a storage firmware shim, tokenizer
case folding, and text-position helpers. The map uses the actual Enhanced
code and call sites there. It does not reuse the II+ cassette labels for those
addresses. The entry signatures always come from the exact bundled ROM.

The categories measure **exclusive PC time**. For example, a numeric-input
routine calling floating-point addition contributes the addition's cycles to
`arithmetic`. Entry counts do not measure inclusive call duration and must not
be added to the category totals. Count entries at executed instruction
boundaries; a stalled PC must not count as repeated calls.

Classify language-card RAM or other banked code as `DOS/user` when it replaces
a ROM region. The page-zero CHRGET interval requires a stock-code check; mask
`$B8/$B9`, which hold the changing text pointer. Its signatures include the mask.
The `$C000-$CFFF` and monitor regions remain broad: monitor low-resolution
graphics and screen output both count as `monitor/IO`. Shared block moves count
as `variable/array lookup`, and the array-offset multiplier stays in that
category. Other shared floating-point helpers count as `arithmetic`.

`harness_landmarks` records verified local addresses. `RUN` starts at `$D912`;
the BASIC prompt before input is `$D43F`. `$FD0C` eventually jumps through the
keyboard vector at `$0038/$0039`, so the harness must take a return address from
the current stack rather than assume one fixed return PC. The random seed is
the five-byte range `$C9-$CD`. Cold start fails to initialize its last byte;
snapshot or set all five bytes for repeatable runs.

These maps do not measure performance, prove complete routine compatibility,
or validate any proposed acceleration. The JSON lists the remaining coarse
boundaries and mapping limits.
