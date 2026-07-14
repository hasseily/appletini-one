"""Build BANKTEST.COM: on-target proof that the Applicard's 32 x 64K RAM
banks (2 MB) are real and distinct.

Method (runs on the Z80 under CP/M):
  1. Relocate to $9000 -- the common upper 32K, which stays mapped to bank 0
     while the lower 32K banks. Executing from there through the whole test
     also proves the common-area mapping: if bit 6 didn't pin the upper half,
     the code would vanish on the first bank switch.
  2. Write pass: for bank 1..31, select (bank<<1)|$40 on port $C0 and store
     the bank number at $4000.
  3. Verify pass: re-select each bank and compare $4000. Two phases, so any
     aliasing (e.g. only 8 real banks: 512 KB hardware) shows up as a stale
     value written by a higher bank.
  4. Restore bank 0 / stock register, print the verdict via BDOS, warm boot.

On a real GZ80 (3 bank bits) this prints FAIL at bank 8 -- that's correct:
the test measures 2 MB, our extension.

Usage: python scripts/build_applicard_banktest.py
Writes software/applicard/BANKTEST.COM (and a .lst listing next to it).
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "software" / "applicard" / "BANKTEST.COM"

PAYLOAD_ORG = 0x9000
BANK_PORT = 0xC0
COMMON_BIT = 0x40
BANKS = 32
PROBE_ADDR = 0x4000


class Asm:
    def __init__(self, org):
        self.org = org
        self.code = bytearray()
        self.labels = {}
        self.fixups = []   # (offset_of_disp_byte, label)
        self.listing = []

    def pc(self):
        return self.org + len(self.code)

    def label(self, name):
        self.labels[name] = self.pc()
        self.listing.append(f"{self.pc():04X}          {name}:")

    def emit(self, text, *byts):
        self.listing.append(
            f"{self.pc():04X}  {' '.join(f'{b:02X}' for b in byts):<12}  {text}")
        self.code += bytes(byts)

    def jr(self, cond_opcode, label, text):
        self.emit(text, cond_opcode, 0x00)
        self.fixups.append((len(self.code) - 1, label))

    def resolve(self):
        for off, name in self.fixups:
            target = self.labels[name]
            disp = target - (self.org + off + 1)
            if not -128 <= disp <= 127:
                raise SystemExit(f"JR out of range to {name}")
            self.code[off] = disp & 0xFF


def build_payload():
    a = Asm(PAYLOAD_ORG)

    a.emit("LD SP,0BF00h        ; own stack in the common upper 32K",
           0x31, 0x00, 0xBF)

    a.emit("LD C,1              ; write pass, banks 1..31", 0x0E, 0x01)
    a.label("wloop")
    a.emit("LD A,C", 0x79)
    a.emit("RLCA                ; bank -> register bits 1-5", 0x07)
    a.emit(f"OR {COMMON_BIT:02X}h               ; keep upper 32K common",
           0xF6, COMMON_BIT)
    a.emit(f"OUT ({BANK_PORT:02X}h),A", 0xD3, BANK_PORT)
    a.emit("LD A,C", 0x79)
    a.emit(f"LD ({PROBE_ADDR:04X}h),A       ; marker = bank number",
           0x32, PROBE_ADDR & 0xFF, PROBE_ADDR >> 8)
    a.emit("INC C", 0x0C)
    a.emit("LD A,C", 0x79)
    a.emit(f"CP {BANKS:02X}h", 0xFE, BANKS)
    a.jr(0x20, "wloop", "JR NZ,wloop")

    a.emit("LD C,1              ; verify pass", 0x0E, 0x01)
    a.label("vloop")
    a.emit("LD A,C", 0x79)
    a.emit("RLCA", 0x07)
    a.emit(f"OR {COMMON_BIT:02X}h", 0xF6, COMMON_BIT)
    a.emit(f"OUT ({BANK_PORT:02X}h),A", 0xD3, BANK_PORT)
    a.emit(f"LD A,({PROBE_ADDR:04X}h)", 0x3A, PROBE_ADDR & 0xFF,
           PROBE_ADDR >> 8)
    a.emit("CP C", 0xB9)
    a.jr(0x20, "fail", "JR NZ,fail")
    a.emit("INC C", 0x0C)
    a.emit("LD A,C", 0x79)
    a.emit(f"CP {BANKS:02X}h", 0xFE, BANKS)
    a.jr(0x20, "vloop", "JR NZ,vloop")

    a.emit("LD DE,okmsg", 0x11, 0x00, 0x00)
    ok_fix = len(a.code) - 2
    a.jr(0x18, "done", "JR done")

    a.label("fail")
    a.emit("LD DE,failmsg", 0x11, 0x00, 0x00)
    fail_fix = len(a.code) - 2

    a.label("done")
    a.emit(f"LD A,{COMMON_BIT:02X}h            ; restore bank 0 (common on)",
           0x3E, COMMON_BIT)
    a.emit(f"OUT ({BANK_PORT:02X}h),A", 0xD3, BANK_PORT)
    a.emit("XOR A               ; then stock register state", 0xAF)
    a.emit(f"OUT ({BANK_PORT:02X}h),A", 0xD3, BANK_PORT)
    a.emit("LD C,9              ; BDOS print string", 0x0E, 0x09)
    a.emit("CALL 5", 0xCD, 0x05, 0x00)
    a.emit("JP 0                ; warm boot", 0xC3, 0x00, 0x00)

    a.label("okmsg")
    ok = f"APPLICARD 2MB: ALL {BANKS} BANKS OK\r\n$".encode("ascii")
    a.emit("okmsg text", *ok)
    a.label("failmsg")
    fail = b"APPLICARD BANK TEST FAILED\r\n$"
    a.emit("failmsg text", *fail)

    a.resolve()
    for fix, name in ((ok_fix, "okmsg"), (fail_fix, "failmsg")):
        addr = a.labels[name]
        a.code[fix] = addr & 0xFF
        a.code[fix + 1] = addr >> 8
    return a


def main():
    payload = build_payload()

    # Loader at $0100: copy the payload up to PAYLOAD_ORG and jump.
    src = 0x0100 + 14
    loader = bytes([
        0x21, src & 0xFF, src >> 8,                      # LD HL,src
        0x11, PAYLOAD_ORG & 0xFF, PAYLOAD_ORG >> 8,      # LD DE,org
        0x01, len(payload.code) & 0xFF, len(payload.code) >> 8,  # LD BC,len
        0xED, 0xB0,                                      # LDIR
        0xC3, PAYLOAD_ORG & 0xFF, PAYLOAD_ORG >> 8,      # JP org
    ])
    assert len(loader) == 14

    OUT.write_bytes(loader + payload.code)
    OUT.with_suffix(".lst").write_text(
        "\n".join(payload.listing) + "\n", encoding="ascii")
    print(f"wrote {OUT} ({14 + len(payload.code)} bytes)")


if __name__ == "__main__":
    main()
