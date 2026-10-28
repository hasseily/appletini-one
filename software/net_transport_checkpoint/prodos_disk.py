#!/usr/bin/env python3
"""Small deterministic ProDOS-order volume packager, using only stdlib.

Only root directories and seedling/sapling files are supported. The original
ProDOS boot blocks and PRODOS kernel are copied from the separately installed
release input; no operating-system code is implemented or patched here.
Format reference: https://prodos8.com/docs/techref/file-organization/
"""
import math
import re
import struct

BLOCK = 512
ENTRY = 39
ROOT_BLOCKS = (2, 3, 4, 5)
BITMAP = 6


def u16(data, offset):
    return int.from_bytes(data[offset:offset + 2], "little")


def root_entries(image):
    """Yield (directory byte offset, 39-byte entry) after checked traversal."""
    if len(image) % BLOCK or len(image) < 7 * BLOCK:
        raise ValueError("Not a ProDOS-order block image")
    seen = set()
    current, previous = 2, 0
    while current:
        if current in seen or current >= len(image) // BLOCK:
            raise ValueError("Directory loop or out-of-range block")
        seen.add(current)
        base = current * BLOCK
        if u16(image, base) != previous:
            raise ValueError("Invalid directory backward link")
        for index in range(1 if current == 2 else 0, 13):
            start = base + 4 + index * ENTRY
            entry = bytes(image[start:start + ENTRY])
            if entry[0] >> 4:
                yield start, entry
        previous, current = current, u16(image, base + 2)


def entry_name(entry):
    return entry[1:1 + (entry[0] & 15)].decode("ascii")


def read_root_files(image):
    """Extract supported root files as a name -> bytes mapping."""
    files = {}
    for _, entry in root_entries(image):
        name = entry_name(entry)
        kind, key = entry[0] >> 4, u16(entry, 17)
        length = int.from_bytes(entry[21:24], "little")
        if name in files:
            raise ValueError("Duplicate name " + name)
        if kind == 1 and length <= BLOCK:
            blocks = (key,)
        elif kind == 2 and length <= 256 * BLOCK:
            index = image[key * BLOCK:(key + 1) * BLOCK]
            if len(index) != BLOCK:
                raise ValueError("Invalid sapling index")
            blocks = [index[i] | (index[256 + i] << 8)
                      for i in range(math.ceil(length / BLOCK))]
        else:
            raise ValueError("Unsupported storage type for " + name)
        payload = bytearray()
        for block in blocks:
            if block >= len(image) // BLOCK:
                raise ValueError("File block outside image")
            payload.extend(image[block * BLOCK:(block + 1) * BLOCK]
                           if block else bytes(BLOCK))
        files[name] = bytes(payload[:length])
    return files


def validate_image(image):
    """Verify directory, ownership, EOF, count, and allocation bitmap."""
    header = image[2 * BLOCK + 4:2 * BLOCK + 43]
    if len(header) != ENTRY or header[0] >> 4 != 15:
        raise ValueError("Missing volume header")
    if header[31:33] != bytes((ENTRY, 13)):
        raise ValueError("Unexpected directory entry layout")
    total, bitmap = u16(header, 37), u16(header, 35)
    if total * BLOCK != len(image) or not 0 < bitmap < total:
        raise ValueError("Volume size/bitmap mismatch")
    bitmap_count = math.ceil(total / 4096)
    used = {0, 1, *range(bitmap, bitmap + bitmap_count)}

    def claim(block):
        if not 0 < block < total or block in used:
            raise ValueError("Shared or invalid allocation block: " + str(block))
        used.add(block)

    current = 2
    while current:
        claim(current)
        current = u16(image, current * BLOCK + 2)
    count = 0
    for _, entry in root_entries(image):
        count += 1
        key, kind = u16(entry, 17), entry[0] >> 4
        size = int.from_bytes(entry[21:24], "little")
        if u16(entry, 37) != 2:
            raise ValueError("Wrong file parent header")
        claim(key)
        allocated = 1
        if kind == 2 and BLOCK < size <= 256 * BLOCK:
            index = image[key * BLOCK:(key + 1) * BLOCK]
            for i in range(math.ceil(size / BLOCK)):
                block = index[i] | (index[i + 256] << 8)
                if block:
                    claim(block)
                    allocated += 1
        elif kind != 1 or size > BLOCK:
            raise ValueError("Unsupported storage/EOF")
        if allocated != u16(entry, 19):
            raise ValueError("Wrong blocks-used count")
    if count != u16(header, 33):
        raise ValueError("Wrong volume file count")
    bitmap_data = image[bitmap * BLOCK:(bitmap + bitmap_count) * BLOCK]
    for block in range(bitmap_count * 4096):
        free = bool(bitmap_data[block // 8] & (0x80 >> (block % 8)))
        if free != (block < total and block not in used):
            raise ValueError("Bitmap mismatch at " + str(block))
    read_root_files(image)


def build_image(template, program, name="LIBTINI.SYSTEM", *, files=(), total=280, volume="LIBTINI",
                max_program_bytes=0x8000):
    """Create a bootable 280-block .po with a kernel and first SYS program.

    Fixed timestamps and allocation order make this byte-for-byte repeatable.
    The embedded ProDOS 2.4.3 Bitsy Bye handles the diagnostic's MLI QUIT.
    """
    if not re.fullmatch(r"[A-Z][A-Z0-9.]{0,14}", name):
        raise ValueError("Invalid ProDOS filename")
    if max_program_bytes not in (0x8000, 0x9F00) or not program or len(program) > max_program_bytes:
        raise ValueError("SYS payload exceeds its checked ProDOS load region")
    source_files = read_root_files(template)
    if "PRODOS" not in source_files:
        raise ValueError("Installed image does not contain PRODOS")
    if not isinstance(total, int) or not 280 <= total <= 65535:
        raise ValueError("ProDOS volume must contain 280..65535 blocks")
    if not re.fullmatch(r"[A-Z][A-Z0-9.]{0,14}", volume):
        raise ValueError("Invalid ProDOS volume name")
    all_files = [("PRODOS", source_files["PRODOS"], 0xFF, 0),
                 (name, program, 0xFF, 0x2000)] + list(files)
    if len(all_files) > len(ROOT_BLOCKS) * 13 - 1:
        raise ValueError("Root directory full")
    names = set()
    for filename, payload, filetype, aux in all_files:
        if not re.fullmatch(r"[A-Z][A-Z0-9.]{0,14}", filename) or filename in names:
            raise ValueError("Invalid or duplicate ProDOS filename")
        if len(payload) > 256 * BLOCK or not 0 <= filetype <= 255 or not 0 <= aux <= 65535:
            raise ValueError("File exceeds sapling/metadata limits")
        names.add(filename)
    image = bytearray(total * BLOCK)
    image[:2 * BLOCK] = template[:2 * BLOCK]
    for i, block in enumerate(ROOT_BLOCKS):
        struct.pack_into("<HH", image, block * BLOCK,
                         ROOT_BLOCKS[i - 1] if i else 0,
                         ROOT_BLOCKS[i + 1] if i + 1 < len(ROOT_BLOCKS) else 0)
    header = bytearray(ENTRY)
    header[0] = 0xF0 | len(volume)
    header[1:1 + len(volume)] = volume.encode("ascii")
    # ProDOS date 2026-10-20 00:00; fixed, not the host clock.
    struct.pack_into("<H", header, 24, (26 << 9) | (10 << 5) | 20)
    header[28:33] = bytes((0, 0, 0xE3, ENTRY, 13))
    struct.pack_into("<HHH", header, 33, len(all_files), BITMAP, total)
    image[2 * BLOCK + 4:2 * BLOCK + 43] = header
    next_free = BITMAP + math.ceil(total / 4096)
    used = set(range(next_free))

    def allocate(payload):
        nonlocal next_free
        count = max(1, math.ceil(len(payload) / BLOCK))
        if count > 256:
            raise ValueError("Payload exceeds sapling capacity")
        required = count + (count > 1)
        if next_free + required > total:
            raise ValueError("Volume full")
        key = next_free
        next_free += required
        used.update(range(key, next_free))
        if count > 1:
            for i in range(count):
                block = key + 1 + i
                image[key * BLOCK + i] = block & 255
                image[key * BLOCK + 256 + i] = block >> 8
            first_data = key + 1
        else:
            first_data = key
        image[first_data * BLOCK:first_data * BLOCK + len(payload)] = payload
        return (2 if count > 1 else 1), key, required

    # Kernel first, then diagnostic. PRODOS itself has no .SYSTEM suffix.
    for slot, (filename, payload, filetype, aux) in enumerate(all_files, start=1):
        kind, key, blocks = allocate(payload)
        entry = bytearray(ENTRY)
        entry[0] = (kind << 4) | len(filename)
        entry[1:1 + len(filename)] = filename.encode("ascii")
        entry[16] = filetype
        struct.pack_into("<HH", entry, 17, key, blocks)
        entry[21:24] = len(payload).to_bytes(3, "little")
        struct.pack_into("<H", entry, 24, (26 << 9) | (10 << 5) | 20)
        entry[30] = 0xE3
        struct.pack_into("<H", entry, 31, aux)
        struct.pack_into("<H", entry, 37, 2)
        directory_block = ROOT_BLOCKS[slot // 13]
        start = directory_block * BLOCK + 4 + (slot % 13) * ENTRY
        image[start:start + ENTRY] = entry
    for block in range(total):
        if block not in used:
            image[BITMAP * BLOCK + block // 8] |= 0x80 >> (block % 8)
    validate_image(image)
    if read_root_files(image)[name] != program:
        raise ValueError("Packaged program differs from linked output")
    return bytes(image)
