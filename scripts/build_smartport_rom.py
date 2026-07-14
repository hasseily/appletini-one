from __future__ import annotations

import re
from pathlib import Path
from typing import Literal, overload

from py65.assembler import Assembler
from py65.devices.mpu6502 import MPU


REPO_ROOT = Path(__file__).resolve().parents[1]
SSC_PATH = REPO_ROOT / "6502_SSC.S"
CN00_PATH = REPO_ROOT / "6502_SSC.CN00.S"
SMARTPORT_PATH = REPO_ROOT / "6502_SMARTPORT.S"
CF00_PATH = REPO_ROOT / "6502_SSC.CF00.S"
ASM_OUTPUT_PATH = REPO_ROOT / "hdl" / "apple" / "smartport_a2retronet_style.asm"
SLOT_MEM_PATH = REPO_ROOT / "hdl" / "apple" / "smartport_a2retronet_style_c700.mem"
CONT_MEM_PATH = REPO_ROOT / "hdl" / "apple" / "smartport_a2retronet_style_c800.mem"

SLOT = 7
SSC = 0

BRANCH_OPS = {
    "BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"
}


def opcode_names() -> set[str]:
    mpu = MPU()
    names: set[str] = set()
    for mnemonic, _mode in mpu.disassemble:
        names.add(mnemonic.upper())
    return names


OPCODES = opcode_names()


def strip_comment(line: str) -> str:
    if ";" in line:
        return line.split(";", 1)[0].rstrip()
    return line.rstrip()


def split_items(text: str) -> list[str]:
    items: list[str] = []
    current: list[str] = []
    depth = 0
    quote: str | None = None

    for ch in text:
        if quote is not None:
            current.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in ("'", '"'):
            current.append(ch)
            quote = ch
            continue
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == "," and depth == 0:
            items.append("".join(current).strip())
            current = []
            continue
        current.append(ch)

    tail = "".join(current).strip()
    if tail:
        items.append(tail)
    return items


def eval_expr(expr: str, symbols: dict[str, int], pc: int) -> int:
    expr = expr.strip()
    if not expr:
        raise ValueError("empty expression")

    if expr.startswith("<"):
        return eval_expr(expr[1:], symbols, pc) & 0xFF
    if expr.startswith(">"):
        return (eval_expr(expr[1:], symbols, pc) >> 8) & 0xFF

    expr = re.sub(
        r"(['\"])(.)\1",
        lambda match: str(ord(match.group(2))),
        expr,
    )

    expr = expr.replace("<*", str(pc & 0xFF))
    expr = expr.replace(">*", str((pc >> 8) & 0xFF))

    expr = re.sub(r"(?<![A-Za-z0-9_$])\*(?![A-Za-z0-9_$])", str(pc), expr)
    expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", expr)
    expr = re.sub(r"%([01]+)", r"0b\1", expr)

    def replace_symbol(match: re.Match[str]) -> str:
        name = match.group(0).upper()
        if name in symbols:
            return str(symbols[name])
        return match.group(0)

    expr = re.sub(r"\b[A-Za-z_][A-Za-z0-9_]*\b", replace_symbol, expr)
    return int(eval(expr, {"__builtins__": {}}, {}))


def parse_instruction(line: str) -> tuple[str, str]:
    parts = line.split(None, 1)
    opcode = parts[0].upper()
    operand = parts[1].strip().upper().replace(" ", "") if len(parts) > 1 else ""
    return opcode, operand


def supports_mode(opcode: str, mode: str) -> bool:
    mpu = MPU()
    return (opcode, mode) in mpu.disassemble


def estimate_instruction_size(opcode: str, operand: str, symbols: dict[str, int], pc: int) -> int:
    opcode = opcode.upper()
    operand = operand.strip()

    if not operand or operand.upper() == "A":
        return 1
    if opcode in BRANCH_OPS:
        return 2
    if operand.startswith("#"):
        return 2
    if operand.startswith("("):
        if operand.endswith("),Y") or operand.endswith(",X)"):
            return 2
        return 3
    if opcode in ("JMP", "JSR"):
        return 3

    suffix = ""
    expr = operand
    if operand.endswith(",X") or operand.endswith(",Y"):
        suffix = operand[-2:].upper()
        expr = operand[:-2].strip()

    try:
        value = eval_expr(expr, symbols, pc)
    except Exception:
        value = 0x100

    if value <= 0xFF:
        if suffix == ",X" and supports_mode(opcode, "zpx"):
            return 2
        if suffix == ",Y" and supports_mode(opcode, "zpy"):
            return 2
        if not suffix and supports_mode(opcode, "zpg"):
            return 2

    return 3


def format_operand(opcode: str, operand: str, symbols: dict[str, int], pc: int) -> str:
    operand = operand.strip()
    if not operand:
        return ""
    if operand.upper() == "A":
        return "A"
    if operand.startswith("#"):
        value = eval_expr(operand[1:], symbols, pc) & 0xFF
        return f"#$%02X" % value
    if operand.startswith("(") and operand.endswith("),Y"):
        value = eval_expr(operand[1:-3], symbols, pc) & 0xFFFF
        return f"($%04X),Y" % value
    if operand.startswith("(") and operand.endswith(",X)"):
        value = eval_expr(operand[1:-3], symbols, pc) & 0xFFFF
        return f"($%04X,X)" % value
    if operand.startswith("(") and operand.endswith(")"):
        value = eval_expr(operand[1:-1], symbols, pc) & 0xFFFF
        return f"($%04X)" % value
    if operand.endswith(",X") or operand.endswith(",Y"):
        suffix = operand[-2:].upper()
        value = eval_expr(operand[:-2], symbols, pc) & 0xFFFF
        return f"$%04X%s" % (value, suffix)

    value = eval_expr(operand, symbols, pc) & 0xFFFF
    return f"$%04X" % value


def is_label_line(line: str) -> bool:
    stripped = line.strip()
    if not stripped:
        return False
    if stripped.startswith("."):
        return False
    first = stripped.split(None, 1)[0]
    upper = first.upper()
    if upper in OPCODES:
        return False
    if upper in {"ORG", "DFB", "ASC", "EQU"}:
        return False
    if len(stripped.split(None, 1)) == 1:
        return True
    return True


def convert_line_syntax(line: str) -> str:
    indent = line[:len(line) - len(line.lstrip(" "))]
    stripped = line.strip()
    if not stripped:
        return ""
    if stripped.startswith(";"):
        return line

    # NAME EQU expr -> NAME = expr
    equ_match = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s+EQU\s+(.+)$", stripped, re.IGNORECASE)
    if equ_match:
        return f"{indent}{equ_match.group(1)} = {equ_match.group(2)}"

    if re.match(r"^ORG\b", stripped, re.IGNORECASE):
        return indent + ".org" + stripped[3:]

    if re.match(r"^DFB\b", stripped, re.IGNORECASE):
        return indent + ".byte" + stripped[3:]

    if re.match(r"^ASC\b", stripped, re.IGNORECASE):
        string_match = re.match(r'^ASC\s+"([^"]*)"$', stripped, re.IGNORECASE)
        if not string_match:
            raise ValueError(f"unsupported ASC syntax: {line}")
        byte_items = ", ".join(f"${ord(ch):02X}" for ch in string_match.group(1))
        return indent + ".byte " + byte_items

    if re.match(r"^\.RES\b", stripped, re.IGNORECASE):
        return indent + ".res" + stripped[4:]

    if ":" not in stripped and is_label_line(stripped):
        parts = stripped.split(None, 1)
        label = parts[0]
        if len(parts) == 1:
            return indent + label + ":"
        remainder = convert_line_syntax(parts[1])
        remainder = remainder.strip() if remainder else ""
        return indent + label + ":" + ("" if not remainder else " " + remainder)

    return line


def preprocess_conditional_source(lines: list[str], symbols: dict[str, int]) -> list[str]:
    output: list[str] = []
    cond_stack: list[tuple[bool, bool]] = []
    active = True

    for raw_line in lines:
        line = raw_line.rstrip("\n")
        stripped = strip_comment(line).strip()

        if re.match(r"^\.IF\b", stripped, re.IGNORECASE):
            expr = stripped[3:].strip()
            value = bool(eval_expr(expr, symbols, 0))
            cond_stack.append((active, value))
            active = active and value
            continue

        if re.match(r"^\.ELSE\b", stripped, re.IGNORECASE):
            parent_active, branch_value = cond_stack[-1]
            active = parent_active and (not branch_value)
            cond_stack[-1] = (parent_active, not branch_value)
            continue

        if re.match(r"^\.ENDIF\b", stripped, re.IGNORECASE):
            parent_active, _branch_value = cond_stack.pop()
            active = parent_active
            continue

        if not active:
            continue

        if re.match(r"^\.ASSERT\b", stripped, re.IGNORECASE):
            continue

        converted = convert_line_syntax(line)
        output.append(converted)

    return output


def replace_exact_block(lines: list[str],
                        old_block: list[str],
                        new_block: list[str],
                        description: str) -> list[str]:
    for start in range(0, len(lines) - len(old_block) + 1):
        if lines[start:start + len(old_block)] == old_block:
            return lines[:start] + new_block + lines[start + len(old_block):]
    raise ValueError(f"could not find SmartPort block to patch: {description}")


def apply_appletini_smartport_compat(lines: list[str]) -> list[str]:
    """Preserve Appletini's direct block-driver zero-page ABI."""
    lines = replace_exact_block(
        lines,
        [
            "RDBLK1: ; READ 512-BYTE BLOCK (in-bank; CF00 RDBLOCK jumps here)",
            "        LDY #$00",
            "        JSR RDPAGE",
            "        INC ADDRH",
            "RDPAGE: LDA DATA",
        ],
        [
            "RDBLK1: ; READ 512-BYTE BLOCK (in-bank; CF00 RDBLOCK jumps here)",
            "        LDY #$00",
            "        JSR RDPAGE",
            "        INC ADDRH",
            "        JSR RDPAGE",
            "        DEC ADDRH",
            "        RTS",
            "RDPAGE: LDA DATA",
        ],
        "RDBLK1 ADDRH restore",
    )
    lines = replace_exact_block(
        lines,
        [
            "WRBLOCK: ; WRITE 512-BYTE BLOCK",
            "        LDY #$00",
            "        JSR WRPAGE",
            "        INC ADDRH",
            "WRPAGE: LDA (ADDRL),Y",
        ],
        [
            "WRBLOCK: ; WRITE 512-BYTE BLOCK",
            "        LDY #$00",
            "        JSR WRPAGE",
            "        INC ADDRH",
            "        JSR WRPAGE",
            "        DEC ADDRH",
            "        RTS",
            "WRPAGE: LDA (ADDRL),Y",
        ],
        "WRBLOCK ADDRH restore",
    )
    return lines


def preprocess_reference_source() -> str:
    preamble_lines = SSC_PATH.read_text().splitlines()
    cn00_lines = CN00_PATH.read_text().splitlines()
    smartport_lines = SMARTPORT_PATH.read_text().splitlines()
    cf00_lines = CF00_PATH.read_text().splitlines()

    cond_symbols = {"SLOT": SLOT, "SSC": SSC}

    preamble: list[str] = [
        "; This file is generated by scripts/build_smartport_rom.py",
        "; Source of truth:",
        ";   - 6502_SSC.CN00.S",
        ";   - 6502_SMARTPORT.S",
        ";   - 6502_SSC.CF00.S",
        ";",
        "; The Appletini SmartPort card uses the reference a2retronet SmartPort",
        "; C800/CF00 flow and the real SSC CN00 slot page for slot 7. The only",
        "; local glue below is for unsupported serial/Pascal entry points, which",
        "; are redirected to RTS because Appletini implements the SmartPort path",
        "; only.",
        "",
        f"SLOT = {SLOT}",
        f"SSC = {SSC}",
        "",
    ]

    for line in preamble_lines:
        if line.strip().startswith(".REPEAT"):
            break
        stripped = line.strip()
        if not stripped:
            preamble.append("")
            continue
        if stripped.startswith(".FEATURE") or stripped.startswith(".MACPACK") or stripped.startswith(".DEFINE"):
            continue
        preamble.append(convert_line_syntax(line))

    preamble.extend(
        [
            "",
            "; Unsupported SSC serial/Pascal targets are intentionally stubbed.",
            "BINIT = IORTS",
            "PASCALINIT = IORTS",
            "PASCALREAD = IORTS",
            "PASCALWRITE = IORTS",
            "PENTRY = IORTS",
            "SROUT = IORTS",
            "SRIN = IORTS",
            "INIT1 = IORTS",
            "BINPUT = IORTS",
            "OUTPUT = IORTS",
            "CICEXIT = IORTS",
            "SEROUT = IORTS",
            "",
            "; Real slot-7 CN00 page from the reference SSC source.",
            "",
        ]
    )

    preamble.extend(preprocess_conditional_source(cn00_lines, cond_symbols))
    preamble.extend(
        [
            "",
            "; Reference SmartPort C800 flow.",
            "",
        ]
    )

    smartport_output = preprocess_conditional_source(smartport_lines, cond_symbols)
    preamble.extend(apply_appletini_smartport_compat(smartport_output))

    preamble.extend(
        [
            "",
            "; Reference CF00 common page.",
            "",
        ]
    )

    preamble.extend(preprocess_conditional_source(cf00_lines, cond_symbols))

    return "\n".join(preamble) + "\n"


@overload
def assemble_source(source_text: str,
                    *,
                    include_symbols: Literal[False] = False) -> tuple[bytearray, bytearray]:
    ...


@overload
def assemble_source(source_text: str,
                    *,
                    include_symbols: Literal[True]) -> tuple[bytearray, bytearray, dict[str, int]]:
    ...


def assemble_source(source_text: str,
                    *,
                    include_symbols: bool = False
                    ) -> tuple[bytearray, bytearray] | tuple[bytearray, bytearray, dict[str, int]]:
    lines = source_text.splitlines()
    symbols: dict[str, int] = {}
    pc = 0

    for raw_line in lines:
        line = strip_comment(raw_line).strip()
        if not line:
            continue

        while ":" in line:
            label, rest = line.split(":", 1)
            label = label.strip()
            if not label:
                break
            symbols[label.upper()] = pc
            line = rest.strip()
            if not line:
                break
        if not line:
            continue

        if "=" in line and not line.startswith("."):
            name, expr = line.split("=", 1)
            symbols[name.strip().upper()] = eval_expr(expr, symbols, pc)
            continue

        if line.lower().startswith(".org"):
            pc = eval_expr(line[4:], symbols, pc)
            continue

        if line.lower().startswith(".byte"):
            pc += len(split_items(line[5:].strip()))
            continue

        if line.lower().startswith(".res"):
            pc += eval_expr(line[4:], symbols, pc)
            continue

        opcode, operand = parse_instruction(line)
        pc += estimate_instruction_size(opcode, operand, symbols, pc)

    memory = bytearray(0x10000)
    assembler = Assembler(MPU())
    pc = 0

    for raw_line in lines:
        line = strip_comment(raw_line).strip()
        if not line:
            continue

        while ":" in line:
            _, rest = line.split(":", 1)
            line = rest.strip()
            if not line:
                break
        if not line:
            continue

        if "=" in line and not line.startswith("."):
            continue

        if line.lower().startswith(".org"):
            pc = eval_expr(line[4:], symbols, pc)
            continue

        if line.lower().startswith(".byte"):
            for item in split_items(line[5:].strip()):
                memory[pc] = eval_expr(item, symbols, pc) & 0xFF
                pc += 1
            continue

        if line.lower().startswith(".res"):
            pc += eval_expr(line[4:], symbols, pc)
            continue

        opcode, operand = parse_instruction(line)
        formatted = opcode
        if operand:
            formatted += " " + format_operand(opcode, operand, symbols, pc)
        encoded = assembler.assemble(formatted, pc)
        for byte in encoded:
            memory[pc] = byte
            pc += 1

    slot_data = memory[0xC700:0xC800]
    cont_data = memory[0xC800:0xD000]
    if include_symbols:
        return slot_data, cont_data, symbols
    return slot_data, cont_data


def patch_abs_jsr(slot_data: bytearray, old_addr: int, new_addr: int) -> int:
    old_lo = old_addr & 0xFF
    old_hi = (old_addr >> 8) & 0xFF
    new_lo = new_addr & 0xFF
    new_hi = (new_addr >> 8) & 0xFF
    count = 0

    for i in range(0, len(slot_data) - 2):
        if slot_data[i] == 0x20 and slot_data[i + 1] == old_lo and slot_data[i + 2] == old_hi:
            slot_data[i + 1] = new_lo
            slot_data[i + 2] = new_hi
            count += 1
    return count


def apply_appletini_entries(slot_data: bytearray,
                            symbols: dict[str, int]) -> None:
    """Publish the Appletini slot-7 block-driver ABI.

    Appletini exposes the ProDOS block entry at Cn0A and SmartPort at Cn0D.
    The reference CN00 layout places DISK elsewhere and uses Cn0A as the boot
    BVC displacement. Install entry trampolines and route that BVC through the
    freed setup area so all three entry paths remain valid on an NMOS 6502.
    """
    base = 0xC700
    disk_addr = symbols["DISK"]
    dentry_addr = symbols["DENTRY"]
    bsetup_addr = symbols["BSETUP"]
    dsetup_addr = symbols["DSETUP"]

    disk_off = disk_addr - base
    smartport_off = disk_off + 3
    dentry_off = dentry_addr - base
    bsetup_off = bsetup_addr - base
    dsetup_off = dsetup_addr - base
    relocated_bsetup_off = 0xA0
    boot_trampoline_off = 0x57

    if not (0 <= disk_off <= 0xFF and 0 <= smartport_off <= 0xFF and
            0 <= dentry_off <= 0xFF and 0 <= bsetup_off <= 0xFF and
            0 <= dsetup_off <= 0xFF):
        raise ValueError("SmartPort CN00 labels moved outside the slot page")

    # Expected reference DISK shape:
    #   DISK: CLC / BCC SETUP / SEC / SETUP: JSR DSETUP / JMP COMMON
    if slot_data[disk_off:disk_off + 4] != bytearray([0x18, 0x90, 0x01, 0x38]):
        raise ValueError("unexpected DISK/SmartPort entry shape")
    if slot_data[smartport_off] != 0x38:
        raise ValueError("SmartPort entry must be DISK+3")
    if slot_data[0x09] != 0x50:
        raise ValueError("expected BVC autostart branch at Cn09")

    expected_setup = bytearray([
        0xA0, 0x10 * SLOT,       # BSETUP: LDY #slot*16
        0x84, 0x26,              #         STY SLOT16
        0xA2, 0xC0 + SLOT,       # DSETUP: LDX #$Cn
        0x8E, 0xF8, 0x07,        #         STX MSLOT
        0x8D, 0xFF, 0xCF,        #         STA ROMSOFF
        0x60,                    #         RTS
    ])
    if slot_data[bsetup_off:bsetup_off + len(expected_setup)] != expected_setup:
        raise ValueError("unexpected BSETUP/DSETUP sequence")
    if dsetup_off != bsetup_off + 4:
        raise ValueError("DSETUP must follow BSETUP")

    relocated_dsetup_off = relocated_bsetup_off + (dsetup_off - bsetup_off)
    relocated_bsetup_addr = base + relocated_bsetup_off
    relocated_dsetup_addr = base + relocated_dsetup_off

    if any(slot_data[relocated_bsetup_off:relocated_bsetup_off + len(expected_setup)]):
        raise ValueError("relocated setup area is not empty")

    slot_data[relocated_bsetup_off:relocated_bsetup_off + len(expected_setup)] = expected_setup
    if patch_abs_jsr(slot_data, bsetup_addr, relocated_bsetup_addr) == 0:
        raise ValueError("no BSETUP JSRs retargeted")
    if patch_abs_jsr(slot_data, dsetup_addr, relocated_dsetup_addr) == 0:
        raise ValueError("no DSETUP JSRs retargeted")

    for i in range(bsetup_off, bsetup_off + len(expected_setup)):
        slot_data[i] = 0x00

    # Cn00 autostart executes BVC with the Cn0A byte as the displacement.
    # JMP absolute is opcode $4C, and Cn0B+$4C = Cn57.
    slot_data[0x0A:0x0D] = bytearray([0x4C, disk_off, 0xC7])
    slot_data[0x0D:0x10] = bytearray([0x4C, smartport_off, 0xC7])
    slot_data[boot_trampoline_off:boot_trampoline_off + 3] = bytearray([
        0x4C, dentry_off, 0xC7
    ])
    slot_data[0xFF] = 0x0A

    bvc_target = 0x0B + (slot_data[0x0A] if slot_data[0x0A] < 0x80 else slot_data[0x0A] - 0x100)
    if bvc_target != boot_trampoline_off:
        raise ValueError("Cn0A trampoline broke the boot BVC target")


def write_mem(path: Path, data: bytearray) -> None:
    path.write_text("\n".join(f"{byte:02x}" for byte in data) + "\n")


def main() -> None:
    source_text = preprocess_reference_source()
    ASM_OUTPUT_PATH.write_text(source_text)
    slot_data, cont_data, symbols = assemble_source(source_text, include_symbols=True)
    # The SmartPort variant requires C707=$00; the reference source carries
    # the documented $3C template value.
    slot_data[7] = 0x00
    apply_appletini_entries(slot_data, symbols)
    write_mem(SLOT_MEM_PATH, slot_data)
    write_mem(CONT_MEM_PATH, cont_data)
    print(f"Wrote {ASM_OUTPUT_PATH}")
    print(f"Wrote {SLOT_MEM_PATH}")
    print(f"Wrote {CONT_MEM_PATH}")


if __name__ == "__main__":
    main()
