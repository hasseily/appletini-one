# Apple IIgs safety profile

Firmware F1.0.6 uses the boot ROM to identify the physical host. For IIgs use,
install the card in physical slot 7 and set that slot to **Your Card**.

## Boot and identity

The boot responder serves its normal C7/C8 reads before identification without
interpreting pin 39 or requiring a banner or signature-read order. The enhanced
IIe autostart ROM checks C705, C703 and C701 before entering C700; it does not
perform the C707 read required by the removed bootstrap classifier.

The boot ROM calls `SEC / JSR $FE1F` before testing the legacy ROM ID bytes and
before displaying `A:APPLETINI`. The PL accepts reports only during a boot-report
session opened by a served C700 entry, this card's C8 claim and WINDOW_BEGIN.
The report session is independent of the menu countdown. A zero menu timeout
cannot invalidate identification or the IIgs slot report. Unrelated LINTXT or
SuperSprite writes to the shared C0F0 address cannot report a machine type.

The command decoder handles each command separately:

- $21-$24 report II/II+, IIe, enhanced IIe, or IIgs.
- $26 starts the physical auxiliary-memory probe; it is not machine ID 6.
- $30/$31 report the auxiliary-memory result before the legacy machine ID.
- $27 escapes the next byte as the raw IIgs $C02D slot mask. That byte cannot
  also trigger an ordinary command.

The first native machine ID stays locked across Apple warm resets. Later
reports within the legacy family (IDs 1-3) do not replace it or fault. This
allows a vTW cold reboot using its embedded enhanced IIe ROM on an unenhanced
IIe or II+. A legacy/IIgs conflict or an unsupported report during an authorized
report session sets a sticky fault. The host-policy reset clears identity.
ONE//e cannot supply a physical-host report.

## Physical output policy

Before a supported legacy report, /INH, /DMA, address/RW drive and physical IRQ
remain disabled. They remain disabled throughout IIgs operation. PS overrides
cannot grant these rights. RDY and /NMI stay high-impedance. The dedicated
open-collector RESET path retains the normal boot hold and reset behavior.

ID 4 immediately selects active-low /M2SEL qualification. Physical C0F reads
and writes also require live /DEVSEL. Legacy hosts do not use this /DEVSEL
rule, allowing a virtual slot-7 interface in another physical slot.

The boot responder is independent of optional-slot permission:

| Host report | Optional physical slots |
| --- | --- |
| Unknown, unsupported or faulted | None |
| Legacy ID 1-3 | Configured slots 1-7 |
| IIgs, slot report incomplete | None |
| IIgs, valid external slot-7 report | Slot 7 only |

C8 ROM and CA00 scratch RAM still require this card's C8 claim and respect
internal-ROM selection. CFFF releases the claim and never receives card data.
Changing from ID 4 to its slot-mask report cannot hide the executing boot ROM.

The physical write arbiter rejects every field of a prohibited ownership
request. The data wrapper records ownership with the byte, so revoking
permission also blocks a byte already buffered for output. ONE//e isolation,
identity/slot faults and reset assertion reach the final pad mask directly;
there is no extra clock delay on fault isolation. The physical and private
ONE//e arbiters remain separate.

## IIgs services and limits

Confirmed external slot 7 supports the boot menu and SmartPort. SuperSprite
uses polling; physical IRQ remains off. Logical slots 1-6, memory replacement,
RamWorks, vTW, AD8088, PS host-memory operations and fake-SHR C029 writes remain
blocked. Passive bus capture and video output remain available.

When another slot boots without running Appletini's ROM, the separate LINTXT
path can still serve C0F I/O with low /M2SEL and live /DEVSEL, when SuperSprite
is disabled. This path does not expose SmartPort ROM/FIFO state or authorize
boot commands, IRQ or bus ownership.

The boot-ROM identity design does not prove that every pre-ID ROM response is
electrically safe on a IIgs. Its initial unqualified reads occur before the
machine can report ID 4, and /M2SEL need not select the slow bus during the
motherboard $FE1F call. This remaining pre-ID data-contention limitation is
separate from the enforced default-off /INH and DMA policy. No hardware change
or universal pre-ID contention guarantee is claimed.

References: Apple's [machine identification note](https://mirrors.apple2.org.za/apple.cabi.net/FAQs.and.INFO/A2.TECH.NOTES.ETC/A2.CLASSIC.TNTS/a2misc007%281%29.htm)
and [IIgs Technical Note 68](https://apple2.gs/technotes/tn-iigs-068/).

## Release validation

Run the boot, bus-policy, wrapper, overlay and ONE//e regressions. Build a fresh
PL and PS image, and package with the accepted bitstream explicitly. Require
nominal routed setup WNS of at least +0.200 ns, zero timing failures, nonnegative
hold and pulse-width slack, clean routing and bus skew, and valid constraints.
Temporary implementation margins must be removed before these final checks.
Record the exact bitstream, XSA, executable and firmware hashes.

Hardware qualification requires cold/warm boots on unenhanced and enhanced
IIe, Europlus in physical slot 4, and IIgs in physical slot 7; vTW reboots;
LINTXT when another slot boots; and IIgs internal/external slot remapping.
Scope /INH, /DMA, address direction, data direction, PHI0, /M2SEL and /DEVSEL.
Build reports and simulations do not establish physical hardware validation.
