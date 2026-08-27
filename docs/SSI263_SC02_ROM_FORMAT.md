# SSI-263 / SC-02 parameter ROM format

## Scope

This note records the format of the supplied SSI-263A parameter ROM and the
use of its low nibbles in the archived SC-02 prototype schematic. It separates
facts read from those sources from behavior in the current hybrid firmware.

The net and latch behavior below is exact for the saved prototype schematic
and its ROM. It does not prove that the final production SSI-263 die used the
same inner wiring. Use a real production-card capture to settle any difference.

The checked-in active table is
[`hdl/apple/ssi263_sc02_rom.mem`](../hdl/apple/ssi263_sc02_rom.mem). The source
image has these identities:

- CRC32: `CC0A72EE`
- SHA-256:
  `9C3BBA73319E1ED3652C85DAC19874DF04CBB72E62FDD63D6CBD7B34FF81F941`

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

## Current hybrid behavior

The current SSI-with-SC-01 backend reads only `rom_byte[7:4]` through
`ssi263_sc02_target()` in
[`hdl/apple/ssi263_formant_pkg.sv`](../hdl/apple/ssi263_formant_pkg.sv). It
does not yet execute any TPARM control path.

This keeps all native upper-nibble targets while retaining the SC-01 timing,
source, and route behavior. It also means the hybrid cannot reproduce
lower-nibble-only distinctions such as the prototype HF/HFC PW3 difference.
