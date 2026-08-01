#!/usr/bin/env python3
"""Assemble the virtual AppleMouse slot-2 firmware.

Builds hdl/apple/mouse_card_slot2.mem (256 bytes at $C200) from
software/mouse_card_slot2.a65 with a small two-pass assembler in the style
of build_boot_menu_rom.py. The page layout is composed here:

  $00-$02  slotregs helper (ldx #$02 / rts)
  $05-$06  AppleMouse ID byte $38 doubling as the shared sec/rts stub
  $07      AppleMouse ID byte $18 (clc)
  $0B-$0C  AppleMouse ID bytes $01 $20
  $0D-$11  stcmd stub (sta R_CMD / clc / rts)
  $12-$1A  entry vectors: SETMOUSE SERVEMOUSE READMOUSE CLEARMOUSE
           POSMOUSE CLAMPMOUSE HOMEMOUSE INITMOUSE READCLAMP
  $1B-$FA  main code (assembled from the .a65)
  $FB      AppleMouse ID byte $D6
  filler   $EA

The build fails loudly on region overflow, branch range violations, or a
collision with any pinned byte.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_PATH = REPO_ROOT / "software" / "mouse_card_slot2.a65"
MEM_PATH = REPO_ROOT / "hdl" / "apple" / "mouse_card_slot2.mem"

BASE = 0xC200
CODE_ORG = BASE + 0x1B
CODE_END = BASE + 0xFB          # exclusive: $FB holds the $D6 ID byte

PINNED = {
    0x00: 0xA2, 0x01: 0x02, 0x02: 0x60,          # slotregs: ldx #$02 / rts
    0x05: 0x38, 0x06: 0x60,                      # ID $38 == sec; + rts stub
    0x07: 0x18,                                  # ID $18 (clc)
    0x0B: 0x01, 0x0C: 0x20,                      # ID bytes
    0x0D: 0x8D, 0x0E: 0xAC, 0x0F: 0xC0,          # stcmd: sta $C0AC
    0x10: 0x18, 0x11: 0x60,                      # clc / rts
    0xFB: 0xD6,                                  # AppleMouse ID byte
}

PINNED_SYMBOLS = {
    "slotregs": BASE + 0x00,
    "secrts":   BASE + 0x05,
    "stcmd":    BASE + 0x0D,
}

VECTOR_ORDER = [
    "setmouse", "servemouse", "readmouse", "clearmouse",
    "posmouse", "clampmouse", "homemouse", "initmouse", "readclamp",
]

BRANCH_OPS = {"BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"}

OPCODES = {
    ("AND", "imm"): 0x29,
    ("BCS", "rel"): 0xB0,
    ("BEQ", "rel"): 0xF0,
    ("BNE", "rel"): 0xD0,
    ("CLC", "imp"): 0x18,
    ("CMP", "imm"): 0xC9,
    ("EOR", "imm"): 0x49,
    ("JMP", "abs"): 0x4C,
    ("JSR", "abs"): 0x20,
    ("LDA", "imm"): 0xA9,
    ("LDA", "abs"): 0xAD,
    ("LDA", "absx"): 0xBD,
    ("LDX", "imm"): 0xA2,
    ("LSR", "imp"): 0x4A,
    ("PHA", "imp"): 0x48,
    ("PLA", "imp"): 0x68,
    ("RTS", "imp"): 0x60,
    ("SBC", "imm"): 0xE9,
    ("SEC", "imp"): 0x38,
    ("STA", "abs"): 0x8D,
    ("STA", "absx"): 0x9D,
    ("TAX", "imp"): 0xAA,
}


class AsmError(RuntimeError):
    pass


def parse_lines(text: str):
    for raw_num, raw in enumerate(text.splitlines(), start=1):
        line = raw.split(";", 1)[0].rstrip()
        if not line.strip():
            continue
        yield raw_num, line


def resolve(value: str, symbols: dict, line_num: int) -> int:
    value = value.strip()
    if value.startswith("$"):
        return int(value[1:], 16)
    if re.fullmatch(r"\d+", value):
        return int(value, 10)
    if value in symbols:
        return symbols[value]
    raise AsmError(f"line {line_num}: unresolved symbol '{value}'")


def assemble(text: str):
    """Two-pass assembly of the main code region. Returns (code, symbols)."""
    symbols = dict(PINNED_SYMBOLS)
    equates = {}
    statements = []

    for line_num, line in parse_lines(text):
        eq = re.fullmatch(r"\s*(\w+)\s*=\s*(\$[0-9A-Fa-f]+|\d+)\s*", line)
        if eq:
            if eq.group(1) in equates:
                raise AsmError(f"line {line_num}: duplicate equate {eq.group(1)}")
            equates[eq.group(1)] = resolve(eq.group(2), {}, line_num)
            continue
        label = re.match(r"^(\w+):\s*(.*)$", line)
        if label:
            statements.append((line_num, "label", label.group(1)))
            rest = label.group(2).strip()
            if rest:
                statements.append((line_num, "op", rest))
            continue
        statements.append((line_num, "op", line.strip()))

    symbols.update(equates)

    def statement_size(line_num: int, op_text: str) -> int:
        parts = op_text.split(None, 1)
        mnem = parts[0].upper()
        if len(parts) == 1:
            return 1
        if mnem in BRANCH_OPS:
            return 2
        if parts[1].startswith("#"):
            return 2
        return 3

    # Pass 1: label addresses.
    pc = CODE_ORG
    for line_num, kind, payload in statements:
        if kind == "label":
            if payload in symbols:
                raise AsmError(f"line {line_num}: duplicate symbol {payload}")
            symbols[payload] = pc
        else:
            pc += statement_size(line_num, payload)
    if pc > CODE_END:
        raise AsmError(
            f"code region overflow: ends at ${pc:04X}, limit ${CODE_END:04X} "
            f"({pc - CODE_END} bytes over)")

    # Pass 2: emission.
    code = bytearray()
    pc = CODE_ORG
    for line_num, kind, payload in statements:
        if kind == "label":
            continue
        parts = payload.split(None, 1)
        mnem = parts[0].upper()
        if len(parts) == 1:
            key = (mnem, "imp")
            if key not in OPCODES:
                raise AsmError(f"line {line_num}: unsupported op {payload}")
            code.append(OPCODES[key])
            pc += 1
            continue
        operand = parts[1].strip()
        if mnem in BRANCH_OPS:
            target = resolve(operand, symbols, line_num)
            rel = target - (pc + 2)
            if rel < -128 or rel > 127:
                raise AsmError(
                    f"line {line_num}: branch {mnem} to {operand} out of "
                    f"range ({rel})")
            code.append(OPCODES[(mnem, "rel")])
            code.append(rel & 0xFF)
            pc += 2
            continue
        if operand.startswith("#"):
            value = resolve(operand[1:], symbols, line_num)
            if value > 0xFF:
                raise AsmError(f"line {line_num}: immediate > $FF")
            key = (mnem, "imm")
            if key not in OPCODES:
                raise AsmError(f"line {line_num}: unsupported op {payload}")
            code.append(OPCODES[key])
            code.append(value)
            pc += 2
            continue
        mode = "abs"
        if operand.lower().endswith(",x"):
            mode = "absx"
            operand = operand[:-2].strip()
        value = resolve(operand, symbols, line_num)
        key = (mnem, mode)
        if key not in OPCODES:
            raise AsmError(f"line {line_num}: unsupported op {payload}")
        code.append(OPCODES[key])
        code.append(value & 0xFF)
        code.append((value >> 8) & 0xFF)
        pc += 3
    return bytes(code), symbols


def compose(code: bytes, symbols: dict) -> bytes:
    page = bytearray([0xEA] * 256)
    for off, val in PINNED.items():
        page[off] = val
    for i, name in enumerate(VECTOR_ORDER):
        if name not in symbols:
            raise AsmError(f"vector routine '{name}' not defined")
        addr = symbols[name]
        if not (BASE <= addr <= BASE + 0xFF):
            raise AsmError(f"vector {name} outside page: ${addr:04X}")
        page[0x12 + i] = addr & 0xFF
    for i, byte in enumerate(code):
        off = (CODE_ORG - BASE) + i
        if off in PINNED:
            raise AsmError(f"code collides with pinned byte at ${off:02X}")
        if off >= 0xFB:
            raise AsmError("code overran the $FB ID byte")
        page[off] = byte
    return bytes(page)


def emit_mem(page: bytes) -> str:
    lines = [
        "// Generated by scripts/build_mouse_rom.py from",
        "// software/mouse_card_slot2.a65 -- do not edit by hand.",
    ]
    for i in range(0, 256, 16):
        lines.append(" ".join(f"{b:02x}" for b in page[i:i + 16]))
    return "\n".join(lines) + "\n"


def build() -> bytes:
    code, symbols = assemble(SRC_PATH.read_text(encoding="utf-8"))
    page = compose(code, symbols)
    free = (CODE_END - CODE_ORG) - len(code)
    print(f"mouse ROM: code {len(code)} bytes, {free} spare before $FB")
    for name in VECTOR_ORDER:
        print(f"  {name:<11} ${symbols[name]:04X}")
    return page


def main() -> int:
    page = build()
    MEM_PATH.write_text(emit_mem(page), encoding="utf-8")
    print(f"wrote {MEM_PATH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
