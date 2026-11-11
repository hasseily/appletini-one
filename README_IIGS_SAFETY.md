# Apple IIgs safety profile

The default setup uses Appletini in **physical slot 7**, with slot 7 set to
**Your Card** in the IIgs Control Panel. Keep the physical slot-3 profile off
for this setup. SmartPort remains in logical slot 7 in both profiles.

Checkpoint 1 adds an opt-in setup for **physical slot 3**. Enable **IIgs
physical slot 3 (after restart)** in Appletini's settings, or set
`host.iigs.slot3=on` in its saved configuration. Restart Appletini to apply
the setting before it releases the Apple reset hold. In the IIgs Control
Panel, leave slot 3 on its internal 80-column setting and set each reserved
logical slot to **Your Card**. Leave those physical slots free of conflicting
cards. Appletini does not change the IIgs Control Panel settings.

The slot-3 setup has passed source checks, simulation, and a ROM boot-order
trace in GSSquared. The F1.3.7dev15 test package at
`build/gs-support-firmware-m350/FIRMWARE.BIN` passed the selected PL timing
gate and ARM/package audits. Board qualification remains pending. See the
[validation record](README_IIGS_VALIDATION.md).

## Boot and identity

With the slot-3 profile selected, Appletini qualifies physical bus accesses
with active-low `/M2SEL` from the start. A selected write to `$C02D` must map
slot 7 externally before the first C7 boot reply. The ROM window must also
permit external access. C8 and CA00 require a C7 claim, and boot commands
require the existing report session. The physical slot-3 `/DEVSEL` selects
`$C0B0–$C0BF`. It does not select virtual slot-7 I/O or ROM.

The default profile keeps its existing bootstrap behavior. It serves the
normal C7/C8 reads before it knows the host and starts `/M2SEL` qualification
when the boot ROM reports a IIgs. On a confirmed IIgs in the default profile,
C0F also requires live physical `/DEVSEL`. The earlier unqualified reads on
this path still carry a pre-ID contention risk on a IIgs.

The boot ROM calls `SEC / JSR $FE1F` before testing legacy ROM ID bytes and
displaying `A:APPLETINI`. It sends commands only through its report session.
The card opens that session on the served C700 entry, C8 claim, and
WINDOW_BEGIN command. The menu timeout does not close the report session.

- `$21–$24` report II/II+, IIe, enhanced IIe, or IIgs.
- `$26` starts the physical auxiliary-memory probe. `$30/$31` report its result.
- `$27` escapes the next byte as the raw IIgs `$C02D` mask.

The first native ID stays locked across Apple warm resets. Later reports
within the legacy family leave that ID intact. A legacy/IIgs conflict, an
unsupported report in the report session, or a legacy ID under the slot-3
profile faults the physical policy. ONE//e cannot supply a physical-host
report. The slot-3 setting does not establish the host's identity or enable
optional cards before the ROM confirms a IIgs.

## Live ownership and interrupts

C0n I/O, Cn ROM, and the shared C8 window have separate rules. Appletini
seeds the live `$C02D` map from the boot report if it has not observed a
qualified write. It then follows qualified writes to `$C02D`, `$C068`, and
the legacy switches. `$C02D` maps slots 1, 2, and 4 through 7. Slots 3 and 4
have permanent C0n windows on the IIgs. Slot 3's ROM follows `$C00A/$C00B`.
The global internal-ROM switch overrides Cn and C8 while leaving C0n
available. Appletini reserves its slot-3 connection for the bus and M2B0.

A selected external Cn access claims C8. An observed internal C3 access
keeps C8 internal until a selected CFFF access or reset. CFFF releases the
claim and receives no card data. Turning a slot internal or enabling the
global internal-ROM override revokes the external C8 claim. Software must
access Cn again after restoring the mapping. The card cannot see internal
C3 or CFFF cycles that run without `/M2SEL`. Measure how ROM 1 and ROM 3
expose those sequences before qualifying GS/OS slot switching.

Live permissions gate side effects, early PC Transporter prefetch, and
buffered read replies. A remap closes the affected windows without
resetting an enabled card's private state. PHI0 continues to advance video
timing, Mockingboard timers, and Disk II rotation during fast-memory cycles.
Mapped and configured Mousecard, Mockingboard/Phasor, PC Transporter, and
SuperSprite can request IRQ in the slot-3 profile. Their physical IRQ
permission depends on live ROM ownership and the selected device mode.
An unrelated or fast-memory access does not release a pending IRQ. The
default physical-slot-7 GS profile keeps physical IRQ off.

| Logical slot | Slot-3 profile service | IIgs service displaced by external ROM |
| --- | --- | --- |
| 1 | Uthernet II and SSC printer interface in separate I/O windows | Printer port |
| 2 | Mousecard, 4Play, or SNES MAX | Modem port |
| 4 | Mockingboard or Phasor | ROM 1 native mouse firmware |
| 5 | Z80 Appli-Card or PC Transporter with private memory | Internal 3.5-inch disk service |
| 6 | Disk II | Internal 5.25-inch disk service |
| 7 | Boot menu and SmartPort, or SuperSprite after handoff | AppleTalk |

The no-slot clock is available only under its enabled Appletini ROM windows.
Leave slot 4 internal to retain native mouse support on ROM 1. ROM 3 removes
that firmware dependency. The virtual Mousecard does not replace the ADB
mouse automatically. [Apple Technical Note 26](https://apple2.gs/technotes/tn-iigs-026/)

Before identification in the default setup, the separate LINTXT path may
serve C0F with active `/M2SEL` and physical `/DEVSEL` when SuperSprite is off.
It grants no SmartPort ROM/FIFO access or boot commands.

## Physical output policy

Both GS profiles block `/INH`, `/DMA`, address/RW drive, host-memory
replacement, RamWorks, vTW/TURBO, AD8088 host-memory access, and PS host-memory
DMA. RDY and `/NMI` remain released. The dedicated open-collector reset
path retains the boot hold and reset behavior. Only a supported legacy ROM
report can grant legacy host-memory rights. The PS cannot grant them.

The physical arbiter discards prohibited ownership tuples. The wrapper
checks buffered replies against the live physical policy and applies
reset, fault, and ONE//e isolation at the pad boundary. The private ONE//e
arbiter stays separate.

The slot-3 profile now includes native M2B0 capture and ordered video replay
in source. It requires confirmed GS identity and observed scanner phase.
The 704x232 output carries text colors, native SHR, and captured borders.
Source tests do not qualify the physical sampling, exact raster timing,
C029 bit-6 address behavior, or GS alternate language fonts. See the
[capture contract](README_IIGS_CAPTURE_PROTOCOL.md) and
[validation record](README_IIGS_VALIDATION.md) before using a GS test image.

## Card-control registers

Addresses below use the default `APPLE_DEBUG_BASE` of `0x40000000`.

| Word index | Address | Access | Fields |
| --- | --- | --- | --- |
| `0xB6` | `0x400002D8` | Read | Bits 7:0 hold the live `$C02D` mask with reserved bits clear. Bit 8 marks the mask valid. Bits 23:16 hold the profile's live C0n mask. Bits 31:24 hold its live Cn mask. Bits 15:9 read zero. |
| `0xB7` | `0x400002DC` | Read and set | Bit 0 sets the slot-3 profile during reset hold, physical reset, or isolation and stays set until PL reset. Bit 1 reports the combined identity/slot-policy fault. Bits 31:2 read zero. |

The C0n and Cn masks describe host ownership within the profile's slot
range, independent of requested card enables. The default GS profile
exposes bit 7 only in those two masks. The slot-3 profile exposes supported
virtual slots 1, 2, and 4–7. Ignore the masks until bit 8 is set. Outside
active GS ownership, `0xB6` reads zero.

Apple documents the slot map in the [IIgs Hardware Reference, chapter 8](https://downloads.reactivemicro.com/Apple%20II%20Items/Documentation/Manuals/IIgs/IIgs%20Hardware%20Reference.pdf).
See [IIgs Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/) for
`/M2SEL`, M2B0, and IRQ behavior.
