#!/usr/bin/env python3
"""Host-side reference decoder for SDD extended SHR modes.

Mirrors apple_cycle_renderer.c's SHR frame path (shr_eval_field_modes /
render_shr_line / shr4_render_cell_*) byte for byte, so real art can
validate the firmware decode without hardware. Supported:

- standard SHR 320/640 (colorfill excluded, as in the SHR4 path)
- SHR4 (magic $D3C8D2B4): per-pixel submode dispatch -- $1 RGGB Malvar
  demosaic (4-bit samples in 320, 2-bit in 640), $2 PAL256, $3 R4G4B4,
  else standard
- SHR-3200 (magic $B3B2B0B0): 200 per-line palettes, reversed index
- interlace (even rows aux field, odd rows main field)
- paired fields (renders both source fields as separate PNG references;
  firmware does not alternate them)

File layouts accepted: 32768 ($2000-$9FFF bank image), 39168 (bank +
6400 palette bytes destined for main per the $9DF8 ctrl pointer), 65536
(aux bank then main bank).

Usage: render_shr4.py IMAGE OUT.png [--paged N]
--paged forces the double mode (0/1/2) when the ctrl byte disagrees
with the art (some converters store the mode at $9DFB instead of $9DF8).
"""

import struct
import sys
import zlib

BANK = 0x8000
MAGIC = 0x9DFC
CTRL = 0x9DF8
PAL_BLOCK = 200 * 32

K_G = (-2, 0, 4, 0, -2, 4, 8, 4, -2, 0, 4, 0, -2)
K_XG = (1, -2, 0, -2, -2, 8, 10, 8, -2, -2, 0, -2, 1)
K_XGX = (-2, -2, 8, -2, 1, 0, 10, 0, 1, -2, 8, -2, -2)
K_RB = (-3, 4, 0, 4, -3, 0, 12, 0, -3, 4, 0, 4, -3)
SAMPLE_OFFSETS = ((0, -2), (-1, -1), (0, -1), (1, -1), (-2, 0), (-1, 0),
                  (0, 0), (1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2))
QUAD_BASE = (8, 12, 0, 4)          # 640-mode palette quadrant per pixel


def fail(msg):
    print(f"error: {msg}", file=sys.stderr)
    raise SystemExit(1)


def write_png(path, width, height, rows):
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data)))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR",
                      struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


class Field:
    """Mode state for one field bank -- shr_eval_field_modes."""

    def __init__(self, bank, aux, main):
        self.bank, self.aux, self.main = bank, aux, main
        self.shr4 = bank[MAGIC - 0x2000:MAGIC - 0x2000 + 4] == \
            b"\xd3\xc8\xd2\xb4"
        self.shr3200 = False
        if not self.shr4 and bank[MAGIC - 0x2000:MAGIC - 0x2000 + 4] == \
                b"\xb3\xb2\xb0\xb0":
            pal = bank[CTRL - 0x2000 + 2] | (bank[CTRL - 0x2000 + 3] << 8)
            if pal < 0xFFFF - PAL_BLOCK:
                self.shr3200 = True
                self.pal3200_bank = self.aux if bank[CTRL - 0x2000 + 1] == 1 \
                    else self.main
                self.pal3200 = pal


def rd(bank, addr):
    """Read memory address addr from a $2000-based bank image."""
    off = addr - 0x2000
    return bank[off] if 0 <= off < BANK else 0


def gs_rgb(raw):
    return (((raw >> 8) & 0xF) * 16, ((raw >> 4) & 0xF) * 16,
            (raw & 0xF) * 16)


def pal_color(field, pal_base, idx):
    a = pal_base + idx * 2
    return gs_rgb(rd(field.bank, a) | (rd(field.bank, a + 1) << 8))


class Renderer:
    def __init__(self, aux, main, paged):
        self.aux, self.main, self.paged = aux, main, paged
        self.interlaced = paged == 1

    def sample320(self, field, px, row):
        rows = 400 if self.interlaced else 200
        if not (0 <= px < 320 and 0 <= row < rows):
            return 0
        bank, py = field.bank, row
        if self.interlaced:
            bank = self.main if (row & 1) else self.aux
            py = row >> 1
        byte = rd(bank, 0x2000 + 160 * py + (px >> 1))
        return (byte & 0xF) if (px & 1) else (byte >> 4)

    def sample640(self, field, px, row):
        rows = 400 if self.interlaced else 200
        if not (0 <= px < 640 and 0 <= row < rows):
            return 0
        bank, py = field.bank, row
        if self.interlaced:
            bank = self.main if (row & 1) else self.aux
            py = row >> 1
        byte = rd(bank, 0x2000 + 160 * py + (px >> 2))
        return (byte >> (6 - 2 * (px & 3))) & 3

    def rggb(self, field, px, row, sample, maxval):
        s = [sample(field, px + dx, row + dy) for dx, dy in SAMPLE_OFFSETS]
        own = s[6] * 255 // maxval
        div = maxval * 16

        def filt(w):
            acc = sum(a * b for a, b in zip(s, w)) * 255 // div
            return max(0, min(255, acc))
        if (px & 1) == 0 and (row & 1) == 0:
            return (own, filt(K_G), filt(K_RB))
        if (px & 1) == 1 and (row & 1) == 0:
            return (filt(K_XG), own, filt(K_XGX))
        if (px & 1) == 0 and (row & 1) == 1:
            return (filt(K_XGX), own, filt(K_XG))
        return (filt(K_RB), filt(K_G), own)

    def line(self, field, y, out_row):
        """One SHR line -> 640 RGB pixels. out_row is the RGGB row space
        (0..199 per field, 0..399 interlaced), like s_f_out_row."""
        scb = rd(field.bank, 0x9D00 + y)
        pal_base = 0x9E00 + (scb & 0xF) * 32
        is640 = bool(scb & 0x80)
        px_row = []
        for xb in range(160):
            byte = rd(field.bank, 0x2000 + 160 * y + xb)
            if is640:
                for lp in range(4):
                    v = (byte >> (6 - 2 * lp)) & 3
                    idx = QUAD_BASE[lp] + v
                    px640 = xb * 4 + lp
                    sub = rd(field.bank, pal_base + idx * 2 + 1) >> 4 \
                        if field.shr4 else 0
                    if field.shr4 and sub == 1:
                        px_row.append(self.rggb(field, px640, out_row,
                                                self.sample640, 3))
                    else:
                        px_row.append(pal_color(field, pal_base, idx))
            else:
                for half in range(2):
                    idx = (byte >> 4) if half == 0 else (byte & 0xF)
                    px = xb * 2 + half
                    if field.shr4:
                        b2 = rd(field.bank, pal_base + idx * 2 + 1)
                        sub = b2 >> 4
                        if sub == 1:
                            c = self.rggb(field, px, out_row,
                                          self.sample320, 15)
                        elif sub == 2:
                            a = 0x9E00 + byte * 2
                            c = gs_rgb(rd(field.bank, a)
                                       | (rd(field.bank, a + 1) << 8))
                        elif sub == 3:
                            group = px // 6
                            base = 0x2000 + 160 * y + group * 3
                            # 160 bytes = 53 1/3 triplets: bytes past the
                            # line read back 0, matching SDD's shader
                            # (out-of-texture texel fetch) and the C
                            # renderer's clamp.
                            b0, b1, bx = (
                                rd(field.bank, base + k)
                                if group * 3 + k < 160 else 0
                                for k in range(3))
                            if px % 6 < 3:
                                c = ((b0 >> 4) * 16, (b0 & 0xF) * 16,
                                     (b1 >> 4) * 16)
                            else:
                                c = ((b1 & 0xF) * 16, (bx >> 4) * 16,
                                     (bx & 0xF) * 16)
                        else:
                            c = pal_color(field, pal_base, idx)
                    elif field.shr3200:
                        a = field.pal3200 + y * 32 + (15 - idx) * 2
                        c = gs_rgb(rd(field.pal3200_bank, a)
                                   | (rd(field.pal3200_bank, a + 1) << 8))
                    else:
                        c = pal_color(field, pal_base, idx)
                    px_row.append(c)
                    px_row.append(c)
        return b"".join(bytes(c) for c in px_row)

    def frame(self, field_index=0):
        rows = [None] * 400
        if self.interlaced:
            for fi, bank in enumerate((self.aux, self.main)):
                field = Field(bank, self.aux, self.main)
                for y in range(200):
                    rows[y * 2 + fi] = self.line(field, y, y * 2 + fi)
        else:
            bank = self.main if (self.paged == 2 and field_index) else self.aux
            field = Field(bank, self.aux, self.main)
            for y in range(200):
                rows[y * 2] = rows[y * 2 + 1] = self.line(field, y, y)
        return rows


def main():
    argv = sys.argv[1:]
    paged = None
    if "--paged" in argv:
        i = argv.index("--paged")
        paged = int(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) != 2:
        fail("usage: render_shr4.py IMAGE OUT.png [--paged N]")
    data = open(argv[0], "rb").read()
    if len(data) < BANK:
        fail(f"file is {len(data)} bytes; need at least {BANK}")
    aux = data[:BANK]
    main_bank = bytearray(BANK)
    if len(data) == 2 * BANK:
        main_bank = data[BANK:]
    elif len(data) >= BANK + PAL_BLOCK:
        # 3200 layout: palette payload lands at the ctrl pointer in main
        pal = aux[CTRL - 0x2000 + 2] | (aux[CTRL - 0x2000 + 3] << 8)
        main_bank[pal - 0x2000:pal - 0x2000 + PAL_BLOCK] = \
            data[BANK:BANK + PAL_BLOCK]
        main_bank = bytes(main_bank)
    ctrl_paged = aux[CTRL - 0x2000]
    if paged is None:
        paged = ctrl_paged if ctrl_paged in (1, 2) else 0
    print(f"ctrl paged={ctrl_paged} rendering paged={paged}")
    r = Renderer(aux, main_bank, paged)
    if paged == 2:
        for field_index in (0, 1):
            out = argv[1].replace(".png", f"_field{field_index}.png")
            write_png(out, 640, 400, r.frame(field_index))
            print(f"wrote {out}")
    else:
        write_png(argv[1], 640, 400, r.frame())
        print(f"wrote {argv[1]}")


if __name__ == "__main__":
    main()
