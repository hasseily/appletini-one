from __future__ import annotations

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
SRC_PATH = REPO_ROOT / "software" / "boot_menu_slot7.a65"
MEM_PATH = REPO_ROOT / "hdl" / "apple" / "boot_menu_slot7.mem"
RELOC_PATH = REPO_ROOT / "hdl" / "apple" / "boot_menu_slot7_reloc.mem"
C8_MEM_PATH = REPO_ROOT / "hdl" / "apple" / "boot_menu_slot7_c8.mem"
PATCH_HEADER_PATH = REPO_ROOT / "ps_sources" / "frontend" / "boot_menu_rom_patch.h"

# Expansion ROM page at $C800-$C8FF. The boot_menu_card hardware only
# decodes this single page as ROM; $CA00-$CA0F is a separate 16-byte
# RAM register file the firmware uses for scratch (see
# boot_menu_slot7.a65). Code/data emitted into $C800-$C8FF must stay
# within this 256-byte block.
C8_BYTES = 0x100
C8_BASE = 0xC800

NTSC_DELAY_X = 0x1D
NTSC_DELAY_Y = 0x74
PAL_DELAY_X = 0x54
PAL_DELAY_Y = 0x2F
IIPLUS_VAPOR_DELAY_X = 0x02
IIPLUS_VAPOR_DELAY_Y = 0x04
NTSC_DELAY_BODY_CYCLES = 17014
PAL_DELAY_BODY_CYCLES = 20264

BRANCH_OPS = {"BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"}

OPCODES = {
    ("AND", "imm"): 0x29,
    ("ASL", "imp"): 0x0A,
    ("BCC", "rel"): 0x90,
    ("BCS", "rel"): 0xB0,
    ("BEQ", "rel"): 0xF0,
    ("BNE", "rel"): 0xD0,
    ("BPL", "rel"): 0x10,
    ("BVC", "rel"): 0x50,
    ("CLV", "imp"): 0xB8,
    ("SEC", "imp"): 0x38,
    ("CMP", "imm"): 0xC9,
    ("CPY", "imm"): 0xC0,
    ("INY", "imp"): 0xC8,
    ("INX", "imp"): 0xE8,
    ("JMP", "abs"): 0x4C,
    ("JSR", "abs"): 0x20,
    ("LDA", "imm"): 0xA9,
    ("LDA", "abs"): 0xAD,
    ("LDA", "zp"):  0xA5,
    ("LDA", "absy"): 0xB9,
    ("LDY", "imm"): 0xA0,
    ("LDY", "abs"): 0xAC,
    ("LDY", "zp"):  0xA4,
    ("LDX", "imm"): 0xA2,
    ("LDX", "abs"): 0xAE,
    ("LDX", "zp"):  0xA6,
    ("DEX", "imp"): 0xCA,
    ("DEY", "imp"): 0x88,
    ("PHA", "imp"): 0x48,
    ("PLA", "imp"): 0x68,
    ("NOP", "imp"): 0xEA,
    ("RTS", "imp"): 0x60,
    ("STA", "abs"): 0x8D,
    ("STA", "zp"):  0x85,
    ("STA", "absy"): 0x99,
    ("STX", "abs"): 0x8E,
    ("STX", "zp"):  0x86,
    ("STY", "abs"): 0x8C,
    ("STY", "zp"):  0x84,
    ("TAX", "imp"): 0xAA,
    ("TAY", "imp"): 0xA8,
    ("TXA", "imp"): 0x8A,
    ("TYA", "imp"): 0x98,
}

# Instructions that have a zero-page form. JSR, JMP, branches don't, and
# anything that takes a Y/X-indexed addr is excluded too (we don't have
# zp,y/zp,x forms in OPCODES yet).
ZP_OPS = {"LDA", "LDY", "LDX", "STA", "STX", "STY"}


def strip_comment(line: str) -> str:
    return line.split(";", 1)[0].strip()


def split_items(text: str) -> list[str]:
    items: list[str] = []
    cur: list[str] = []
    quote: str | None = None
    for ch in text:
        if quote is not None:
            cur.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in ("'", '"'):
            quote = ch
            cur.append(ch)
            continue
        if ch == ",":
            items.append("".join(cur).strip())
            cur = []
            continue
        cur.append(ch)
    tail = "".join(cur).strip()
    if tail:
        items.append(tail)
    return items


def eval_expr(expr: str, symbols: dict[str, int], pc: int) -> int:
    expr = expr.strip()
    if expr.startswith("#"):
        expr = expr[1:].strip()
    if expr.startswith("<"):
        return eval_expr(expr[1:], symbols, pc) & 0xFF
    if expr.startswith(">"):
        return (eval_expr(expr[1:], symbols, pc) >> 8) & 0xFF

    expr = re.sub(r"(['\"])(.)\1", lambda m: str(ord(m.group(2))), expr)
    expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", expr)
    expr = re.sub(r"\b[A-Za-z_][A-Za-z0-9_]*\b",
                  lambda m: str(symbols[m.group(0).upper()]),
                  expr)
    expr = expr.replace("*", str(pc))
    return int(eval(expr, {"__builtins__": {}}, {}))


def parse_source() -> list[str]:
    lines: list[str] = []
    for raw in SRC_PATH.read_text().splitlines():
        line = strip_comment(raw)
        if line:
            lines.append(line)
    return lines


def is_assignment(line: str) -> bool:
    return "=" in line and not line.startswith("*") and not line.startswith(".")


def parse_instruction(line: str) -> tuple[str, str]:
    parts = line.split(None, 1)
    op = parts[0].upper()
    operand = parts[1].strip() if len(parts) > 1 else ""
    return op, operand


def try_resolve(operand: str, symbols: dict[str, int]) -> int | None:
    """Try to evaluate an operand using only already-known symbols.
    Returns None if any symbol is unresolved (forward reference)."""
    expr = operand.strip()
    if expr.startswith("#"):
        return None  # immediates aren't subject to zp-vs-abs sizing
    expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", expr)
    try:
        names = re.findall(r"\b[A-Za-z_][A-Za-z0-9_]*\b", expr)
        for name in names:
            if name.upper() not in symbols:
                return None
        for name in names:
            expr = re.sub(rf"\b{re.escape(name)}\b", str(symbols[name.upper()]), expr)
        return int(eval(expr, {"__builtins__": {}}, {}))
    except Exception:
        return None


def instruction_mode(op: str, operand: str,
                     symbols: dict[str, int] | None = None) -> str:
    if op in BRANCH_OPS:
        return "rel"
    if not operand or operand.upper() == "A":
        return "imp"
    if operand.startswith("#"):
        return "imm"
    if operand.upper().endswith(",Y"):
        return "absy"
    # Zero-page form picked when (a) op has a zp-mode opcode and (b) the
    # operand resolves to < $100 from already-known symbols. Forward
    # references (unresolved symbols) fall back to abs to keep pass-1
    # size estimates consistent with pass-2 emission.
    if op in ZP_OPS and symbols is not None:
        val = try_resolve(operand, symbols)
        if val is not None and 0 <= val < 0x100:
            return "zp"
    return "abs"


def instruction_size(op: str, operand: str,
                     symbols: dict[str, int] | None = None) -> int:
    mode = instruction_mode(op, operand, symbols)
    if mode == "imp":
        return 1
    if mode in {"imm", "rel", "zp"}:
        return 2
    return 3


def collect_symbols(lines: list[str]) -> dict[str, int]:
    symbols: dict[str, int] = {}
    pc = 0

    for line in lines:
        if is_assignment(line):
            name, expr = line.split("=", 1)
            symbols[name.strip().upper()] = eval_expr(expr, symbols, pc)
            continue
        if line.startswith("*"):
            _, expr = line.split("=", 1)
            pc = eval_expr(expr, symbols, pc)
            continue
        if line.startswith(".byte"):
            pc += len(split_items(line[5:].strip()))
            continue

        first = line.split(None, 1)[0]
        if first.upper() not in {op for op, _mode in OPCODES} and first.upper() not in BRANCH_OPS:
            symbols[first.upper().rstrip(":")] = pc
            rest = line[len(first):].strip()
            if not rest:
                continue
            line = rest

        op, operand = parse_instruction(line)
        pc += instruction_size(op, operand, symbols)

    return symbols


def emit_instruction(data: bytearray,
                     reloc: bytearray,
                     pc: int,
                     op: str,
                     operand: str,
                     symbols: dict[str, int]) -> int:
    mode = instruction_mode(op, operand, symbols)
    opcode = OPCODES[(op, mode)]
    data[pc] = opcode
    pc += 1

    if mode == "imp":
        return pc
    if mode == "imm":
        data[pc] = eval_expr(operand[1:], symbols, pc) & 0xFF
        return pc + 1
    if mode == "rel":
        target = eval_expr(operand, symbols, pc)
        offset = target - (pc + 1)
        if offset < -128 or offset > 127:
            raise ValueError(f"branch out of range: {op} {operand}")
        data[pc] = offset & 0xFF
        return pc + 1
    if mode == "zp":
        addr = eval_expr(operand, symbols, pc)
        data[pc] = addr & 0xFF
        return pc + 1

    expr = operand[:-2] if mode == "absy" else operand
    addr = eval_expr(expr, symbols, pc)
    data[pc] = addr & 0xFF
    data[pc + 1] = (addr >> 8) & 0xFF
    if 0xC700 <= addr <= 0xC7FF:
        reloc[pc + 1] = 1
    return pc + 2


def build() -> tuple[bytearray, bytearray, bytearray, dict[str, int]]:
    lines = parse_source()
    symbols = collect_symbols(lines)
    memory = bytearray([0xEA] * 0x10000)
    reloc = bytearray([0x00] * 0x10000)
    # Track what's been emitted to which region so we can flag overflow
    # for each independently.
    slot_max_pc = 0
    c8_max_pc = 0
    pc = 0

    def bump(new_pc: int) -> None:
        nonlocal slot_max_pc, c8_max_pc
        if 0xC700 <= new_pc <= 0xC800:
            slot_max_pc = max(slot_max_pc, new_pc)
        elif 0xC800 <= new_pc <= 0xC900:
            c8_max_pc = max(c8_max_pc, new_pc)

    for line in lines:
        if is_assignment(line):
            continue
        if line.startswith("*"):
            _, expr = line.split("=", 1)
            pc = eval_expr(expr, symbols, pc)
            continue
        if line.startswith(".byte"):
            for item in split_items(line[5:].strip()):
                memory[pc] = eval_expr(item, symbols, pc) & 0xFF
                pc += 1
                bump(pc)
            continue

        first = line.split(None, 1)[0]
        if first.upper() not in {op for op, _mode in OPCODES} and first.upper() not in BRANCH_OPS:
            rest = line[len(first):].strip()
            if not rest:
                continue
            line = rest

        op, operand = parse_instruction(line)
        pc = emit_instruction(memory, reloc, pc, op, operand, symbols)
        bump(pc)

    if slot_max_pc > 0xC800:
        raise ValueError(f"slot ROM exceeds 256 bytes by {slot_max_pc - 0xC800} bytes")
    if c8_max_pc > 0xC900:
        raise ValueError(f"expansion ROM page exceeds $C800-$C8FF by {c8_max_pc - 0xC900} bytes")

    slot = memory[0xC700:0xC800]
    slot_reloc = reloc[0xC700:0xC800]
    if len(slot) != 256:
        raise ValueError("slot ROM must be 256 bytes")
    slot[-1] = 0x00
    slot_reloc[-1] = 0x00

    c8 = memory[C8_BASE:C8_BASE + C8_BYTES]
    if len(c8) != C8_BYTES:
        raise ValueError("expansion ROM must be 256 bytes")

    return slot, slot_reloc, c8, symbols


def write_mem(path: Path, data: bytes | bytearray) -> None:
    """Write a .mem file with LF-only line endings (matching the
    repo's existing committed format, regardless of host OS)."""
    body = "".join(f"{byte:02x}\n" for byte in data)
    path.write_bytes(body.encode("ascii"))


def write_patch_header(path: Path, symbols: dict[str, int]) -> None:
    x_opcode = symbols["DELAY_FRAME_X_OPCODE"]
    y_opcode = symbols["DELAY_FRAME_Y_OPCODE"]
    iiplus_x_opcode = symbols["IIPLUS_VAPOR_X_OPCODE"]
    iiplus_y_opcode = symbols["IIPLUS_VAPOR_Y_OPCODE"]
    x_offset = x_opcode - C8_BASE + 1
    y_offset = y_opcode - C8_BASE + 1
    iiplus_x_offset = iiplus_x_opcode - C8_BASE + 1
    iiplus_y_offset = iiplus_y_opcode - C8_BASE + 1
    for name, offset in (
        ("delay X immediate", x_offset),
        ("delay Y immediate", y_offset),
        ("II+ vaporlock X immediate", iiplus_x_offset),
        ("II+ vaporlock Y immediate", iiplus_y_offset),
    ):
        if offset < 0 or offset >= C8_BYTES:
            raise ValueError(f"{name} patch offset is outside the C8 ROM page")

    body = f"""#ifndef BOOT_MENU_ROM_PATCH_H
#define BOOT_MENU_ROM_PATCH_H

#include <stdint.h>

/* Generated by scripts/build_boot_menu_rom.py from software/boot_menu_slot7.a65. */
#define BOOT_MENU_C8_PATCH_DELAY_X_OFFSET       0x{x_offset:02X}U
#define BOOT_MENU_C8_PATCH_DELAY_Y_OFFSET       0x{y_offset:02X}U
#define BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_X_OFFSET 0x{iiplus_x_offset:02X}U
#define BOOT_MENU_C8_PATCH_IIPLUS_VAPOR_Y_OFFSET 0x{iiplus_y_offset:02X}U

#define BOOT_MENU_C8_DELAY_NTSC_X               0x{NTSC_DELAY_X:02X}U
#define BOOT_MENU_C8_DELAY_NTSC_Y               0x{NTSC_DELAY_Y:02X}U
#define BOOT_MENU_C8_DELAY_PAL_X                0x{PAL_DELAY_X:02X}U
#define BOOT_MENU_C8_DELAY_PAL_Y                0x{PAL_DELAY_Y:02X}U
#define BOOT_MENU_C8_IIPLUS_VAPOR_X             0x{IIPLUS_VAPOR_DELAY_X:02X}U
#define BOOT_MENU_C8_IIPLUS_VAPOR_Y             0x{IIPLUS_VAPOR_DELAY_Y:02X}U

#define BOOT_MENU_C8_DELAY_NTSC_BODY_CYCLES     {NTSC_DELAY_BODY_CYCLES}U
#define BOOT_MENU_C8_DELAY_PAL_BODY_CYCLES      {PAL_DELAY_BODY_CYCLES}U

#endif /* BOOT_MENU_ROM_PATCH_H */
"""
    path.write_bytes(body.encode("ascii"))


def main() -> None:
    slot, reloc, c8, symbols = build()
    write_mem(MEM_PATH, slot)
    write_mem(RELOC_PATH, reloc)
    write_mem(C8_MEM_PATH, c8)
    write_patch_header(PATCH_HEADER_PATH, symbols)
    print(f"Wrote {MEM_PATH}")
    print(f"Wrote {RELOC_PATH}")
    print(f"Wrote {C8_MEM_PATH}")
    print(f"Wrote {PATCH_HEADER_PATH}")


if __name__ == "__main__":
    main()
