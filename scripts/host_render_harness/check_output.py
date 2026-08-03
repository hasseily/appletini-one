#!/usr/bin/env python3
"""Pixel-exact verification of host_render_harness dumps.

For each dumped frame the shadow banks captured at the same moment are
re-decoded with scripts/render_shr4.py (the byte-exact reference for
the C renderer) and compared per pixel. Any mismatch means the C
renderer's published frame does not reflect its own shadow content --
exactly the class of bug reported for SHR-to-SHR transitions.

Usage: check_output.py <out_dir>
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import render_shr4  # noqa: E402


def load_banks(out: Path, main_name: str, aux_name: str):
    main = (out / main_name).read_bytes()
    aux = (out / aux_name).read_bytes()
    return aux[0x2000:0xA000], main[0x2000:0xA000]


def decode_reference(aux_bank: bytes, main_bank: bytes):
    ctrl = aux_bank[render_shr4.CTRL - 0x2000]
    paged = ctrl if ctrl in (1, 2) else 0
    r = render_shr4.Renderer(aux_bank, main_bank, paged)
    return r.frame(), paged


def compare(name: str, frame_bin: Path, rows) -> bool:
    data = frame_bin.read_bytes()
    assert len(data) == 640 * 400 * 4, f"{name}: bad dump size {len(data)}"
    bad = 0
    first = None
    for y in range(400):
        ref = rows[y]
        row_off = y * 640 * 4
        for x in range(640):
            b = data[row_off + x * 4]
            g = data[row_off + x * 4 + 1]
            r = data[row_off + x * 4 + 2]
            er, eg, eb = ref[x * 3], ref[x * 3 + 1], ref[x * 3 + 2]
            if (r, g, b) != (er, eg, eb):
                bad += 1
                if first is None:
                    first = (x, y, (r, g, b), (er, eg, eb))
    if bad:
        x, y, got, exp = first
        print(f"FAIL {name}: {bad} mismatching pixels; first at "
              f"({x},{y}) got RGB{got} expected RGB{exp}")
        return False
    print(f"PASS {name}: 640x400 pixel-exact vs reference decode")
    return True


def edge_profile(name: str, frame_bin: Path) -> None:
    """Print the rightmost 16 columns of row 100 to characterize any
    edge tint (the dragons blue-column question)."""
    data = frame_bin.read_bytes()
    y = 100
    cells = []
    for x in range(624, 640):
        o = (y * 640 + x) * 4
        cells.append(f"{data[o+2]:02X}{data[o+1]:02X}{data[o]:02X}")
    print(f"  {name} row {y} cols 624-639 (RRGGBB): {' '.join(cells)}")


def main() -> int:
    out = Path(sys.argv[1])
    ok = True

    cases = (
        ("t1 dragons", "t1_dragons_frame.bin", "t1_main.bin", "t1_aux.bin"),
        ("t3 beach", "t3_beach_frame.bin", "t3_beach_main.bin",
         "t3_beach_aux.bin"),
        ("t3 eye320", "t3_eye_frame.bin", "t3_eye_main.bin",
         "t3_eye_aux.bin"),
        ("t3 fluidart", "t3_fluid_frame.bin", "t3_fluid_main.bin",
         "t3_fluid_aux.bin"),
    )
    for name, frame, main_name, aux_name in cases:
        aux_bank, main_bank = load_banks(out, main_name, aux_name)
        rows, paged = decode_reference(aux_bank, main_bank)
        print(f"--- {name} (paged={paged}) ---")
        ok = compare(name, out / frame, rows) and ok
        edge_profile(name, out / frame)

    print("checker:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
