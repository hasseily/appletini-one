"""Read/write PCPI Applicard CP/M 2.2 disk images (Apple 5.25" .do, 140K).

Layout (cpmtools `apple-do`, validated against pcpiboot.do -- see
scripts/test_applicard_card.py and README_APPLICARD.md):
  35 tracks x 16 x 256-byte sectors, DOS-order image;
  3 reserved boot tracks; data area = tracks 3-34 = 128 x 1K blocks;
  CP/M software skew per track: 0,6,12,3,9,15,14,5,11,2,8,7,13,4,10,1;
  directory = blocks 0-1, 64 entries; EXM=0 (one 16K logical extent
  per directory entry, 16 x 1-byte block pointers).

Commands:
  python pcpi_disk.py ls      IMG
  python pcpi_disk.py info    IMG
  python pcpi_disk.py extract IMG CPMNAME OUTFILE
  python pcpi_disk.py add     IMG LOCALFILE [CPMNAME]
  python pcpi_disk.py rm      IMG CPMNAME
  python pcpi_disk.py blank   IMG            (create empty non-boot data disk)
  python pcpi_disk.py verify  IMG            (text-integrity check of .ASM/.SUB)
"""

import sys
from pathlib import Path

TRACKS = 35
SECTRK = 16
SECLEN = 256
BOOTTRK = 3
BLOCKSIZE = 1024
MAXDIR = 64
SKEW = [0, 6, 12, 3, 9, 15, 14, 5, 11, 2, 8, 7, 13, 4, 10, 1]
DATA_BLOCKS = (TRACKS - BOOTTRK) * SECTRK * SECLEN // BLOCKSIZE  # 128
DIR_BLOCKS = MAXDIR * 32 // BLOCKSIZE  # 2
SECS_PER_BLOCK = BLOCKSIZE // SECLEN  # 4
IMAGE_SIZE = TRACKS * SECTRK * SECLEN


def sector_offset(seq):
    """Data-area sector sequence number -> byte offset in the .do image."""
    track = BOOTTRK + seq // SECTRK
    return (track * SECTRK + SKEW[seq % SECTRK]) * SECLEN


def read_block(img, block):
    out = bytearray()
    for s in range(SECS_PER_BLOCK):
        off = sector_offset(block * SECS_PER_BLOCK + s)
        out += img[off:off + SECLEN]
    return bytes(out)


def write_block(img, block, data):
    assert len(data) == BLOCKSIZE
    for s in range(SECS_PER_BLOCK):
        off = sector_offset(block * SECS_PER_BLOCK + s)
        img[off:off + SECLEN] = data[s * SECLEN:(s + 1) * SECLEN]


def read_dir(img):
    raw = b"".join(read_block(img, b) for b in range(DIR_BLOCKS))
    entries = []
    for i in range(MAXDIR):
        e = raw[i * 32:(i + 1) * 32]
        entries.append({
            "index": i,
            "user": e[0],
            "name": bytes(b & 0x7F for b in e[1:9]).decode("ascii",
                                                           "replace"),
            "ext": bytes(b & 0x7F for b in e[9:12]).decode("ascii",
                                                           "replace"),
            "ex": e[12], "s2": e[14], "rc": e[15],
            "al": list(e[16:32]),
        })
    return entries


def write_dir_entry(img, index, entry_bytes):
    assert len(entry_bytes) == 32
    block = (index * 32) // BLOCKSIZE
    data = bytearray(read_block(img, block))
    off = (index * 32) % BLOCKSIZE
    data[off:off + 32] = entry_bytes
    write_block(img, block, bytes(data))


def display_name(e):
    ext = e["ext"].strip()
    return e["name"].strip() + ("." + ext if ext else "")


def live_entries(img):
    return [e for e in read_dir(img) if e["user"] <= 15]


def used_blocks(img):
    used = set(range(DIR_BLOCKS))
    for e in live_entries(img):
        for b in e["al"]:
            if b:
                used.add(b)
    return used


def file_entries(img, cpmname):
    want = cpmname.upper()
    ents = [e for e in live_entries(img) if display_name(e) == want]
    return sorted(ents, key=lambda e: (e["s2"], e["ex"]))


def extract(img, cpmname):
    ents = file_entries(img, cpmname)
    if not ents:
        raise SystemExit(f"{cpmname}: not found")
    out = bytearray()
    for e in ents:
        records = e["rc"]
        nblocks = (records * 128 + BLOCKSIZE - 1) // BLOCKSIZE
        data = b"".join(read_block(img, b) for b in e["al"][:nblocks])
        out += data[:records * 128]
    return bytes(out)


def parse_cpm_name(cpmname):
    cpmname = cpmname.upper()
    if "." in cpmname:
        name, ext = cpmname.split(".", 1)
    else:
        name, ext = cpmname, ""
    if not (1 <= len(name) <= 8) or len(ext) > 3:
        raise SystemExit(f"bad CP/M name: {cpmname}")
    ok = set("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$#&@!%'()-{}_~`")
    if not all(c in ok for c in name + ext):
        raise SystemExit(f"bad CP/M name chars: {cpmname}")
    return name.ljust(8), ext.ljust(3)


def add(img, data, cpmname):
    name, ext = parse_cpm_name(cpmname)
    if file_entries(img, cpmname):
        raise SystemExit(f"{cpmname}: already exists on image")

    records = (len(data) + 127) // 128
    if records == 0:
        records = 1
    padded = data + b"\x1a" * (records * 128 - len(data))

    free = sorted(set(range(DIR_BLOCKS, DATA_BLOCKS)) - used_blocks(img))
    nblocks = (records * 128 + BLOCKSIZE - 1) // BLOCKSIZE
    if nblocks > len(free):
        raise SystemExit(f"{cpmname}: needs {nblocks} blocks, "
                         f"only {len(free)} free")

    free_slots = [e["index"] for e in read_dir(img) if e["user"] > 15]
    nextents = (records + 127) // 128  # 128 records (16K) per extent
    if nextents > len(free_slots):
        raise SystemExit(f"{cpmname}: needs {nextents} dir entries, "
                         f"only {len(free_slots)} free")

    blocks = free[:nblocks]
    bi = 0
    for x in range(nextents):
        ex_records = min(128, records - x * 128)
        ex_blocks = (ex_records * 128 + BLOCKSIZE - 1) // BLOCKSIZE
        al = blocks[bi:bi + ex_blocks]
        for j, b in enumerate(al):
            start = (x * 128 + (j * BLOCKSIZE) // 128) * 128
            chunk = padded[start:start + BLOCKSIZE]
            chunk += b"\x1a" * (BLOCKSIZE - len(chunk))
            write_block(img, b, chunk)
        bi += ex_blocks
        entry = bytes([0]) + name.encode() + ext.encode() + \
            bytes([x & 0x1F, 0, (x >> 5) & 0xFF, ex_records]) + \
            bytes(al + [0] * (16 - len(al)))
        write_dir_entry(img, free_slots[x], entry)
    return nblocks


def remove(img, cpmname):
    ents = file_entries(img, cpmname)
    if not ents:
        raise SystemExit(f"{cpmname}: not found")
    for e in ents:
        block = (e["index"] * 32) // BLOCKSIZE
        data = bytearray(read_block(img, block))
        data[(e["index"] * 32) % BLOCKSIZE] = 0xE5
        write_block(img, block, bytes(data))


def blank_image():
    img = bytearray(b"\x00" * IMAGE_SIZE)
    for b in range(DIR_BLOCKS):
        write_block(img, b, b"\xe5" * BLOCKSIZE)
    return img


def cmd_ls(img):
    files = {}
    for e in live_entries(img):
        key = display_name(e)
        files.setdefault(key, 0)
        files[key] += e["rc"] * 128
    total = 0
    for name in sorted(files):
        print(f"  {name:<12} {files[name]:>7}")
        total += files[name]
    free = (DATA_BLOCKS - len(used_blocks(img))) * BLOCKSIZE
    print(f"{len(files)} files, {total} bytes; {free} bytes free")


def cmd_verify(img):
    ok = True
    for e in live_entries(img):
        name = display_name(e)
        if not (name.endswith(".ASM") or name.endswith(".SUB")):
            continue
        data = extract(img, name).rstrip(b"\x1a")
        printable = all(b in (9, 10, 13) or 32 <= b < 127 for b in data)
        longest = max((len(l) for l in data.split(b"\r\n")), default=0)
        status = "OK" if printable and longest < 100 else "CORRUPT"
        if status != "OK":
            ok = False
        print(f"  {name:<12} printable={printable} longest_line={longest} "
              f"{status}")
    if not ok:
        raise SystemExit("verify failed: format assumptions are wrong")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, path = sys.argv[1], Path(sys.argv[2])

    if cmd == "blank":
        path.write_bytes(blank_image())
        print(f"created blank data disk {path}")
        return

    img = bytearray(path.read_bytes())
    if len(img) != IMAGE_SIZE:
        raise SystemExit(f"{path}: not a 140K .do image")

    if cmd == "ls":
        cmd_ls(img)
    elif cmd == "info":
        used = used_blocks(img)
        print(f"blocks used {len(used)}/{DATA_BLOCKS}, "
              f"free {(DATA_BLOCKS - len(used)) * BLOCKSIZE} bytes, "
              f"dir slots free "
              f"{sum(1 for e in read_dir(img) if e['user'] > 15)}/{MAXDIR}")
    elif cmd == "verify":
        cmd_verify(img)
    elif cmd == "extract":
        data = extract(img, sys.argv[3])
        Path(sys.argv[4]).write_bytes(data)
        print(f"extracted {sys.argv[3]} ({len(data)} bytes)")
    elif cmd == "add":
        local = Path(sys.argv[3])
        cpmname = sys.argv[4] if len(sys.argv) > 4 else local.name
        n = add(img, local.read_bytes(), cpmname)
        path.write_bytes(img)
        print(f"added {cpmname} ({n} blocks)")
    elif cmd == "rm":
        remove(img, sys.argv[3])
        path.write_bytes(img)
        print(f"removed {sys.argv[3]}")
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
