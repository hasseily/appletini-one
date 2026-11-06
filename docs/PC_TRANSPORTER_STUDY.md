# Virtual PC Transporter in slot 5

Design study for Appletini One, based on source revision `a5a0669`. Updated 2026-11-06 with the [full source and software review](PC_TRANSPORTER_REVIEW.md). MAME references use commit `ce12c0474a7f5786748327d0320179344bddffba`; they describe that implementation, not a verified hardware specification.

## Recommendation

Add the Applied Engineering PC Transporter as a third, mutually exclusive slot 5 choice alongside the PCPI Appli-Card and ALF AD8088 Plus. Use the AD8088 as the starting point for evaluating an ARM-hosted x86 interpreter. Give the Transporter its own FPGA slot interface and its own PC machine model. The original card used a **NEC V30**, a 16-bit 8086-compatible CPU. The manual gives about 7.14 MHz. MAME uses 7.16 MHz, half the PC 14.318 MHz clock, and divides it by 6 for the standard 1.193 MHz timer clock. Its closest relative in Appletini is the AD8088. [Applied Engineering user manual, pp. 72–76](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=91) · [Appletini AD8088](../README_AD8088.md)

The compatibility target is the original `AEPC.SYSTEM` and Portware running on the Apple. A DOS boot is one milestone. The test that establishes Transporter compatibility is DOS using an Apple resource through that software, such as booting from a DOS hard disk file on ProDOS and copying a file across with `TRANSFER.EXE`. The two early gates are a physical Apple read from the shared RAM window within the bus deadline and a guest I/O operation that the Apple services and completes. [Original system software, pp. 81–83](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=100) · [File transfer instructions](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=199)

### Evidence and limits

“Confirmed” below means confirmed within the named source or inspected binary. This study records no completed Transporter emulator boot, original-card trace, or Appletini physical timing test.

| Evidence label | What it establishes | What it does not establish |
| --- | --- | --- |
| **Documented** | Behavior described by AE, the CPU maker, or the card designers. | That every shipped card or software revision behaves that way. |
| **MAME source** | Behavior implemented in the pinned source. Header comments are identified separately where they disagree with code. | Original-card behavior or a successful emulator run. |
| **Software static** | Bytes and instruction sequences found in the identified original files. | That a path executes in a given session, or the hardware response to it. |
| **Inference** | A proposed function supported by source or software patterns. | A complete register definition. |
| **Appletini source / calculation** | Existing code, configured interfaces, and arithmetic from clocks and timing taps. | A measured service bound or connector timing margin. |
| **Proposed / open** | Design choices and the evidence needed to accept them. | Completed implementation or validation. |

The static review used the [archived 2.0.5 system disk](https://www.cvxmelody.net/PC_Transporter_System_Disk_2.0.5_800K.zip). Keep these identities with future traces:

| Input | Bytes | SHA-256 |
| --- | ---: | --- |
| Archive ZIP | — | `b0da1f3ea4b38709ee04351e0de93f499a4d6997b383e03cad1f704f61a40403` |
| ProDOS-order disk image | 819200 | `96f682de1f49d89279fb5ea2def1c89298eb7f72efa2b41e483c2c7beb81619d` |
| `AEPCBIOS` | 8192 | `b2ac846f17f5a16ca80c80b7fb608746c72e8a2c2918dcf10ffa51f890d1a886` |
| `AEPC.SYSTEM` | 33850 | `6985d0ce1cc828886615b44532d632bbef0d73eac0072e18d33c3393acd9410f` |
| `AEPC.DISKS` | 7168 | `9efab678d448509cfbbff6921b77f1a82c2f4219845305f8b890f629c7cb3bfc` |

Apple code addresses below assume load bases `$2000` for `AEPC.SYSTEM` and `$1400` for `AEPC.DISKS`. File offsets identify the evidence if a loader relocates code. The review retains the detailed disassembly findings.

## What the original card did

The Apple booted ProDOS from another device and launched `AEPC.SYSTEM`, which contains the Control Panel and Portware. The Apple code configured the card and loaded `AEPCBIOS` into card RAM before releasing the V30. The card had no Apple slot boot ROM. `PCINSTALL` could prepend drivers from `AEPC.DISKS` to ProDOS so Apple software could use the card’s RAM disk and supported drives. The original system files also include the saved configuration and loadable device drivers. [System files, pp. 93–96](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=112) · [ProDOS disk drivers, pp. 140–142](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=159)

The card had 384–768 KiB of RAM accessible by both processors, with 256–640 KiB available to DOS applications depending on the configuration. It also had 16 KiB of CGA display RAM, an XT keyboard interface, a timer and speaker path, a TransDrive MFM floppy controller for 360 KiB and 720 KiB PC disks, and a socket for an optional 8087. MAME identifies the floppy controller as an Apple ISM, the MFM half of the later SWIM, on the Apple side of the card at `$CF20–$CF27`. CGA supported 40- and 80-column text and 320×200 and 640×200 graphics. The //e keyboard option used an adapter on the motherboard keyboard connector; MAME shows its modifier-key state at `$CF36`. The IIgs RGB option used a ColorSwitch cable. Appletini can model the PC-visible devices and use its own DVI, USB and audio outputs. [Card hardware, pp. 74–76](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=93) · [//e keyboard adapter](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=53) · [IIgs ColorSwitch](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=61)

### Portware routing: documented design and model differences

**Documented:** Portware runs on the Apple and services guest I/O. The patent places data at guest `$BF000–$BF3FF` and controls at `$BF400–$BF7FF`. It latches the port address and byte lanes and selects separate wait and notification bits by direction:

| Transfer | Wait bit(s) | Notification bit(s) |
| --- | --- | --- |
| Even byte write | D0 | D1 |
| Odd byte write | D4 | D5 |
| Even byte read | D2 | D3 |
| Odd byte read | D6 | D7 |
| Word bus cycle | Combines the two wait bits; the patent writes `D0 + D4` and `D2 + D6` without defining the operator | Combines the two notification bits, written `D1 + D5` and `D3 + D7` |

Neither bit set allows access without notification or waiting. Wait alone waits for prior pending service. Notification alone requests service without waiting for completion. Both request service and wait. This is the documented design; confirm shipped-card behavior. [US4888680A, Figures 3–6 and state tables](https://patents.google.com/patent/US4888680A/en).

**MAME source:** `port_routing()` fetches `RAM[$BF400 + (port & ~1)]`, then selects the low nibble for an even port or the high nibble for an odd port. Entries start at `$BF400`, `$BF402`, `$BF404`, and so on: a two-byte stride, not `port / 2`. The handler treats nibble zero as untrapped. For reads, an even nonzero nibble returns the latched byte and stalls; an odd nibble stalls and retries after service. Any nonzero nibble traps a write. Word accesses consult the starting port's nibble. These rules approximate, but do not implement, the direction and lane table above. For example, nibble `$1` traps a read in MAME although its documented read controls are zero. [Routing and I/O handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L1156).

**Open trace:** capture the controls written by the original loader and compare reads, writes and word lanes. Verify whether Apple `$C800–$CBFF` and guest `$BF000–$BF3FF` expose the same storage. Resolve these differences before fixing the FPGA routing contract; MAME's odd/even-nibble rule is not that contract.

### Storage and operating modes

| Path | Original behavior | Proposed coverage |
| --- | --- | --- |
| DOS hard disk on ProDOS | Portware uses a file on a ProDOS volume as a DOS hard disk. A bootable file can hold the DOS system and applications, while the Apple starts `AEPC.SYSTEM` from the surrounding ProDOS volume. | First end-to-end storage target. Use a disposable copy of a ProDOS image on the normal Apple storage path and let the original software service the guest requests. Verify persistence after a restart. [Manual, pp. 147–155](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=166) |
| ProDOS RAM disk | With the drivers installed, `/RAMAEPC` uses card RAM. In slot 5 it appears as S5,D1. Starting the PC with the reboot action reuses that memory and erases the RAM disk. | Separate acceptance test with the supplied ProDOS drivers. Verify read/write and the documented change in ownership when starting the PC. [Manual, pp. 140–145](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=159) |
| Drives on the Transporter | The card’s controller supports 360 KiB and 720 KiB PC floppy formats. The manual also describes ProDOS file structures on MFM media formatted by a Transporter drive. AE’s patched ProDOS can put the largest drive at S5,D2 and additional drives at S2,D1/D2. A real slot 2 block device takes precedence. | Follow-on work. First model PC floppy images and controller behavior, then validate the ProDOS media format and unit mapping. [Manual, pp. 127–146](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=146) |
| Drives on an Apple controller | Portware can present a configured Apple drive as a DOS floppy. The manual describes DOS file structures on Apple GCR media for this path. | Follow-on Portware test using the existing Apple controller and matching image format. [Manual, pp. 135–139](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=154) |

The Control Panel distinguishes resuming DOS, rebooting DOS, and quitting to ProDOS. Resume preserves the guest session. Reboot initializes the guest again. Quit discards the guest session and returns control to ProDOS. Treat those actions separately from an Apple warm Control-Reset, a cold reset, and a change of virtual card. [Control Panel actions, pp. 86–88](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=105)

Other stock integration tests include `APLCLOCK` reading the ProDOS clock, `LAUNCHER.EXE` starting a DOS program selected in ProDOS, and the DOS mouse driver working with the Apple-side driver. AppleWorks can also use the Transporter RAM disk as expanded desktop memory. These are separate from booting a DOS system or detecting a RAM disk. [DOS utilities and AppleWorks, Appendices C–D](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=198)

## Working register map

**MAME source:** the BSD-3-Clause card implementation provides a starting map. Its Apple //e slot list includes `pcxport`. It implements BIOS loading, pointer and page windows, parts of the control and IRQ paths, CGA and TransDrive. It instantiates a PIT but does not connect `$CF28–$CF2B` to it. Keyboard clock/data callbacks are stubs. A PIT callback drives the speaker level, but the card file never assigns its `m_pc_spkrdata` gate, so sound remains incomplete. Mailbox and clock bytes have no device logic. `$CF28–$CF2B`, `$CF33` and `$CF37` fall back to stored register bytes. Original software provides stronger function evidence for these registers than MAME's “unknown” comments. [Card implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp) · [Slot options](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/cards.cpp#L232).

| Address or area | Evidence and working interpretation | Open trace or hardware test |
| --- | --- | --- |
| Apple `$C500`, `$CFFF` | **Software static:** `AEPC.DISKS` at `$1496–$14FF` (file `$0096–$00FF`) releases C8 with a `$CFFF` read, reads `$C700`, runs the pointer and RAM probes below, and repeats down to `$C100`. It finds the card by probe, not by a fixed slot; slot 5 answers at `$C500`. **MAME source:** returns `$FF` on the slot page and `$C0D0–$C0DF`. | Claim, release, reset, returned bytes and coexistence with motherboard C8 ROM and other slots. |
| Apple `$C800–$CBFF` | **MAME source:** accesses guest I/O ports 0h–3FFh through the I/O map. Most use backing bytes, but CGA handlers at 3D0h–3DFh also serve Apple `$CBD0–$CBDF`; the whole window is not passive RAM. **MAME notes:** `$C822–$C827` is the hard disk request block: operation (2 read, 3 write, 4 verify), dispatch/status, drive, offset into `MSDOSVOL`, and sector count. `$C828–$C82A` holds a three-byte MLI request pointer, with `$C828` bit 7 requesting service. `$C860–$C864` covers keyboard ports. `$CAC1–$CAC5` holds year, month, day, hour and minute. | RAM alias at `$BF000`, request field widths/units and ordering, request-flag masking in the pointer, keyboard IRQ, clock encoding and CGA side effects. Mailbox and clock bytes alone do not implement a service. |
| Apple `$CC00–$CDFF`, `$CE00–$CEFF` | **MAME source:** `$CF05/$CF06` set bits 23–16/15–8 of the 512-byte `$CC00` base; `$CF0E/$CF0F` do the same for the 256-byte `$CE00` base. Every bank or page write changes the base immediately. Low eight bits stay zero; the 512-byte window is only 256-byte aligned. Apple code uses a `$BFF00` base to reach vector `$BFFFE`. | First-read deadline after all four registers, bank/end-of-RAM crossings, low-bit decode, and ordering against pointer and guest accesses. |
| Apple `$CF00–$CF04` | **MAME source:** three exposed pointer bytes backed by an unmasked 32-bit variable. `$CF03` reads/writes then increments; `$CF04` leaves it unchanged. RAM ends at `$BFFFF`; reads above it return `$FF`, writes drop. Pointer writes in `$B0000–$BFFFF` also reach the 16 KiB CGA area at `$B8000`; reads do not alias. | Physical pointer width and wrap, absent RAM and CGA alias behavior. See boundary cases below. |
| Apple `$CF20–$CF27` | **MAME source:** ISM floppy controller with a 16 MHz clock and 250 kbit/s MFM parameters. Read/write meanings and signal polarities appear below. | Original part/revision, controller side effects and timing on hardware, and native Apple 3.5 Drive signaling. |
| Apple `$CF28–$CF2B` | **Software static / inference:** strong evidence for timer channels 0, 1, 2 and control, respectively. MAME leaves these Apple registers unconnected to its PIT. | Clock, gates, counter/latch behavior and timer status/IRQ routing. These affect startup. |
| Apple `$CF2C–$CF2F` | **MAME source:** forwards writes to CGA ports 3D6h–3D9h; no device-specific read handler. **MAME notes:** `$C832/$C833` supplies mode/register selection and data from the Apple. | Readback, address mirrors and mode changes through both Apple and guest paths. |
| Apple `$CF30–$CF32` | **MAME source:** partially implements control clear/set, guest INT, Apple IRQ enable and transfer acknowledgment. Read meanings differ from writes and from the header comment; see the separate map below. | Timer-related low bits, status readback, reset/HALT sequence and real IRQ behavior. |
| Apple `$CF33` | **Software static / inference:** read path consumes keyboard scan codes after testing `$CF32` bit 6. A separate write path uses `$CF33` for control. | Read-to-clear, buffering, IRQ effects and write-side bit meanings. |
| Apple `$CF34/$CF35` | **MAME source:** trapped guest port address, low/high byte. | Full address decode and odd-word bus-cycle reporting. |
| Apple `$CF36` | **MAME source:** //e keyboard modifiers: control bit 0 and shift bit 1 active low; Open Apple bit 2, Solid Apple bit 3; caps lock bit 7 active low. MAME notes identify the driver's choice of `$CF36` or IIgs `$C025`. | Modifier polarity, key state and focus behavior with original software. |
| Apple `$CF37` | **Software static:** writes `$03`, then `$01` on failure, while probing RAM through `$CC00`. | Exact RAM enable/configuration/selection function. No bit assignment is established. |
| Guest `$A0000–$AFFFF`, `$F0000–$FFFFF` | **MAME source:** BIOS upload RAM at `$A0000`, read-only alias at `$F0000`, including the reset vector. | Upload bounds, alias and write behavior with the original loader. |
| Guest `$B8000`, `$BF400`, `$BFFFE` | **MAME source:** CGA area, routing table and Apple-supplied V30 vector. The Apple requests guest INT; MAME's 8259 collects ISA lines without supplying that vector. | Guest writes to the table, vector-write ordering and interrupt completion. |

The model facts above come from the [actual card handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L761) and [CGA port installation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/isa/cga.cpp#L334). Trace the remaining questions, including any original `$C0D0–$C0DF` probe.

### Pointer width and wrap

**MAME source:** `$CF00–$CF02` replace only the low 24 bits of `m_offset`; `$CF03` increments the full `uint32_t` without masking it. The RAM helper checks the address against 768 KiB independently. [Type and RAM helpers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L207).

| Boundary | Pinned MAME behavior | Open hardware test |
| --- | --- | --- |
| `$00FFFF` → `$010000` | Carries into the third exposed byte. | Bank-byte carry. |
| `$0BFFFF` → `$0C0000` | Leaves populated RAM; later reads return `$FF`. | Absent-RAM decode, separate from counter wrap. |
| `$0FFFFF` → `$100000` | Continues above 20 bits. | The patent describes a 20-bit counter; test the actual limit. |
| `$FFFFFF` → `$01000000` | Exposed bytes read zero, but the hidden high byte keeps the address outside RAM. Rewriting all three exposed bytes preserves it. | This model artifact does not establish physical behavior. |

**Software static:** `AEPC.DISKS` at `$149F–$14BA` (file `$009F–$00BA`) checks stable pointer reads, increments through `$CF03`, and low-byte carry into the middle byte. It does not settle the larger wrap cases. **Proposed test:** cover all boundaries with reads and writes through both data ports, and window accesses across RAM limits. Do not copy the hidden-byte artifact into RTL without evidence.

### Timer, keyboard and RAM-probe evidence

**Software static:** the following sequences occur in the hashed 2.0.5 files. **Inference:** their layout and control words strongly identify an 8253/8254-compatible timer at `$CF28–$CF2B`; they do not yet establish its gate/status wiring.

| Register | Original sequence | Inferred function |
| --- | --- | --- |
| `$CF28` | `AEPC.SYSTEM` at `$4244` writes `$12` to `$CF2B`; `$4254/$4258` write/read `$CF28`. | Channel 0; `$12` selects low-byte access, mode 1, binary. |
| `$CF29` | At `$6EB9` (file `$4EB9`), write `$74` to `$CF2B`, then `$12`, `$00` to `$CF29`. `AEPC.DISKS` repeats this at `$1500` (file `$0100`). | Channel 1; low/high bytes, mode 2, count 18. |
| `$CF2A` | At `$6F48` (file `$4F48`), write `$B0` to `$CF2B`, then `$A9`, `$04` to `$CF2A`. | Channel 2; low/high bytes, mode 0, count 1193. |
| `$CF2B` | The three control words select the matching adjacent data registers. | Timer control register. |

The [Intel 8254 control-word format](https://www.cs.cmu.edu/~410/doc/8254.pdf#page=6) supports these decodes. MAME's board notes identify a discrete NEC uPD71054. At the working 1.193 MHz clock, count 1193 lasts about 1 ms; this is a calculation.

After the channel 2 load, the original routine writes `$01` to `$CF31`. The routine polls `$CF31` bit 5, then clears `$01` through `$CF30`. It later loads count 106 and repeats the poll. **Inference:** these accesses suggest a timer gate and an output-status path. **Open trace:** determine the exact bit meanings and polarity.

MAME reports the Apple HALT control state in read bit 5. MAME implements no timer gate action for write bit 0. **Inference:** if that HALT control bit is set, the first poll ends after one pass. Thus, a completed MAME boot does not prove correct calibration. Include timer initialization and this calibration routine in the first compatibility test.

**Software static:** at `$36AD`, `AEPC.SYSTEM` tests `$CF32` bit 6; at `$36BA` it reads `$CF33`, rejects values at/above `$E0`, masks bit 7 for a key table and uses the saved sign bit for make/break handling. This strongly supports a keyboard scan-code read and related ready bit. At `$4B62–$4B6E`, a distinct path writes `$04` to `$CF32` or `$CF33` according to A's sign. **Open trace:** read-to-clear, buffering, IRQ and write-side control functions. Do not give reads and writes the same meaning by assumption.

**Software static:** `AEPC.SYSTEM` writes `$12` to `$CF2B` at `$4246` (file `$2246`). The `RTS` at `$4249` ends this routine. A separate routine starts at `$424A` (file `$224A`) and writes `$20` to `$CF32`. These adjacent addresses do not prove execution order.

At `$6E95–$6EA9` (file `$4E95–$4EA9`), the software writes `$CB` to `$CF30` and `$E4` to `$CF32`. It then reads `$CF33` and writes `$0C` to `$CF31`. Together, the masks include bits 0 to 3 of the control pair. The `$CF32` masks include bits 2, 5, 6 and 7.

**Open trace:** determine which bits affect the hardware. The software writes these bits, but their hardware functions remain unknown. Some bits can have no effect.

**Software static:** `AEPC.DISKS` at `$14BC–$14F4` (file `$00BC–$00F4`) points `$CC00` at `$BF000`, writes `$03` to `$CF37`, and tests `$A5/$52` in `$CC00`; on failure it retries with `$01`. `AEPC.SYSTEM` has a matching write/test helper at `$6E7D`. **Open trace:** establish which RAM configuration or selection this changes. The probe is known; the bit map is not.

### Control reads, writes and IRQ masking

The following table states **MAME source behavior only**. Its `$CF30` handler disagrees with the header's claim of control-value readback. The original timer polling above also calls the `$CF31` read interpretation into question.

| Access | Implemented behavior | Open hardware question |
| --- | --- | --- |
| Write `$CF30/$CF31` | Clear/set actions for bit 4 RESET, bit 5 HALT, bit 6 guest INT, bit 7 Apple IRQ enable. Low bits do not form a complete control-pair implementation. | Reset sequencing and low-bit gate/control functions. The header links bit 3 to a forced 80×25 mode on IRQ; verify that software path. |
| Read `$CF30` | Stored low seven bits plus pending data in bit 7. Control writes do not maintain that backing byte. | Actual control/status readback. Bit 7 is not modeled as IRQ-enable readback. |
| Read `$CF31` | Low stored bits, RESET/HALT in bits 4/5, transfer width in bit 6, guest-read direction in bit 7. | Timer-output/status role of read bit 5 and all mixed read/write meanings. |
| Read/write `$CF32` | Read bit 7 reports pending data. Writing bit 7 clears pending and resumes a guest stalled for I/O. | Other status/control bits, including the keyboard-ready evidence for bit 6 and the `$20` and `$E4` writes recorded above. |
| Apple IRQ | `pending && enabled`. Disabling masks IRQ while preserving pending; enabling again reasserts it. Acknowledgment clears pending and drops IRQ. | Physical timing and interaction with other interrupt causes. |
| Guest interrupt acknowledgment | Returns the byte at card RAM `$BFFFE`; that callback does not clear the guest INT line. `$CF30` bit 6 clears it. | Original assertion, vector and clear ordering. |

Sources: [read handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L859), [write handlers](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L997), and [IRQ/vector handling](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L1130).

### TransDrive controller details

**MAME source:** the card adapter forces a SWIM1 into its ISM mode. These are implemented details, with original hardware validation still open:

| Register or signal | Pinned model behavior |
| --- | --- |
| `$CF20/$CF21` | Read pops FIFO data/mark; empty reads set an error, as does reading a mark as data. Writes queue data/marks. |
| `$CF22` | Read returns and clears error. Write queues a CRC operation. |
| `$CF23` | Parameter RAM read/write advances the index modulo 16. |
| `$CF24/$CF25` | Phases/setup. Phase 0 drives direction; phase 1 drives step; phase 2 reads low at track zero. |
| `$CF26` | Read returns mode. Write clears selected mode bits and resets the parameter index; the adapter prevents clearing bit 6. |
| `$CF27` | Read returns handshake. Write sets selected mode bits and forces bit 6 set. |
| Mode bits | 0 FIFO clear; 1/2 drive selects; 3 action; 4 read=0/write=1; 5 side; 6 ISM; 7 motor. |
| Drive/motor | Motor-on gates selection. Select values 1/2 choose the two drives; 0/3 select none in the adapter. |
| Handshake SENSE bit 3 | Set when a drive exists and `!wpt_r()` is true: a writable drive. Track-zero and write-protect polarities must remain explicit. |
| Side select | The adapter inverts ISM head select for PC drives. |

Sources: [card adapter](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L665) and [ISM register/FIFO logic](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/machine/swim1.cpp#L187).

**MAME source:** the configured drives are PC 360 KiB 5.25-inch and 720 KiB 3.5-inch types. Comments describe native Apple 3.5 Drive status signaling, but the adapter implements PC-style phase/SENSE behavior. Native Apple 3.5 Drive support needs separate work. There is no direct ISM-to-PC DMA/IRQ6 connection in this model; Portware may still implement guest floppy ports and request a V30 interrupt through the Apple bridge. [Drive configuration](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L296).

**Proposed:** sector images can supply ordinary contents, but the controller needs track/sector order, marks, CRC, FIFO service and overrun/underrun, rotation, step/head timing and write protection. At 250 kbit/s, one data byte takes about 32 µs; the 2 µs MFM half-cell is a separate timing unit. Define how formatting and unusual tracks persist in the image format. **Open tests:** read, write, verify, format, wrong CRC, missing sector, protected media, absent drive and drive switching, followed by original-card timing and native Apple 3.5 Drive comparison.

Use the disk-loaded startup described in the v1.31 manual as the reference. An earlier AE catalog advertises direct ProDOS floppy boot and Apple Memory Expansion Card compatibility. The manual requires disk-loaded drivers for ProDOS access to attached drives, and MAME leaves the `$C0nX` registers unimplemented. Direct boot and an independent Apple memory-card mode need version-specific evidence. The catalog gives 7.16 MHz and the manual 7.14 MHz. MAME uses 7.16 MHz and clocks the timer at CPU clock / 6, the standard 1.193 MHz, so 7.16 MHz is half the PC 14.318 MHz clock. Use 7.16 MHz as the working value and confirm the divider on the trace. [AE 1988 catalog](https://mirrors.apple2.org.za/ftp.apple.asimov.net/documentation/advertisements/applied_engineering/AE1988.pdf) · [Manual, pp. 75 and 140](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=159)

## Appletini design

The following is a **proposed implementation**. Resolve the source/model differences above before treating register behavior as fixed. The physical shared-memory read deadline remains the main hardware feasibility gate.

### Slot selection and FPGA interface

Today the Appli-Card runs a Z80 against private banked RAM. The AD8088 runs x86 code in the ARM firmware, with a slot 5 mailbox and an FPGA bus master for the AD8088’s Apple-memory window. The Transporter reverses that memory-access direction. The Apple reads and writes PC RAM through the card aperture. For delegated I/O, the Apple CPU runs the service and accesses its own hardware. Start with a Transporter slot slave and IRQ source. Any case that needs the card to master the Apple bus must come from a trace. [Appli-Card design](../README_APPLICARD.md) · [AD8088 design](../README_AD8088.md) · [Existing slot 5 RTL](../hdl/apple/applicard_card.sv)

- Add a third explicit processor value. Preserve the saved values for Z80 and AD8088 and handle unknown values at load. The current firmware, menu, and FPGA mode encode two choices. Add a dedicated Transporter responder behind the same slot 5 selection and physical-slot policy. Route only the active responder into the physical and ONE//e bus paths. [Config enum](../ps_sources/frontend/config_menu.h#L71) · [Slot configuration](../ps_sources/frontend/config_menu.c#L5121) · [Bus wiring and policy](../hdl/apple/apple_top.sv#L2405)
- Reuse the existing C8 owner and internal-ROM state, with the live physical select and isolation checks used by the other card responders. Keep deadline-sensitive register reads, pointer/window state, port backing bytes, device decode, data staging and Apple IRQ state in the FPGA. Include the timer/status and keyboard-input paths needed by startup. A qualified Apple access must change the pointer at most once. Drop data drive and IRQ on slot disable or physical isolation, and clear pending requests on reset or mode change. [C8 tracking](../hdl/apple/soft_switch_manager.sv#L206) · [SSC expansion-window decode](../hdl/apple/ssc_card.sv#L90) · [Physical IRQ and RDY](../hdl/apple/apple_bus_wrapper.sv#L485).
- Pass each delegated bus transfer with its full port address, active byte lanes, direction and data to the bridge. Use the model's `$CF32` pending/acknowledgment, `$CF31` transfer status and `$CF34/$CF35` address as a starting map, subject to the original-software trace. For a read waiting for supplied data, publish the result before completion. Tag outstanding work so a late acknowledgment after reset cannot finish a new request. Support separate notification and waiting decisions; resolve routing by direction and lane before committing the design. Do not implement MAME's odd/even-nibble shortcut as a hardware rule. [MAME transfer handling](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/bus/a2bus/pc_xporter.cpp#L1090).
- Route the Apple-supplied guest INT and vector using the verified control map. Keep guest INT separate from Apple IRQ and preserve pending requests when Apple IRQ is masked, as the current model does. Verify assertion, mask/re-enable, acknowledgment and guest-INT clearing against original software and hardware.

A proposed read that requires both notification and waiting follows this sequence. It shows the service order, not a verified routing-bit decode or a requirement to rewind the instruction pointer. Confirm the register mapping and suspension boundary on the trace.

```mermaid
sequenceDiagram
    participant Guest as V30 guest on ARM
    participant Bridge as FPGA slot 5 bridge
    participant Apple as Apple CPU with Portware
    Guest->>Bridge: Read transfer pending, suspend before committing result
    Bridge->>Apple: Publish pending, status and port; assert IRQ if enabled
    Apple->>Bridge: Read $CF31 and $CF34/35
    Apple->>Bridge: Write byte into $C800 window, write 1 to $CF32 bit 7
    Bridge-->>Apple: Release Apple IRQ
    Bridge-->>Guest: Supply result and complete transfer exactly once
```

### Shared RAM and the physical bus deadline

The Z-7020 has 140 blocks of 36 Kib, about 630 KiB of raw block RAM before existing logic and memory use. A full 768 KiB Transporter RAM image therefore requires external memory. The existing physical wrapper leaves `RDY` high impedance. A `$CF03` or `$CF04` read, or a read inside the `$CC00` or `$CE00` page window, must return the correct byte in the same Apple bus cycle. An ARM polling loop or a cache that merely works for sequential transfers cannot guarantee an arbitrary pointer read. [AMD DS190](https://docs.amd.com/api/khub/documents/juMnxca71Tf2gfjmNyjM8A/content) · [Physical bus wrapper](../hdl/apple/apple_bus_wrapper.sv#L513)

| Backing option | Benefit | Gate |
| --- | --- | --- |
| PS DDR with a reserved RAM range and a bounded FPGA access path | Keeps the full RamWorks allocation and gives the ARM a direct guest-memory path. The existing layout lists a free range starting at `0x3F900000`, subject to a full reservation check. | Measure cold misses and establish a service bound under load. HP3 currently serves Disk II staging. Any shared path needs explicit arbitration and a Disk II regression. Include controller contention and refresh in the budget. Define cache visibility and ownership before enabling ARM and FPGA writers. [DDR map](../ps_sources/frontend/compositor_layout.h#L54) · [HP3 wiring](../hdl/apple/apple_top.sv#L1357) · [AMD DDR arbitration](https://docs.amd.com/r/en-US/ug585-zynq-7000-SoC-TRM/DDR-Port-Arbitration) |
| A reserved PSRAM region with an FPGA deadline path | Can build on the existing deadline, queueing and forwarding patterns used for Apple memory. | The two PSRAM devices share commands and address. The current design makes the full logical 8 MiB available to RamWorks. A reservation must have an explicit capacity and coexistence rule, and the ARM guest-memory access cost must be measured. [PSRAM service](../hdl/apple/psram_simple.sv) · [PSRAM device wiring](../hdl/apple/psram_driver.sv#L456) · [Current allocation](../hdl/apple/apple_top.sv#L72) |
| Small block RAM front end | Gives fast access to the 1 KiB port window, both page windows, the routing table page, queues, and frequently used or prefetched data. It is also enough for a bridge proof with a small synthetic RAM range. | Use as a front end or test fixture. The full physical-slot gate still requires a proven backing path for cold reads at arbitrary addresses. |

**Proposed:** start a memory request on the earliest valid event. Pointer writes and increments can prefetch. Every bank or page write (`$CF05`, `$CF06`, `$CF0E`, `$CF0F`) changes the corresponding window. A following read may address any byte, including the last byte or a bank-crossing byte. A normal 6502 instruction sequence leaves several cycles; a bus master could read in the next cycle. Whole-window prefetch is a candidate, with a required service bound still to prove.

**Calculation and Appletini source:** a 512-byte fill needs 64 beats of 64-bit data, about 481 ns at 133 MHz before other costs. HP3 uses AXI3 with a maximum 16-beat burst, so this needs at least four bursts. Of the approximately 720 ns gross adjacent-cycle budget below, about 239 ns remains for initial latency, arbitration, refresh, transfer gaps and staging. Address requests may overlap data, but the configuration gives no worst-case guarantee. **Open hardware gate:** measure and bound this path under load. If it cannot meet the deadline, choose another memory service design before claiming physical-slot support. [HP3 interface](../scripts/create_project.tcl#L483).

Keep the `$C800` backing bytes in block RAM while preserving device side effects, including CGA accesses through `$CBD0–$CBDF`. Forward queued writes to reads and preserve order across line crossings and Apple/guest access. Establish one coherent view of BIOS, ports, routing and display state. Define ARM cache ownership and ordering explicitly. Test guest instruction fetch after Apple upload and Apple reads immediately after guest writes.

The current RTL samples the authoritative address eight fabric clocks after the filtered PHI0 rise and opens the normal data-drive window at clock 25. At the configured 133 MHz, the difference is 17 clocks, about **128 ns gross**. Responder and output staging consume part of it. The early PHI1 sample can give a speculative lookup about **0.49 µs gross** on a stock //e, but it can still contain the preceding DMA address. Speculation can fetch data. Only the later qualified cycle can drive data or change card state. A prefetch launched after a pointer write at tap 59 has about **0.72 µs gross** to the next drive opening in an adjacent-cycle stress case at nominal //e speed. These are calculations from RTL taps and nominal clocks. Measure the actual pin margin. [Phase taps](../hdl/apple/apple_bus_wrapper.sv#L95) · [Authoritative sample and strobes](../hdl/apple/apple_bus_wrapper.sv#L624) · [Fabric clock setting](../scripts/create_project.tcl#L150)

Record request and reply times, queue high-water marks, late replies and the first failing pointer and bus owner. Measure data setup and hold at the Apple connector against the supported host’s requirement. Cover cold random seeks, pointer carry, line crossing, read-after-write before a queue drains, and an intervening reset or C8 release. Use adjacent cycles and injected late responses in simulation. On the board, repeat with scanout, Disk II, SD or USB activity and competing ARM DDR traffic. A passing run requires zero wrong bytes, dropped writes or missed deadlines, correct pin release and positive setup and hold margin. A soak run complements the latency bound. Keep simulation, Vivado results and board captures separate. [Existing memory service counters](../hdl/apple/apple_top.sv#L1346) · [PSRAM admission and counters](../hdl/apple/psram_simple.sv#L233)

### ARM firmware and user-facing devices

#### BIOS and NEC `0Fh` evidence

**Software static:** the hashed 8192-byte `AEPCBIOS` contains twelve `$0F` bytes. Inspection found operands and data, with no NEC-prefix instruction in the identified BIOS code:

| File offset of `$0F` | Inspected use |
| --- | --- |
| `$0115`, `$02EB` | Immediate masks in `AND AH,$0F` and `AND DL,$0F`. |
| `$0433` | Keyboard translation table data; the handler selects BIOS offset `$E3E8` at `$E99E`. |
| `$065E` | Immediate in `MOV CX,$000F`. |
| `$0A09`, `$1499`, `$1E79` | Branch/call displacement bytes. |
| `$0BC8`, `$0D43` | Immediate values in `MOV AH,$0F`. |
| `$1068` | Immediate in `CMP AH,$0F`. |
| `$1AC6`, `$1AC8` | Character bitmap data in the font at file `$1A6E`, referenced as BIOS offset `$FA6E`. |

The reset jump at file `$1FF0` targets `F000:E0F5`. These findings narrow the BIOS dependency question. **Open trace:** executed instructions in BIOS, DOS, utilities, drivers and any patched or downloaded code. A byte scan alone is not an instruction trace, and this result does not establish full V30 compatibility. [Static inspection and exact instruction offsets](PC_TRANSPORTER_REVIEW.md#nec-0fh-use-the-supplied-bios-does-not-show-a-dependency).

**Appletini source:** the current build selects `CPU_8086`, where opcode `$0F` means `POP CS`. The source contains 186 handlers (PUSHA/POPA, BOUND, immediate PUSH/IMUL, port-string I/O, immediate shifts, ENTER/LEAVE), but the current profile excludes them. `CPU_V20` enables the illegal-opcode path and shift-count limit, removes `POP CS`, and makes `$D6` execute `XLAT` through the `CPU_NO_SALC` fall-through. It does not implement the NEC `0Fh` group. NEC bit-field `INS`/`EXT` are distinct from the 186 byte/word port-string instructions. [Configuration](../third_party/xtulator_cpu/cpuconf.h#L22) · [0F decode](../third_party/xtulator_cpu/cpu.c#L1296) · [186 handlers](../third_party/xtulator_cpu/cpu.c#L1879) · [D6 decode](../third_party/xtulator_cpu/cpu.c#L3045).

**Proposed:** exercise the original BIOS with an explicit, separately compiled or runtime-selected CPU profile and unsupported-instruction diagnostics. Keep the AD8088 profile stable. Use the trace to choose the work needed for the first boot, while retaining full NEC instruction support as a separate compatibility requirement. MAME's NEC interpreter supplies a reference, not proof that the vendored core is sufficient. [NEC instruction implementation](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/cpu/nec/necinstr.hxx#L358).

#### Word I/O and suspension

**Documented:** V30 word I/O at an even port uses one bus cycle; an odd port uses two. **Appletini source:** the AD8088 adapter truncates port addresses to eight bits and splits word operations into byte calls. **Proposed:** use a separate machine context, preserve full 16-bit port addresses and active byte lanes, and model aligned words and odd-word split transfers separately. **Open trace:** card decode of upper port addresses, reporting of split transfers and transfer-size flags. [NEC manual, section 4, printed page 4-3](https://www.dosdays.co.uk/media/nec/NEC-V20-V30-Users-Manual.pdf) · [AD8088 callbacks](../ps_sources/frontend/ad8088_machine.c#L106).

**Documented:** the 80186 and V30 leave SI unchanged during `INSB/INSW`. They leave DI unchanged during `OUTSB/OUTSW`. The 8086 does not implement these instructions. [Intel 80186/80188 manual](https://ardent-tool.com/CPU/docs/Intel/808x/manuals/210912-001.pdf) · [NEC manual](https://www.dosdays.co.uk/media/nec/NEC-V20-V30-Users-Manual.pdf).

**Appletini source, corrected 2026-11-06:** the vendored `INSB/INSW` handlers changed SI and the `OUTSB/OUTSW` handlers changed DI. The core now steps DI only for input and SI only for output. The `CPU_8086` build still excludes these handlers; `cpuconf.h` now takes the profile from the compiler command line, so a `-DCPU_V20` build enables them while the AD8088 keeps `CPU_8086`. [Input handlers](../third_party/xtulator_cpu/cpu.c#L1998) · [Output handlers](../third_party/xtulator_cpu/cpu.c#L2052) · [Profile selection](../third_party/xtulator_cpu/cpuconf.h#L25) · [Change list](../third_party/xtulator_cpu/LICENSE.txt).

**Appletini source, added 2026-11-06:** `CPU_t` has an `io_pending` flag. A port callback sets it when the transfer cannot complete yet. Every I/O instruction (`IN`, `OUT`, `INS`, `OUTS`) tests the flag right after the port call and, if set, restores IP to its first prefix byte and returns from `cpu_exec` before it changes any register or memory. `cpu_exec` makes no progress while the flag is set, and `cpu_reset` clears it. Inside REP, each completed iteration has already committed, so a pending iteration leaves CX, the index and memory at the last completed transfer, as a real 80186 does when an interrupt arrives mid-REP. The machine model owns the rest: it latches the request, posts it to the bridge, clears the flag on acknowledgment, returns the completed data on the retry instead of posting again, and delivers no interrupt while a transfer is pending. [Retry macro](../third_party/xtulator_cpu/cpu.c#L1116) · [Flag](../third_party/xtulator_cpu/cpu.h#L44).

**Tests:** `scripts/test_ad8088_cpu.c` models the pending bridge with per-call masks. The default build covers `IN` and `OUT` pending once and twice, a slice while pending, and exactly-once completion. The `-DCPU_V20` build adds `REP INSB` pending on the third transfer, `REP OUTSB` pending on the second, `REP INSW` with DF=1, and a single `OUTSB` pending once. All pass on 2026-11-06 under the `CPU_8086`, `CPU_V20` and `CPU_186` profiles with the MinGW gcc that Xilinx 2025.2 bundles. The file header gives the build lines.

**Still open:** the AD8088 adapter splits word I/O into two byte calls and cuts the port to eight bits, so a pending high byte would repeat the low byte. The Transporter machine must give the core real word callbacks with both lanes, one request record per bus cycle, and hold a completed first byte of an odd word until the second completes. MAME's even-nibble case, complete with latched data then hold, needs a second flag that stops `cpu_exec` after the instruction commits. Test delayed replies, odd ports, port boundaries, interrupts and reset against that machine.

#### Services and devices

- **Scheduling and timer — proposed.** Run bounded guest slices while servicing the Apple bridge, video, USB and storage. Timers must advance on card time while the guest is halted or waiting, including Apple-side startup calibration. Use CPU clock / 6 as the model's working timer rate until the physical divider is checked. Implement the inferred timer counters/latches/control and resolve gate/status wiring early. Accept the Apple-supplied guest vector and INT, including wake from `HLT`; keep guest INT and Apple IRQ separate. [Firmware service loop](../ps_sources/frontend/main.c#L5282).
- **Video and keyboard — proposed.** Share CGA state across guest ports, Apple `$CF2C–$CF2F` and the `$CBD0–$CBDF` port window. Start with text, cursor, attributes and status, then graphics. Verify the model's pointer-write alias from `$B0000–$BFFFF` into the 16 KiB at `$B8000`. Render through RGB565 with an explicit Apple/PC transition. Feed USB make/break events through the original Apple-side scan-code/status path once `$CF33/$CF32` behavior is resolved, and verify the resulting guest ports 60h–64h and IRQ. Supply `$CF36` modifiers for the //e driver; restore Apple focus on exit. [Compositor](../ps_sources/frontend/compositor.c) · [USB HID](../ps_sources/frontend/usb_hid_service.c) · [Original keyboard connection](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=53).
- **Storage and sound — proposed.** First use original Portware and a bootable `MSDOSVOL`, validating the request block and ProDOS mailbox fields on the trace. Add TransDrive with the register side effects, media model and tests above. Use Appletini audio for the speaker after timer/gate behavior is resolved. Test printer, serial, mouse and clock drivers individually after storage works. [Original hardware and Portware](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=93).

## Work order and acceptance

All gates below remain **open**. Static findings guide the tests; they do not mark a gate complete. An instrumented emulator shows that model's execution. Use original-card captures to resolve disputed hardware behavior and Appletini captures to prove the new implementation's physical timing.

| Gate | Work | Evidence to keep |
| --- | --- | --- |
| 1. Resolve startup and register differences | Use the hashed 2.0.5 disk on a //e model with slot 5 `pcxport`. Trace slot/C8 probing, `$CF37` RAM tests, timer setup and `$6F48` calibration, `$CF30–$CF33` reads/writes, keyboard input, BIOS upload and reset at `$FFFF0`. Capture routing-table contents and the vector/INT sequence. Record the first divergence; a model fix must not become hardware evidence by assumption. Compare disputed functions with an original card. | Source/image/ROM hashes, Apple and guest PCs, timer gates/status, routing bytes decoded by direction/lane, first completed or stalled request, and separately identified original-card captures. [Earlier media](https://mirrors.apple2.org.za/ftp.apple.asimov.net/images/hardware/processor/pc_transporter/) · [MAME watchpoints](https://docs.mamedev.org/debugger/watchpoint.html). |
| 2. Prove slot and RAM timing | Build a small FPGA bridge and synthetic diagnostic. Verify C8, both pointer data ports and all listed carry/wrap boundaries. Read immediately after each of `$CF05/$CF06/$CF0E/$CF0F`, including bank-only changes, the last window byte, bank/end-of-RAM crossings and cold random addresses. Cover port device effects, full-range backing, read-after-write and isolation. Verify ONE//e separately. | RTL simulations, synthesis/implementation reports, bounded memory latency, connector setup/hold captures and counters under simultaneous disk/video/ARM load. Zero wrong bytes, dropped writes or late replies. |
| 3. Prove Portware service and CPU suspension | The core's string-I/O register fix and `io_pending` retry hook are in place with host tests (see above). Build the Transporter machine model on them: word callbacks with lanes, one request record per bus cycle, acknowledgment handling and no interrupts while pending. Then test synthetic and original Apple handlers. Cover separate wait/notification controls, both byte lanes, aligned/odd words and REP in both DF directions. Include delayed replies, IRQ masking, guest INT completion and reset during pending work. Verify the register map from gate 1. | Record tests without waits and with delayed replies. Verify data, unchanged registers, active indexes, CX and completion counts. Keep paired Apple/guest traces of pending requests and IRQ transitions. Confirm that each transfer completes once. |
| 4. Boot DOS through the Apple | Use unmodified `AEPC.SYSTEM`/`AEPCBIOS`, verified timer and keyboard paths, CGA text on DVI and USB input. Boot a verified DOS hard disk file on ProDOS. Trace executed opcodes and unsupported instructions; keep BIOS-only static evidence separate. Copy a file both ways with `TRANSFER.EXE`, test persistence, `/RAMAEPC` ownership and AE's `PCTRAM.SYSTEM`. | Boot capture, executed CPU-profile coverage, keyboard make/break results, file hashes, restart persistence, RAM test and reset results. [Original RAM test](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=114). |
| 5. Expand device and host coverage | Add CGA graphics, TransDrive, speaker and optional Portware drivers. Test FIFO/CRC/parameter/mode side effects, signal polarities, read/write/verify/format, bad or absent media, write protection and drive switching. Validate guest floppy service separately from the Apple-side controller wiring. Treat native Apple 3.5 Drive signaling as separate work. Test Control Panel resume/reboot/quit, slot isolation, both existing slot 5 modes, then IIgs, II Plus and vTW. | Host/firmware/FPGA revisions, controller and guest traces, media persistence, original-card timing comparisons, feature matrix and named failures. [Pinned ISM model](https://github.com/mamedev/mame/blob/ce12c0474a7f5786748327d0320179344bddffba/src/devices/machine/swim1.cpp). |

The archived AE system disk is a source for the original launcher, BIOS, drivers and utilities. Its bundled `MSDOSVOL` is listed as utility media. Use a separately verified bootable DOS 3.3 image or prepare a bootable volume through AE’s documented setup. Keep original media as local test inputs, work from copies, and record the hash of each input and any prepared boot image. For write tests, let only one system own the enclosing ProDOS image and verify that DOS changes persist after close and reopen. [AE distribution files, pp. 93–95](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=112) · [AE hard disk setup, pp. 147–155](https://www.applefritter.com/appleii-box/harrowalsh.de/APPLEBOX/APPLE2/Manuals/AE%20PC%20Transporter%20v131.pdf#page=166)

Track the FPGA implementation and ARM firmware build separately. The bridge needs Vivado synthesis, implementation and an exported XSA. The guest machine, menu and services need a Vitis firmware build. Package the matching bitstream and ARM binaries only after both build results are known. The physical //e run then verifies the combined system, including C8 contention and IRQ release. [Vivado workflow](../README_VIVADO.md) · [Firmware build and packaging scripts](../scripts/SCRIPTS_README.md)

## Baseline checks and review decisions

- **Prior study baseline, recorded 2026-11-06:** `python -B scripts/test_applicard_card.py` passed; `python -B scripts/test_ad8088_card.py` stopped at the config assertion expecting `117U` while the source defines `124U`. These tests were not rerun for this documentation update. Resolve that baseline when adding the third selection, then rerun both card checks and the AD8088 CPU test. [Test assertion](../scripts/test_ad8088_card.py#L249) · [Config version](../ps_sources/frontend/config_menu.c#L44).
- The first delivery target is an enhanced //e with an empty physical slot 5, original AE software, a ProDOS-backed bootable DOS volume, and Appletini DVI and USB. Use the 768 KiB card profile and validate the BIOS path with the optional 8087 absent. A passing emulator boot would establish a software milestone. A passing board test is required for the physical slot claim.
- Keep shared RAM backing open until the physical read deadline has a proven bound. Keep routing, mixed control readback and timer/keyboard wiring open until the source disagreements are resolved. No original boot trace has yet confirmed this register map.

| Topic | Established by this review | Still needed |
| --- | --- | --- |
| Pointer wrap | MAME exposes three bytes but increments an unmasked 32-bit variable. The driver tests low-byte carry. | Physical width, high-boundary wrap and absent-RAM behavior. |
| NEC `0Fh` | No prefix instruction found in identified code of the hashed BIOS; twelve byte occurrences classified. Current core lacks the NEC group. | Executed coverage across BIOS/DOS/drivers and the intended V30 compatibility profile. |
| Formerly unknown registers | Original software strongly identifies timer channels/control and a keyboard scan-code read; `$CF37` participates in RAM probing. | Gates, status/read-to-clear, IRQ, write controls and RAM-selection bit meanings. |
| Routing and control | Pinned handler behavior, table stride, IRQ masking and read/comment conflicts are known. | Shipped-card direction/lane semantics and full read/write map. |
| TransDrive | Pinned ISM register side effects and PC-drive wiring are known. | Hardware timing/polarities, media-format fidelity and native Apple 3.5 Drive support. |
| Appletini feasibility | Existing bus taps and AXI configuration give calculations and test targets. | RTL/firmware implementation, builds, emulator execution and physical-board proof. |
