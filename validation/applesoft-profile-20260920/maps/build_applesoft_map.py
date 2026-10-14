"""Build a ROM-pinned, exclusive PC category map; uses only Python's stdlib.

Run from any directory. Inputs are the saved public disassembly beside this
script and docs/Apple2e_Enhanced.rom. All outputs stay beside this script.
The public disassembly is for Apple II+, so Enhanced changes are explicit below.
"""

from __future__ import annotations

import hashlib
import html
import json
from pathlib import Path
import re


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
ROM_PATH = ROOT / "docs" / "Apple2e_Enhanced.rom"
SOURCE_PATH = HERE / "Applesoft.source.html"
ROM_SHA256 = "aab38a03ca8deabbb2f868733148c2efd6f655a59cd9c5d058ef3e0b7aa86a1a"
SOURCE_SHA256 = "4c794cabc4ad63e688d13551f48d4cb7706b12cf94b5a7472820b8961b59d192"
SOURCE_URL = "https://6502disassembly.com/a2-rom/Applesoft.html"

P = "parser/dispatch"
V = "variable/array lookup"
L = "loops/control"
A = "arithmetic"
I = "literals/number input"
O = "number output"
S = "strings/GC"
G = "graphics"
M = "monitor/IO"
U = "DOS/user"

# Each boundary starts one half-open interval and ends the preceding interval.
# These are PC ownership regions, not inclusive caller/callee cost categories.
BOUNDARIES = [
    (0x0000, U, "RAM / application / DOS"),
    (0x00B1, P, "RAM CHRGET / CHRGOT; require stock-code guard"),
    (0x00C9, U, "RAM / application / DOS"),
    (0xC000, M, "I/O and slot firmware address region"),
    (0xD000, P, "Applesoft dispatch tables, tokens, error text (mostly data)"),
    (0xD365, L, "GTFORPNT: locate FOR/GOSUB stack frame"),
    (0xD393, V, "BLTU: shared memory relocation helper"),
    (0xD3D6, P, "CHKMEM: expression/statement stack room check"),
    (0xD3E3, S, "REASON: storage space check and GC invocation"),
    (0xD410, P, "Errors, command input dispatch, program editing/link repair"),
    (0xD52C, M, "INLIN / INCHR: input front end"),
    (0xD559, P, "PARSE_INPUT_LINE: tokenize entered text"),
    (0xD61A, L, "FNDLIN / FL1: linked line search"),
    (0xD649, P, "NEW / CLEAR / interpreter reset"),
    (0xD6A5, M, "LIST and token/line display"),
    (0xD766, L, "FOR / STEP setup"),
    (0xD7D2, P, "NEWSTT / TRACE / statement dispatch"),
    (0xD849, L, "RESTORE / DATA pointer reset"),
    (0xD858, M, "ISCNTC: keyboard polling (LDA $C000)"),
    (0xD863, L, "Ctrl-C / STOP / END / CONT"),
    (0xD8B0, M, "SAVE / LOAD / VARTIO / PROGIO"),
    (0xD912, L, "RUN / GOSUB / GOTO / RETURN / DATA / IF / ON"),
    (0xDA0C, I, "LINGET: decimal line-number conversion"),
    (0xDA46, V, "LET / LET2: assignment and numeric variable store"),
    (0xDA7A, S, "LET_STRING / PUTSTR: string assignment"),
    (0xDACF, M, "PRINT and text formatting/output wrappers"),
    (0xDB71, I, "INPUT / GET / READ orchestration and input errors"),
    (0xDCF9, L, "NEXT: loop frame update and continuation"),
    (0xDD67, P, "FRMNUM / type checks / FRMEVL / expression parser"),
    (0xDED5, V, "FRM_VARIABLE: resolve/load variable (including int-to-FP)"),
    (0xDEF9, G, "SCRN expression front end"),
    (0xDF0C, P, "UNARY function argument parsing and dispatch"),
    (0xDF4F, A, "OR / AND / numeric relations"),
    (0xDF7D, S, "String comparison (includes shared boolean result tail)"),
    (0xDFCD, M, "PDL: paddle input front end"),
    (0xDFD6, V, "DIM / PTRGET and scalar name processing"),
    (0xE000, P, "DOS/monitor BASIC entry vectors E000/E003"),
    (0xE007, V, "Scalar lookup/allocation and array descriptor setup"),
    (0xE102, A, "MAKINT / MKINT / AYINT: integer range/conversion"),
    (0xE11E, V, "ARRAY lookup/allocation/indexing; includes integer multiplier"),
    (0xE2DE, S, "FRE: explicit garbage collection and free-space query"),
    (0xE2F2, A, "GIVAYF: signed integer to floating point"),
    (0xE2FF, M, "POS: cursor-column query"),
    (0xE301, A, "SNGFLT: byte to floating point"),
    (0xE306, P, "DEF FN / FN dispatch and function environment"),
    (0xE3C5, O, "STR$: number-to-string front end"),
    (0xE3D5, S, "String allocation, literals, GC, concatenate, slice, LEN/ASC"),
    (0xE6F5, A, "GTBYTC / GETBYT / CONINT: byte argument conversion"),
    (0xE707, I, "VAL and temporary string parsing pointers"),
    (0xE746, A, "GTNUM / COMBYTE / GETADR: address argument conversion"),
    (0xE764, M, "PEEK / POKE / WAIT front ends"),
    (0xE7A0, A, "Floating math, normalization, transfer, compare, integer conversion"),
    (0xEC4A, I, "FIN: decimal text to FAC (includes ADDACC helper)"),
    (0xED0A, O, "FOUT constants / line and FAC number formatting"),
    (0xEE8D, A, "SQR / power / EXP / polynomial / RND / trigonometry"),
    (0xF10B, P, "CHRGET template / seed data / cold start"),
    (0xF1D5, M, "CALL / IN# / PR# wrappers"),
    (0xF1EC, G, "Low-resolution graphics command front ends"),
    (0xF256, M, "VTAB / SPEED text controls"),
    (0xF26D, L, "TRACE / NOTRACE"),
    (0xF273, M, "NORMAL / INVERSE / FLASH"),
    (0xF286, V, "HIMEM / LOMEM storage limits"),
    (0xF2CB, L, "ONERR / HANDLERR / RESUME"),
    (0xF331, P, "DEL: program editing"),
    (0xF390, G, "GR / TEXT display modes"),
    (0xF39F, M, "STORE / RECALL"),
    (0xF3D8, G, "HGR / HPLOT / HGLIN / DRAW / XDRAW"),
    (0xF775, M, "Enhanced SHLOAD/STORE/RECALL firmware shim"),
    (0xF787, P, "Enhanced tokenizer case-fold helpers"),
    (0xF7AA, O, "Enhanced LIST line-number prefix wrapper"),
    (0xF7B4, M, "Enhanced 40/80-column text-position helpers"),
    (0xF7D9, V, "GETARYPT: STORE/RECALL array lookup wrapper"),
    (0xF7E7, M, "Enhanced HTAB"),
    (0xF800, M, "Monitor ROM; includes low-res graphics and screen output"),
    (0x10000, U, "end"),
]

ENTRY_NAMES = """
GTFORPNT BLTU BLTU2 CHKMEM REASON ERROR RESTART NUMBERED_LINE PUT_NEW_LINE
FIX_LINKS INLIN INCHR PARSE_INPUT_LINE FNDLIN FL1 NEW CLEAR CLEARC LIST
FOR STEP NEWSTT TRACE_ EXECUTE_STATEMENT RESTORE ISCNTC STOP END CONT
SAVE LOAD RUN GOSUB GOTO RETURN DATA DATAN REMN IF REM ONGOTO LINGET
LET LET2 LET_STRING PRINT CRDO STRPRT OUTDO GET INPUT READ PROCESS_INPUT_ITEM
FINDATA NEXT NEXT_1 FRMNUM FRMEVL FRM_ELEMENT FRM_VARIABLE UNARY OR AND
RELOPS STRCMP PDL DIM PTRGET PTRGET2 PTRGET3 ISLETC NAME_NOT_FOUND
MAKE_NEW_VARIABLE SET_VARPNT_AND_YA GETARY MAKINT MKINT AYINT ARRAY
USE_OLD_ARRAY MAKE_NEW_ARRAY FIND_ARRAY_ELEMENT MULTIPLY_SUBSCRIPT
MULTIPLY_SUBS_1 FRE GIVAYF POS SNGFLT DEF FUNCT STR STRINI STRSPA STRLIT
GETSPA GARBAG FIND_HIGHEST_STRING MOVE_HIGHEST_STRING_TO_TOP CAT MOVSTR
FREFAC FRETMP CHRSTR LEFTSTR RIGHTSTR MIDSTR LEN ASC GETBYT CONINT VAL
GETADR PEEK POKE WAIT FADDH FSUB FSUBT FADD FADDT NORMALIZE_FAC_1
ZERO_FAC OVERFLOW LOG FMULT FMULTT MULTIPLY_1 LOAD_ARG_FROM_YA MUL10
DIV10 DIV FDIV FDIVT LOAD_FAC_FROM_YA STORE_FAC_AT_YX_ROUNDED
COPY_ARG_TO_FAC COPY_FAC_TO_ARG_ROUNDED ROUND_FAC SGN FLOAT ABS FCOMP
FCOMP2 QINT INT FIN ADDACC GETEXP INPRT LINPRT PRINT_FAC FOUT SQR FPWRT
NEGOP EXP POLYNOMIAL RND COS SIN TAN ATN GENERIC_CHRGET COLD_START CALL
IN_NUMBER PR_NUMBER PLOT HLIN VLIN COLOR VTAB SPEED TRACE NOTRACE
HIMEM LOMEM ONERR HANDLERR RESUME DEL GR TEXT STORE RECALL HGR2 HGR
BKGND HPOSN HPLOT0 HGLIN DRAW1 XDRAW1 HCOLOR HPLOT DRAW XDRAW GETARYPT HTAB
""".split()

EXTRA_ENTRIES = [
    (0x00B1, "RAM_CHRGET", 0xF10B),
    (0x00B7, "RAM_CHRGOT", 0xF111),
    (0xD43F, "BASIC_PROMPT", None),
    (0xE053, "SCALAR_LOOKUP_LOOP_START", None),
    (0xE169, "ARRAY_LOOKUP_START", None),
    (0xF775, "ENHANCED_SHLOAD", None),
    (0xF777, "ENHANCED_STORAGE_SHIM", None),
    (0xF787, "ENHANCED_TOKEN_LOOKAHEAD", None),
    (0xF78C, "ENHANCED_TOKEN_CHAR", None),
    (0xF79A, "ENHANCED_UPPERCASE_CHAR", None),
    (0xF7AA, "ENHANCED_LIST_LINPRT", None),
    (0xF7B4, "ENHANCED_COLUMN_CHECK", None),
    (0xF7C3, "ENHANCED_TAB_SPACES", None),
    (0xF7CA, "ENHANCED_HTAB_HELPER", None),
    (0xFDED, "MON_COUT", None),
    (0xFD0C, "MON_RDKEY", None),
    (0xFD18, "MON_RDKEY_INDIRECT", None),
    (0xFC58, "MON_HOME", None),
    (0xFCA8, "MON_WAIT", None),
    (0xFB1E, "MON_PREAD", None),
]


def main() -> None:
    rom = ROM_PATH.read_bytes()
    source = SOURCE_PATH.read_bytes()
    assert len(rom) == 0x4000, "Expected 16 KiB C000-FFFF ROM"
    assert hashlib.sha256(rom).hexdigest() == ROM_SHA256, "Unsupported ROM"
    assert hashlib.sha256(source).hexdigest() == SOURCE_SHA256, "Source snapshot changed"
    text = html.unescape(re.sub(r"<[^>]+>", "", source.decode("utf-8")))
    labels = {}
    reference = {}
    row_bytes = {}
    for line in text.splitlines():
        if not re.match(r"^[0-9a-f]{4}:", line):
            continue
        address = int(line[:4], 16)
        data = bytes.fromhex(" ".join(re.findall(r"[0-9a-f]{2}", line[6:19])))
        row_bytes[address] = data
        for offset, value in enumerate(data):
            assert address + offset not in reference
            reference[address + offset] = value
        if line[19:20].isupper():
            labels[line[19:].split()[0]] = address

    intervals = []
    for (start, category, label), (end, _, _) in zip(BOUNDARIES, BOUNDARIES[1:]):
        assert start < end
        known = [a for a in reference if start <= a < end]
        changed = [a for a in known if reference[a] != rom[a - 0xC000]]
        intervals.append({
            "start": start, "end_exclusive": end,
            "start_hex": f"0x{start:04X}", "end_exclusive_hex": f"0x{end:04X}",
            "category": category, "label": label,
            "requires_rom": start >= 0xC000,
            "requires_stock_chrget": start == 0x00B1,
            "reference_known_bytes": len(known), "reference_different_bytes": len(changed),
        })
    assert intervals[0]["start"] == 0 and intervals[-1]["end_exclusive"] == 0x10000
    assert all(a["end_exclusive"] == b["start"] for a, b in zip(intervals, intervals[1:]))

    def category_at(address):
        return next(i["category"] for i in intervals if i["start"] <= address < i["end_exclusive"])

    entries = []
    for address, name, template in sorted(
        [(labels[name], name, None) for name in ENTRY_NAMES] + EXTRA_ENTRIES
    ):
        location = template if template is not None else address
        signature = rom[location - 0xC000: location - 0xC000 + 12]
        mask = bytearray([0xFF] * len(signature))
        if template is not None:
            for mutable in (0x00B8, 0x00B9):
                if address <= mutable < address + len(mask):
                    mask[mutable - address] = 0
        expected = row_bytes.get(location)
        entries.append({
            "address": address, "address_hex": f"0x{address:04X}",
            "name": name, "category": category_at(address),
            "requires_rom": template is None,
            "requires_stock_chrget": template is not None,
            "rom_offset": location - 0xC000,
            "signature_hex": signature.hex(), "signature_mask_hex": mask.hex(),
            "reference_first_row_match": None if expected is None else (
                rom[location - 0xC000:location - 0xC000 + len(expected)] == expected
            ),
        })

    differences = [a for a, value in reference.items() if value != rom[a - 0xC000]]
    result = {
        "schema_version": 1,
        "address_format": "integer byte addresses; all intervals are [start, end_exclusive)",
        "metadata": {
            "rom_path": str(ROM_PATH), "rom_base": 0xC000, "rom_size": len(rom),
            "rom_sha256": ROM_SHA256, "source_url": SOURCE_URL,
            "source_snapshot": SOURCE_PATH.name, "source_sha256": SOURCE_SHA256,
            "source_original": "https://www.txbobsc.com/scsc/scdocumentor/",
            "reference_known_bytes": len(reference),
            "reference_matching_bytes": len(reference) - len(differences),
            "reference_different_bytes": len(differences),
            "reference_different_addresses": [f"0x{a:04X}" for a in differences],
            "enhanced_patch_basis": "Exact bundled ROM bytes and disassembly of F775-F7FF plus patched call sites; old II+ cassette labels are not used for that region.",
        },
        "categories": [P, V, L, A, I, O, S, G, M, U],
        "harness_landmarks": {
            "run_entry": 0xD912,
            "basic_restart": 0xD43C,
            "basic_prompt_before_input": 0xD43F,
            "keyboard_read_entry": 0xFD0C,
            "keyboard_read_indirect_jump": 0xFD18,
            "keyboard_handler_pointer": 0x0038,
            "keyboard_return": "At FD0C, return address = ((mem[0x100|((SP+1)&0xff)] | (mem[0x100|((SP+2)&0xff)]<<8))+1)&0xffff. Capture dynamically; DOS hooks and callers differ.",
            "rnd_seed_start": 0x00C9,
            "rnd_seed_end_exclusive": 0x00CE,
            "rnd_template_start": 0xF123,
            "rnd_template_hex": rom[0xF123 - 0xC000:0xF128 - 0xC000].hex(),
            "rnd_initialization_note": "Cold start copies only through CC; CD is not initialized by the F150 copy loop. Snapshot or initialize all five C9-CD bytes explicitly for reproducibility.",
        },
        "caveats": [
            "Exclusive PC occupancy only. Callee cycles use the callee's category, not the caller's. Do not sum inclusive routine counters into these categories.",
            "Count entries only at retired/executed instruction boundaries; counting repeated stalled-PC samples overcounts calls.",
            "requires_rom regions must map to DOS/user when the effective instruction source is language-card RAM, banked RAM, or another ROM. Address alone cannot establish ROM identity.",
            "C000-CFFF is a coarse slot-firmware/I/O category, not a claim that the Applesoft ROM owns those addresses.",
            "The RAM CHRGET interval needs code-signature guards with B8/B9 masked because they are the text pointer. If patched or remapped, categorize as DOS/user.",
            "The source listing is II+, not Enhanced IIe. All entry signatures come from the exact Enhanced ROM; reference byte differences are reported and Enhanced helper regions are separately mapped.",
            "Only bytes printed in the HTML listing were compared. Long data directives can omit bytes; this is not proof of full binary equivalence.",
            "Categories are coarse semantic ownership: shared memory movers are variable/array, array integer multiply is variable/array, shared FP helpers are arithmetic, and monitor low-res graphics is monitor/IO.",
            "PRINT/LIST formatting is monitor/IO except numeric conversion. INPUT/READ orchestration is literals/number input; its actual I/O and math callees belong to other categories.",
            "ROM data tables are covered for completeness but normally should have no executed instruction samples.",
            "Entry signatures pin addresses and local bytes only; they do not prove complete routine compatibility or validate an acceleration replacement.",
        ],
        "intervals": intervals,
        "entries": entries,
    }
    output = HERE / "applesoft_enhanced_map.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    (HERE / "applesoft_enhanced_intervals.tsv").write_text(
        "start\tend_exclusive\tcategory\tlabel\n" + "".join(
            f"{i['start_hex']}\t{i['end_exclusive_hex']}\t{i['category']}\t{i['label']}\n"
            for i in intervals
        ), encoding="utf-8"
    )
    print(f"Wrote {len(intervals)} complete nonoverlapping intervals and {len(entries)} entries")
    print(f"ROM SHA256: {ROM_SHA256}")
    print(f"Reference displayed bytes: {len(reference)}; same: {len(reference)-len(differences)}; different: {len(differences)}")
    print(output)


if __name__ == "__main__":
    main()
