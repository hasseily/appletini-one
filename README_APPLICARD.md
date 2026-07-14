# PCPI Appli-Card

Appletini One provides a PCPI Appli-Card compatible Z80 coprocessor in Apple
slot 5. The Apple-visible latch protocol is implemented in PL, while the Z80
and its private memory run on the Zynq PS.

## Compatibility

- Two byte latches and two handshake flags match the PCPI card protocol.
- The embedded 2 KB v9 boot ROM is CRC-checked at firmware startup.
- Z80 memory contains 32 banks of 64 KB, for 2 MB total.
- Bank-register bits 1-3 retain GZ/80S behavior; bits 4-5 select the additional banks.
- The upper 32 KB common area maps to bank 0 when bank-register bit 6 is set.
- The Z80 interpreter supports the documented and undocumented instruction behavior
  exercised by ZEXALL.

The card has no slot ROM and no shared Apple/Z80 memory. Both processors
communicate entirely through the latch protocol, so PS execution latency does
not change Apple-visible timing.

## Apple Interface

Slot 5 uses `$C0D0-$C0DF`:

| Address | Read | Write |
| --- | --- | --- |
| `$C0D0` | Read Z80-to-6502 byte and clear its pending flag | Ignored |
| `$C0D1` | Read back the 6502-to-Z80 latch | Write a byte and set its pending flag |
| `$C0D2` | Bit 7 reports a byte pending for the Z80 | Ignored |
| `$C0D3` | Bit 7 reports a byte pending for the 6502 | Ignored |
| `$C0D4` | `$FF` | Ignored |
| `$C0D5` | Reset the Z80 and return `$FF` | Reset the Z80 |
| `$C0D6` | `$FF` | Ignored; the physical card's CTC socket is unpopulated |
| `$C0D7` | Pulse Z80 NMI and return `$FF` | Pulse Z80 NMI |
| `$C0D8-$C0DF` | `$FF` | Ignored |

Reads of `$C0D5` and `$C0D7` perform the same side effect as writes. Reset
clears both latches and flags in PL before the PS restarts the Z80.

## Z80 Interface

The I/O decoder mirrors each function across a 32-port range:

| Ports | Behavior |
| --- | --- |
| `$00-$1F` | Read back or write the Z80-to-6502 latch |
| `$20-$3F` | Read and consume the 6502-to-Z80 latch |
| `$40-$5F` | Read handshake flags |
| `$60-$7F` | Map or unmap the boot ROM |
| `$80-$BF` | Unpopulated/reserved, reads return `$FF` |
| `$C0-$DF` | Write the bank/common-area register |
| `$E0-$FF` | Ignored, reads return `$FF` |

The PL register bridge is AxiSimple slave 7 at `0x40070000`. A sequence number
is attached to each Apple-to-Z80 byte so a delayed PS acknowledgement cannot
consume a later byte.

## Firmware Controls

The **Z80 Applicard** configuration tab enables slot 5 and selects Standard or
Maximum CPU share. The settings are stored as:

```text
applicard.slot5.enabled=on|off
applicard.resource.max=on|off
```

UART commands:

```text
z80 status
z80 on|off
z80 reset
z80 budget <tstates>
z80 wall <microseconds>
z80 dump <hex-address> [length]
```

Physical slot 5 must be empty whenever the virtual card is enabled.

## Demo Media

`software/applicard/DEMOBOOT.DO` boots PCPI CP/M 2.2 through the virtual Disk II
in slot 6. `DEMOGAME.DO` contains the bundled game data disk. The directory also
contains banking and CPU-validation programs plus source media for
compatibility testing.

The embedded ROM is generated from `software/applicard/APPLICARD.ROM` by
`scripts/gen_applicard_rom_c.py`; runtime firmware never loads an SD-card ROM
override.

Run `python scripts/test_applicard_card.py` for the protocol, banking, wiring,
configuration, and service integration checks. The PL testbench is
`hdl/sim/tb_applicard_card.sv`.
