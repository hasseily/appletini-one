# PC Transporter bus probe

Use an enhanced Apple //e with the Appletini PC Transporter in slot 5 and the physical slot empty. Start from an idle, disposable session before running any PC Transporter software. The probe checks that the guest is still in reset and that the card has no pending interrupt. It changes 13 card RAM bytes and tries to restore them and the pointer and page registers before returning. A failed bus access can prevent restoration.

## Run

Boot `build/PCT.PROBE.po` or copy `build/PCT.PROBE.bin` to a ProDOS volume as a `BIN` file named `PCT.PROBE` with load address `$2000`. From the BASIC.SYSTEM prompt, run these commands.

```text
BLOAD PCT.PROBE,A$2000
CALL 8192
```

To use the Monitor after `BLOAD`, enter `CALL -151` from BASIC, then enter `200CG`. The Monitor entry calls the probe and returns to the Monitor prompt. [Apple II Reference Manual](https://www.applelogic.org/files/AIIREF.pdf)

The probe prints `PCT5 RESULT 00` when all checks pass. For a failure, read the hex code on screen and examine `2003.200B` at the Monitor prompt. The probe releases the C8 window before reporting the result.

| Apple address | Result |
| --- | --- |
| `$2003–$2006` | ASCII `PCT5` (`50 43 54 35`) when the probe finishes |
| `$2007` | Status, with `00` for pass and `FF` while running |
| `$2008`, `$2009` | Expected and observed bytes |
| `$200A–$200B` | Apple bus address of the failed read, low byte first |

| Code | Check |
| --- | --- |
| `01`–`05` | Internal Cx ROM off, slot page response, guest idle, pointer latches, C8 release |
| `10`–`12` | Nonincrementing read, low-byte carry, bank carry |
| `13`–`15` | Last RAM byte to absent RAM, carry beyond 20 bits, 24-bit wrap |
| `20`–`23` | CC window last byte after bank and page changes, including a bank crossing |
| `24`–`26` | CE window last byte after bank and page changes |
| `30`–`33` | Read after write through the held pointer, advancing pointer, CC window and CE window |
| `40` | A card RAM byte failed readback after restoration |

The probe seeds distinct backing addresses, then reads each page-window target on the instruction immediately after a bank or page write. The 65C02 still fetches the instruction and its address bytes between those I/O cycles. The expected 24-bit wrap and absent-RAM read of `$FF` come from the Appletini design. Original-card behavior at these boundaries is open. [PC Transporter study](../../docs/PC_TRANSPORTER_STUDY.md)

## Build and check

Run from the repository root in PowerShell with ACME on `PATH`.

```powershell
$out = 'software/pc_transporter_probe/build'
New-Item -ItemType Directory -Path $out -Force | Out-Null
acme -f plain -o "$out/PCT.PROBE.bin" software/pc_transporter_probe/probe.a
python -B software/pc_transporter_probe/verify_probe.py "$out/PCT.PROBE.bin"
```

The verification script uses `py65`. It runs both entry points and checks that simulated faults produce the expected codes. It also checks restoration and the displayed result. This fixture checks program behavior against a register model. A physical //e run and connector timing measurements remain open.

To create the ProDOS-order disk from the repository's ProDOS 2.4.3 and BASIC.SYSTEM files, run the following after assembling the binary.

```powershell
@'
import sys
from pathlib import Path
sys.path.insert(0, 'software/net_transport_checkpoint')
from prodos_disk import build_image, read_root_files
out = Path('software/pc_transporter_probe/build')
template = Path('software/ProDOS_2_4_3.po').read_bytes()
basic = read_root_files(template)['BASIC.SYSTEM']
probe = (out / 'PCT.PROBE.bin').read_bytes()
disk = build_image(template, basic, name='BASIC.SYSTEM', volume='PCT.PROBE',
                   files=[('PCT.PROBE', probe, 0x06, 0x2000)])
(out / 'PCT.PROBE.po').write_bytes(disk)
'@ | python -B -
```
