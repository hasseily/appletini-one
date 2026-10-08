# SSI-263 parameter ROM format

## Scope

This note records the format of the supplied SSI-263A parameter ROM and the
use of its low nibbles in the archived SC-02 prototype schematic. The active
table now uses target codes read from an SSI-263P die. It keeps the original
low control nibbles because the die read does not establish their byte wiring.

The net and latch behavior below is exact for the saved prototype schematic
and its ROM. It does not prove that the final production SSI-263 die used the
same inner wiring. Use a real production-card capture to settle any difference.

The checked-in active table is
[`hdl/apple/ssi263_sc02_rom.mem`](../hdl/apple/ssi263_sc02_rom.mem). The low
control bits came from a previously supplied `ssi263a.bin` when commit
`9ddcdc3` first added this table. That source file is not in this repo, so
the recorded hashes identify it but do not let this repo check its origin.
The 2 KiB source image had these identities:

- CRC32: `CC0A72EE`
- SHA-256:
  `9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941`

## Die target update for F1.2.5-d2

The active table's six upper-nibble fields use the phoneme bits from
[Casso's SSI-263P die extraction](https://github.com/relmer/Casso/tree/87704604d38531ff0ac2ddefdb0b1779c0b1982f/specs/024-mockingboard-speech/rom-extraction).
The exact 64-row raw bit matrix is saved as
[`ssi263p_die_bits.csv`](../scripts/fixtures/ssi263_native/ssi263p_die_bits.csv).
The independent [die review](https://github.com/tgeczy/ssi263-speech/blob/0cc92b3452ea6f1392b45f6036c780343c8f30a7/docs/die.md)
reports agreement on all 1,856 visible bits. F1 uses the bottom PAR row as
its low bit; that field reading remains under study.

Selectors 0, 1, 2, 3, 5, and 6 now hold die-decoded F1, F2, nasal, F3,
voice amplitude, and noise amplitude targets. The first four fields already
agreed across all 64 phones. The update changes 16 amplitude bytes: L1, Z,
J, SCH, V, F, THV, TH, M, N, NG, and LB have at least one changed target.
Selectors 4 and 7 stay zero. Every low nibble remains byte-for-byte as before;
its SHA-256 when stored as one low-nibble value per byte is
`ec766017bd444b275405bfb2825083e58e59ea1113328b5ba1f881ad0760118d`.

The new active 512-byte table has SHA-256
`ea494f047de11c533cb36a51d8686949206cedb91ab4855bdf9bde9500f828d9`.
Padding it with 1,536 zero bytes gives SHA-256
`849baa20baae3d756f26813cf4e4f47392573e735cb4c66afdc434f9932147e0`;
that padded table is not the original source image. Run
`python scripts/test_ssi263_die_rom.py` to check all raw bits, target fields,
control nibbles, and the host table mirror.

## Address and byte layout

The ROM address is:

```text
{phone[5:0], selector[2:0]}
```

This gives 64 phone rows with eight bytes per row. The first 512 bytes of the
2 KiB source contain the active table. Source offsets `0x200` through `0x7ff`
are zero.

Each byte has two separate fields:

```text
bit 7                                      bit 0
+-------------------+-------------------------+
| target code [7:4] | TPARM3..TPARM0 [3:0]   |
+-------------------+-------------------------+
```

The upper nibble supplies a filter or amplitude target. The lower nibble does
not extend that target. Its pins are four control lines named `TPARM3..0`. The
scan logic gives those pins a different use in each selector slot.

| Selector | Upper-nibble path | Low-nibble use |
|---:|---|---|
| 0 | F1 | TPARM0 chooses when PW0 sets |
| 1 | F2 | TPARM0 chooses when PW1 sets |
| 2 | F2 resonance | TPARM1, TPARM2, and TPARM3 control held source and transition state |
| 3 | Shared F3/F4 | None; low nibble is zero |
| 4 | Filter amplitude path; host amplitude supplies the target | None; ROM byte is zero |
| 5 | Voice amplitude | None; low nibble is zero |
| 6 | Fricative amplitude | None; low nibble is zero |
| 7 | No parameter write | None; ROM byte is zero |

## TPARM0: PW0 and PW1 start times

TPARM0 is used in selector slots 0 and 1. It is not stored as PW0 or PW1 data.
It selects the U38 comparison point for the low four bits of duration counter
U37:

```text
U38_equal = U37.low == (TPARM0 ? 2 : 6)
PW0 set    = U38_equal AND WR_SEL0
PW1 set    = U38_equal AND WR_SEL1
```

A phone write clears PW0 and PW1. Once set, each latch holds for the rest of
the phone.

- Selector-0 TPARM0 selects whether PW0 sets at nominal duration phase 2/16
  or 6/16. PW0 then permits selector-5 voice-amplitude transition steps on
  rate edges.
- Selector-1 TPARM0 makes the same choice for PW1. PW1 permits selector-6
  fricative-amplitude steps, qualifies the selector-2 PW3 load, and helps
  qualify the TPARM3 route update.

Thus TPARM0 controls when later amplitude and source changes may begin. It is
not a direct voiced, fricative, or stop flag.

## TPARM1: held PW3 source and envelope control

Selector-2 TPARM1 reaches the held PW3 latch. The schematic gives this rule:

```text
PW3 loads (latched_CTRL OR NOT TPARM1)
    only when PW1 AND WR_SEL2; otherwise PW3 holds
```

The two TPARM1 values therefore mean:

- `TPARM1=0`: force PW3 high at the qualified load.
- `TPARM1=1`: make PW3 follow the latched CTRL state.

PW3 feeds the sheet-6 source and envelope logic through:

```text
U104C = PW3 AND U62./Q
AMPCT0 = NOT U104C
FRICATIVE = NOT(D3+4 OR U104C)
             AND (U62./Q OR VOICE_AMPLITUDE_ZERO)
```

U104C affects the U68 amplitude-counter direction and terminal behavior, the
U85 reset path for U62, the gated noise clock, and the fricative source term.
TPARM1 therefore changes held source and envelope timing. It is not a simple
voice/noise selection bit.

## TPARM2: transition permit and route-update permit

Selector-2 TPARM2 loads two held states with opposite polarity:

```text
PW2 = TPARM2
PW5 = NOT TPARM2
```

PW5 feeds the U32B transition-write gate:

```text
U32B = NOT(PW5 AND (TPHO5 OR voice_amplitude_nonzero
                         OR fricative_amplitude_nonzero))
```

U32B qualifies transition writes for F1, F2, and the shared F3/F4 value. With
TPARM2 set, PW5 is clear and cannot block those writes. With TPARM2 clear, PW5
may block them under the shown phone and amplitude conditions. The normal
transition timing pulse still applies in either case.

PW2 also participates in the gated clock that lets U20B accept TPARM3. It has
no direct source-mute role in the drawn circuit.

## TPARM3: held fricative route request

Selector-2 TPARM3 is the data input to U20B. It reaches U20B only when the
drawn PW0, PW1, PW2, amplitude-counter-zero, and
fricative-amplitude-zero conditions permit the gated WR_SEL2 edge. Otherwise
U20B keeps its prior state.

At the selector-2 edge, the settled gate is:

```text
U20_clock = PW1
            AND (PW2_old OR TPARM2_current)
            AND ((PW0 AND PW1 AND AMPCT_ZERO)
                 OR FRICATIVE_AMPLITUDE_ZERO)
```

Two later latches turn U20B into separate, phase-held route switches:

- U112 passes U20B during Phi1 to `FRIC1_SW`.
- U166A samples `NOT U20B` on the positive Phi0 edge for `FRIC2_SW`.

The two switches do not change as a live bit and complement. After both latches
settle, TPARM3 selects these paths:

- `TPARM3=1`: FRIC1 enabled and FRIC2 disabled.
- `TPARM3=0`: FRIC1 disabled and FRIC2 enabled.

The prototype signal order is:

```text
VOICE -> F1 -> F2(+FRIC1) -> F3 -> F4 -> F5(+FRIC2) -> output
```

FRIC1 enters at the F2 output, before F3, and receives the later formant
shaping. FRIC2 enters after F5 and bypasses F3 and F4. Their exact level still
needs a real-card capture.

## Values present in the ROM

The active 64-row table uses the low nibble as follows:

| Selector | Low-nibble values | Set-bit counts |
|---:|---|---|
| 0 | `0`, `1` | TPARM0 is set for 9 of 64 phones |
| 1 | `0`, `1` | TPARM0 is set for 50 of 64 phones |
| 2 | `4`, `6`, `8`, `A`, `C`, `E` | TPARM1/2/3 are set for 57/58/59 phones; TPARM0 is always clear |
| 3-7 | `0` | No TPARM bits are set |

The HF/HFC rows show why the lower nibble matters:

```text
$2C: 71 90 0A C0 00 00 80 00
$2D: 71 90 08 C0 00 00 80 00
```

Their upper target nibbles are identical. Their only difference is selector-2
TPARM1:

- `A = 1010`: TPARM1 is set, so PW3 follows CTRL at the qualified load.
- `8 = 1000`: TPARM1 is clear, so PW3 is forced high.

The prototype therefore distinguishes these two phones through source and
envelope control, not through their formant or amplitude targets.

## Current implementation

The production `ssi263_parameter_rom` reads the active 512-byte table.
`ssi263_native_controller` uses the upper targets and lower control bits.
The native source and tract modules then form audio. The Apple bus wrapper
keeps D7 and IRQ response timing in a separate block. See
[the native engine](SSI263_NATIVE.md) for the active source list and checks.

The archived prototype helps explain the ROM fields, but it does not prove
the final SSI-263 die used the same internal wiring.
