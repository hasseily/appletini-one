#!/usr/bin/env python3
"""Regression tests for Appletini Disk II WOZ file handling.

These are host-side tests for the WOZ file-format contract implemented by
ps_sources/frontend/disk2_service.c. They intentionally avoid hardware, SD
cards, Vitis, or pytest so they can run quickly with:

    python scripts/test_disk2_woz.py
"""

from __future__ import annotations

import binascii
import re
import struct
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DISK2_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "disk2_service.c"
UART_CONTROL_C = REPO_ROOT / "ps_sources" / "frontend" / "uart_control.c"
DISK2_CARD_SV = REPO_ROOT / "hdl" / "apple" / "disk2_card.sv"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
from test_disk2_standard import encode_6and2, decode_6and2  # noqa: E402

WOZ_HEADER_SIZE = 12
WOZ_MAGIC2 = b"\xff\x0a\x0d\x0a"
WOZ_INFO_SIZE = 60
WOZ_TMAP_SIZE = 160
WOZ1_TRACK_BYTES = 6656
WOZ1_TRK_OFFSET = 6646
WOZ_EMPTY_TRACK_BYTES = 6400
WOZ_BLOCK_BYTES = 512
WOZ_TRKV2_BYTES = 8
WOZ_TRKV2_TABLE_BYTES = WOZ_TMAP_SIZE * WOZ_TRKV2_BYTES
WOZ_TMAP_EMPTY = 0xFF
APPLEWIN_RAND_MAX = 32767
APPLEWIN_RAND_3_10 = (APPLEWIN_RAND_MAX * 3) // 10
DISK2_TRACK_STREAM_BYTES = 8192


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def le16(value: int) -> bytes:
    return struct.pack("<H", value)


def le32(value: int) -> bytes:
    return struct.pack("<I", value)


def read_le16(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<H", data, offset)[0]


def read_le32(data: bytes | bytearray, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


def write_le16(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<H", data, offset, value)


def write_le32(data: bytearray, offset: int, value: int) -> None:
    struct.pack_into("<I", data, offset, value)


def chunk(name: bytes, payload: bytes) -> bytes:
    require(len(name) == 4, "chunk name must be 4 bytes")
    return name + le32(len(payload)) + payload


def info_payload(*,
                 version: int = 2,
                 disk_type: int = 1,
                 write_protected: int = 0,
                 optimal_bit_timing: int = 32) -> bytes:
    payload = bytearray(WOZ_INFO_SIZE)
    payload[0] = version
    payload[1] = disk_type
    payload[2] = write_protected
    payload[4:4 + 16] = b"Appletini tests "
    if version >= 2:
        payload[37] = 1
        payload[39] = optimal_bit_timing
    return bytes(payload)


def update_woz_crc(data: bytearray) -> None:
    crc = binascii.crc32(data[WOZ_HEADER_SIZE:]) & 0xFFFFFFFF
    data[8:12] = le32(crc)


def assert_woz_crc(data: bytes | bytearray) -> None:
    expected = read_le32(data, 8)
    actual = binascii.crc32(data[WOZ_HEADER_SIZE:]) & 0xFFFFFFFF
    require(expected == actual, f"CRC mismatch: expected {expected:08X}, got {actual:08X}")


def make_header(version: int) -> bytearray:
    require(version in (1, 2), "WOZ version must be 1 or 2")
    return bytearray(b"WOZ" + bytes([ord("0") + version]) + WOZ_MAGIC2 + b"\x00\x00\x00\x00")


def qtrack_to_tmap_index(qtrack: int) -> int:
    index = qtrack
    return min(index, WOZ_TMAP_SIZE - 1)


def fill_empty_track() -> bytes:
    state = 1
    out = bytearray(WOZ_EMPTY_TRACK_BYTES)
    for i in range(WOZ_EMPTY_TRACK_BYTES):
        value = 0
        for bit in range(8):
            state = ((state * 214013) + 2531011) & 0xFFFFFFFF
            if ((state >> 16) & APPLEWIN_RAND_MAX) < APPLEWIN_RAND_3_10:
                value |= 1 << bit
        out[i] = value
    return bytes(out)


def map_tmap_entry(tmap: bytearray, tmap_index: int, trk_index: int) -> None:
    tmap[tmap_index] = trk_index
    if tmap_index > 0:
        tmap[tmap_index - 1] = trk_index
    if tmap_index + 1 < WOZ_TMAP_SIZE:
        tmap[tmap_index + 1] = trk_index


def pack_woz_bits(stream: bytes, *, prefix_zero_bits: int = 0, ff_sync_zero_bits: int = 0) -> tuple[bytes, int]:
    bits = [0] * prefix_zero_bits
    for value in stream:
        for shift in range(7, -1, -1):
            bits.append((value >> shift) & 1)
        if value == 0xFF:
            bits.extend([0] * ff_sync_zero_bits)

    out = bytearray((len(bits) + 7) // 8)
    for index, bit in enumerate(bits):
        if bit:
            out[index >> 3] |= 1 << (7 - (index & 7))
    return bytes(out), len(bits)


def woz_bit_at(bits: bytes | bytearray, bit_pos: int) -> int:
    return (bits[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1


def applewin_lss_bytes(bits: bytes | bytearray, bit_count: int) -> bytes:
    out = bytearray()
    head_window = 0
    shift = 0
    latch_delay = 0

    for bit_pos in range(bit_count):
        head_window = ((head_window << 1) | woz_bit_at(bits, bit_pos)) & 0xF
        output_bit = ((head_window >> 1) & 1) if head_window else 0
        shift = ((shift << 1) | output_bit) & 0xFF

        if latch_delay:
            latch_delay = max(0, latch_delay - 4)
            if shift == 0:
                latch_delay += 4

        if not latch_delay:
            latch = shift
            if shift & 0x80:
                out.append(latch)
                latch_delay = 7
                shift = 0
    return bytes(out)


def applewin_lss_bytes_circular(bits: bytes | bytearray,
                                bit_count: int,
                                bit_cells: int) -> bytes:
    out = bytearray()
    head_window = 0
    shift = 0
    latch_delay = 0

    if bit_count <= 0:
        return bytes(out)

    for index in range(bit_cells):
        head_window = ((head_window << 1) | woz_bit_at(bits, index % bit_count)) & 0xF
        output_bit = ((head_window >> 1) & 1) if head_window else 0
        shift = ((shift << 1) | output_bit) & 0xFF

        if latch_delay:
            latch_delay = max(0, latch_delay - 4)
            if shift == 0:
                latch_delay += 4

        if not latch_delay:
            latch = shift
            if shift & 0x80:
                out.append(latch)
                latch_delay = 7
                shift = 0
    return bytes(out)


def applewin_lss_run(bits: bytes | bytearray,
                     bit_count: int,
                     bit_cells: int,
                     *,
                     start_offset: int = 0,
                     head_window: int = 0,
                     shift: int = 0,
                     latch_delay: int = 0,
                     latch: int = 0,
                     weak_bit: int = 0) -> tuple[bytes, int, int, int, int, int]:
    """Run AppleWin's WOZ LSS over a circular bitstream.

    This is a behavior model of Disk.cpp:DataLatchReadWOZ(), with deterministic
    weak bits for tests. It returns emitted latch bytes and the state that
    AppleWin carries across later WOZ accesses.
    """

    out = bytearray()
    if bit_count <= 0:
        return bytes(out), 0, head_window & 0xF, shift & 0xFF, latch_delay, latch & 0xFF

    bit_pos = start_offset % bit_count
    head_window &= 0xF
    shift &= 0xFF
    latch &= 0xFF

    for _ in range(bit_cells):
        head_window = ((head_window << 1) | woz_bit_at(bits, bit_pos)) & 0xF
        output_bit = ((head_window >> 1) & 1) if head_window else (weak_bit & 1)
        bit_pos = (bit_pos + 1) % bit_count

        shift = ((shift << 1) | output_bit) & 0xFF

        if latch_delay:
            latch_delay = max(0, latch_delay - 4)
            if shift == 0:
                latch_delay += 4

        if not latch_delay:
            latch = shift
            if shift & 0x80:
                out.append(latch)
                latch_delay = 7
                shift = 0

    return bytes(out), bit_pos, head_window, shift, latch_delay, latch


def applewin_shift_write(raw: bytes | bytearray,
                         bit_count: int,
                         start_offset: int,
                         latch_loads: list[tuple[int, int]],
                         *,
                         write_protected: bool = False) -> tuple[bytes, int]:
    """Run AppleWin's WOZ DataShiftWriteWOZ over a sequence of latch loads.

    Models Disk.cpp:1536-1576. Each (latch_byte, bit_cells) entry simulates
    a 6502 LDA-from-buffer + STA $C08D,X load (Q6=1,Q7=1) followed by `bit_cells`
    of write-shift activity (Q6=0,Q7=1) before the next load. Each bit cell
    emits the MSB of the shift register into the raw bit stream at the
    current bit_offset, then shifts the register left.

    When write_protected is True the bit stream is not modified, but
    bit_offset still advances by exactly the total bit cells, matching
    AppleWin's `if (floppy.m_bWriteProtected) { UpdateBitStreamPosition;
    return; }` early-return path (Disk.cpp:1545-1550).
    """
    if bit_count <= 0:
        return bytes(raw), 0

    out = bytearray(raw)
    bit_offset = start_offset % bit_count

    for latch_byte, bit_cells in latch_loads:
        shift_reg = latch_byte & 0xFF
        for _ in range(bit_cells):
            output_bit = (shift_reg >> 7) & 1
            shift_reg = (shift_reg << 1) & 0xFF

            if not write_protected:
                byte_idx = bit_offset >> 3
                bit_mask = 0x80 >> (bit_offset & 7)
                if output_bit:
                    out[byte_idx] |= bit_mask
                else:
                    out[byte_idx] &= (~bit_mask) & 0xFF

            bit_offset = (bit_offset + 1) % bit_count

    return bytes(out), bit_offset


# ----- AppleWin per-access session model (Disk.cpp:1338-1576, 2124-2247) -----

# Soft-switch low-nibble values on a Disk II card.
SOFT_C8C = 0xC  # Q6 LOW
SOFT_C8D = 0xD  # Q6 HIGH
SOFT_C8E = 0xE  # Q7 LOW
SOFT_C8F = 0xF  # Q7 HIGH


def applewin_woz_session(initial_bits: bytes | bytearray,
                         bit_count: int,
                         accesses: list[tuple[int, int, int | None]],
                         *,
                         initial_bit_offset: int = 0,
                         initial_shift_reg: int = 0,
                         write_protected: bool = False
                         ) -> tuple[bytes, int, int, int, bool]:
    """Simulate AppleWin's per-access burst-mode WOZ write state machine.

    accesses: list of (bit_cells_elapsed, addr_low_4_bits, write_byte_or_None).
        `bit_cells_elapsed` is the number of WHOLE bit cells (= 4 Apple
        cycles each at bit_timing=32) since the previous access or session
        start. Use whole bit cells so we don't have to model the
        m_extraCycles fractional accumulator.

    Returns (final_bits, final_bit_offset, final_shift_reg,
             final_floppy_latch, final_write_started).
    """
    bits = bytearray(initial_bits)
    if bit_count <= 0:
        return bytes(bits), 0, initial_shift_reg & 0xFF, 0, False

    bit_offset = initial_bit_offset % bit_count
    shift_reg = initial_shift_reg & 0xFF
    floppy_latch = 0
    write_started = False
    q6 = 0
    q7 = 0
    # AppleWin's GetBitCellDelta updates m_diskLastCycle on every call. If no
    # function calls it during an access, the elapsed cycles roll over to the
    # next access's GetBitCellDelta call. cells_pending models that carry.
    cells_pending = 0

    def emit_n(n: int) -> None:
        # DataShiftWriteWOZ inner loop (Disk.cpp:1562-1573).
        nonlocal shift_reg, bit_offset
        for _ in range(n):
            output_bit = (shift_reg >> 7) & 1
            shift_reg = (shift_reg << 1) & 0xFF
            if not write_protected:
                byte_idx = bit_offset >> 3
                bit_mask = 0x80 >> (bit_offset & 7)
                if output_bit:
                    bits[byte_idx] |= bit_mask
                else:
                    bits[byte_idx] &= (~bit_mask) & 0xFF
            bit_offset = (bit_offset + 1) % bit_count

    def advance_n(n: int) -> None:
        nonlocal bit_offset
        bit_offset = (bit_offset + n) % bit_count

    def reset_lss() -> None:
        nonlocal shift_reg, write_started
        shift_reg = 0
        write_started = False

    for cells_elapsed, addr_low, write_byte in accesses:
        old_q6, old_q7 = q6, q7
        cells_pending += cells_elapsed

        # Step 1: drain pending shifts if seqFunc was dataShiftWrite
        # (Disk.cpp:2165, 2212).
        if old_q6 == 0 and old_q7 == 1:
            emit_n(cells_pending)
            cells_pending = 0

        # Step 2: SetSequencerFunction updates writeMode/loadMode
        # (Disk.cpp:2131-2140). m_writeStarted resets when writeMode goes 0.
        if addr_low == SOFT_C8C: q6 = 0
        elif addr_low == SOFT_C8D: q6 = 1
        elif addr_low == SOFT_C8E: q7 = 0
        elif addr_low == SOFT_C8F: q7 = 1
        if q7 == 0:
            write_started = False
        # Leaving checkWriteProtAndInitWrite advances remaining cells without
        # running the LSS (Disk.cpp:2142-2152).
        if (old_q6 == 1 and old_q7 == 0) and not (q6 == 1 and q7 == 0):
            advance_n(cells_pending)
            cells_pending = 0

        # Step 3: dispatch on addr_low (Disk.cpp:2185-2187, 2232-2234).
        if addr_low == SOFT_C8D:
            # LoadWriteProtect (Disk.cpp:1895-1932). Always sets latch to WP
            # value; if writeStarted, early return; else (WOZ) advance bits +
            # ResetLogicStateSequencer.
            floppy_latch = 0xFF if write_protected else 0x00
            if not write_started:
                advance_n(cells_pending)
                cells_pending = 0
                reset_lss()
        # SetReadMode/SetWriteMode have no LSS effect beyond Q7 update.

        # Step 4: even-address READ runs DataLatchReadWriteWOZ when the new
        # seqFunc isn't dataShiftWrite (Disk.cpp:2191-2197). The read path
        # advances bit_offset by the elapsed cells (via DataLatchReadWOZ for
        # readSequencing or UpdateBitStreamPosition for
        # checkWriteProtAndInitWrite). For our session model we don't track
        # head_window/latch_delay; we just advance bit_offset.
        if write_byte is None and addr_low in (SOFT_C8C, SOFT_C8E):
            new_is_shift_write = (q6 == 0 and q7 == 1)
            if not new_is_shift_write:
                advance_n(cells_pending)
                cells_pending = 0

        # Step 5: IOWrite to dataLoadWrite mode loads the latch
        # (Disk.cpp:2238-2244).
        if write_byte is not None and q6 == 1 and q7 == 1:
            floppy_latch = write_byte & 0xFF
            # DataLoadWriteWOZ (Disk.cpp:1510-1534).
            if not write_protected:
                if not write_started:
                    advance_n(cells_pending)
                    cells_pending = 0
                write_started = True
                shift_reg = floppy_latch
            else:
                advance_n(cells_pending)
                cells_pending = 0

    return bytes(bits), bit_offset, shift_reg, floppy_latch, write_started


def applewin_stepper_result(phase: int, magnets: int, is_woz: bool = True) -> tuple[int, int]:
    """Return AppleWin's integer phase and precise head key after deferred step."""

    direction = 0
    if magnets & (1 << ((phase + 1) & 3)):
        direction += 1
    if magnets & (1 << ((phase + 3) & 3)):
        direction -= 1

    quarter_direction = 0
    if is_woz and magnets in (0xC, 0x6, 0x3, 0x9):
        quarter_direction = direction
        direction = 0

    next_phase = max(0, min(79, phase + direction))
    precise_x2 = (next_phase * 2) + quarter_direction
    if precise_x2 < 0:
        precise_x2 = 0
    return next_phase, precise_x2


def rotate_woz_bits(bits: bytes | bytearray, bit_count: int, bit_offset: int) -> bytes:
    out = bytearray((bit_count + 7) // 8)
    if bit_count <= 0:
        return bytes(out)
    bit_offset %= bit_count
    for index in range(bit_count):
        if woz_bit_at(bits, (bit_offset + index) % bit_count):
            out[index >> 3] |= 1 << (7 - (index & 7))
    return bytes(out)


def applewin_track_switch_bit_offset(old_bit_offset: int, old_bit_count: int, new_bit_count: int) -> int:
    if new_bit_count == 0:
        return 0
    if old_bit_count == 0:
        old_bit_count = 8
    offset = (old_bit_offset * new_bit_count) // old_bit_count
    offset += 7
    return 0 if offset >= new_bit_count else offset


def applewin_track_seam(bits: bytes | bytearray, bit_count: int) -> tuple[int, int]:
    bit_offset = 0
    shift_reg = 0
    zero_count = 0
    start_bit_offset = -1
    nibble_start_bit_offset = -1
    sync_ff_start_bit_offset = -1
    sync_ff_run_length = 0
    longest_sync_ff_start_bit_offset = -1
    longest_sync_ff_run_length = 0

    if bit_count <= 0:
        return 0, 0

    while True:
        output_bit = woz_bit_at(bits, bit_offset)
        bit_offset += 1
        if bit_offset == bit_count:
            bit_offset = 0

        if (start_bit_offset < 0 and bit_offset == 0) or bit_offset == start_bit_offset:
            break

        if shift_reg & 0x80:
            if output_bit == 0:
                zero_count += 1
                continue

            if shift_reg == 0xFF and zero_count == 2:
                if sync_ff_start_bit_offset < 0:
                    sync_ff_start_bit_offset = nibble_start_bit_offset
                sync_ff_run_length += 1

            if (shift_reg != 0xFF or zero_count != 2) and sync_ff_start_bit_offset >= 0:
                if start_bit_offset < 0:
                    start_bit_offset = nibble_start_bit_offset
                if longest_sync_ff_run_length < sync_ff_run_length:
                    longest_sync_ff_start_bit_offset = sync_ff_start_bit_offset
                    longest_sync_ff_run_length = sync_ff_run_length
                sync_ff_start_bit_offset = -1
                sync_ff_run_length = 0

            shift_reg = 0
            zero_count = 0

        shift_reg = ((shift_reg << 1) | output_bit) & 0xFF
        if shift_reg == 0x01:
            nibble_start_bit_offset = bit_count - 1 if bit_offset == 0 else bit_offset - 1

    if longest_sync_ff_run_length:
        return longest_sync_ff_start_bit_offset, longest_sync_ff_run_length
    return 0, 0


def make_woz1_track(payload: bytes, *, bit_count: int | None = None) -> bytes:
    require(len(payload) <= WOZ1_TRK_OFFSET, "WOZ1 test payload too large")
    track = bytearray(WOZ1_TRACK_BYTES)
    track[:len(payload)] = payload
    write_le16(track, WOZ1_TRK_OFFSET, len(payload))
    write_le16(track, WOZ1_TRK_OFFSET + 2, bit_count if bit_count is not None else len(payload) * 8)
    return bytes(track)


def make_woz1(entries: list[tuple[int, bytes, int | None]]) -> bytearray:
    tmap = bytearray([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE)
    trks = bytearray()
    for trk_index, payload, bit_count in entries:
        require(trk_index == len(trks) // WOZ1_TRACK_BYTES, "WOZ1 test tracks must be append ordered")
        map_tmap_entry(tmap, trk_index * 4, trk_index)
        trks += make_woz1_track(payload, bit_count=bit_count)
    data = make_header(1)
    data += chunk(b"INFO", info_payload(version=1))
    data += chunk(b"TMAP", bytes(tmap))
    data += chunk(b"TRKS", bytes(trks))
    update_woz_crc(data)
    return data


def make_empty_woz1() -> bytearray:
    data = make_header(1)
    data += chunk(b"INFO", info_payload(version=1))
    data += chunk(b"TMAP", bytes([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE))
    data += chunk(b"TRKS", b"")
    update_woz_crc(data)
    return data


def make_woz2(entries: list[tuple[int, int, bytes, int | None, int | None]]) -> bytearray:
    tmap = bytearray([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE)
    table = bytearray(WOZ_TRKV2_TABLE_BYTES)
    bits = bytearray()
    next_block = 3
    for trk_index, tmap_index, payload, bit_count, block_count in entries:
        padded = (len(payload) + WOZ_BLOCK_BYTES - 1) & ~(WOZ_BLOCK_BYTES - 1)
        if block_count is None:
            block_count = padded // WOZ_BLOCK_BYTES
        require(block_count * WOZ_BLOCK_BYTES >= len(payload), "WOZ2 block count too small")
        descriptor = trk_index * WOZ_TRKV2_BYTES
        write_le16(table, descriptor, next_block)
        write_le16(table, descriptor + 2, block_count)
        write_le32(table, descriptor + 4, bit_count if bit_count is not None else len(payload) * 8)
        map_tmap_entry(tmap, tmap_index, trk_index)
        padded_payload = bytearray(block_count * WOZ_BLOCK_BYTES)
        padded_payload[:len(payload)] = payload
        if block_count * WOZ_BLOCK_BYTES > len(payload):
            padded_payload[len(payload):] = bytes([0x77]) * (block_count * WOZ_BLOCK_BYTES - len(payload))
        bits += padded_payload
        next_block += block_count
    data = make_header(2)
    data += chunk(b"INFO", info_payload())
    data += chunk(b"TMAP", bytes(tmap))
    data += chunk(b"TRKS", bytes(table + bits))
    update_woz_crc(data)
    return data


def make_empty_woz2() -> bytearray:
    return make_woz2([])


class WozImage:
    """Small host model of disk2_service.c's WOZ read/write rules."""

    def __init__(self, data: bytes | bytearray):
        self.data = bytearray(data)
        self.version = 0
        self.read_only = False
        self.tmap_offset = 0
        self.trks_offset = 0
        self.trks_size = 0
        self.optimal_bit_timing = 32
        self.tmap = bytearray()
        self.parse()

    def parse(self) -> None:
        require(len(self.data) >= WOZ_HEADER_SIZE, "WOZ file too small")
        require(self.data[:3] == b"WOZ" and self.data[4:8] == WOZ_MAGIC2, "bad WOZ header")
        require(self.data[3] in (ord("1"), ord("2")), "unsupported WOZ version")
        self.version = self.data[3] - ord("0")
        pos = WOZ_HEADER_SIZE
        have_info = have_tmap = have_trks = False
        while pos + 8 <= len(self.data):
            name = bytes(self.data[pos:pos + 4])
            size = read_le32(self.data, pos + 4)
            next_pos = pos + 8 + size
            require(next_pos >= pos and next_pos <= len(self.data), "chunk extends past EOF")
            payload = pos + 8
            if name == b"INFO":
                require(not have_info and size >= WOZ_INFO_SIZE, "bad INFO chunk")
                require(self.data[payload + 1] == 1, "not a 5.25 WOZ")
                self.read_only = self.data[payload + 2] != 0
                info_version = self.data[payload]
                if info_version >= 2 and self.data[payload + 39] != 0:
                    self.optimal_bit_timing = self.data[payload + 39]
                have_info = True
            elif name == b"TMAP":
                require(not have_tmap and size >= WOZ_TMAP_SIZE, "bad TMAP chunk")
                self.tmap_offset = payload
                self.tmap = bytearray(self.data[payload:payload + WOZ_TMAP_SIZE])
                have_tmap = True
            elif name == b"TRKS":
                require(not have_trks, "duplicate TRKS chunk")
                self.trks_offset = payload
                self.trks_size = size
                have_trks = True
            pos = next_pos
        require(have_info and have_tmap and have_trks, "missing required WOZ chunk")
        if self.version == 1:
            require(self.trks_size % WOZ1_TRACK_BYTES == 0, "WOZ1 TRKS size is not track-aligned")
        else:
            require(self.trks_size >= WOZ_TRKV2_TABLE_BYTES, "WOZ2 TRKS missing descriptor table")

    def sync_tmap(self) -> None:
        self.data[self.tmap_offset:self.tmap_offset + WOZ_TMAP_SIZE] = self.tmap

    def update_trks_size(self, size: int) -> None:
        write_le32(self.data, self.trks_offset - 4, size)
        self.trks_size = size

    def write_crc(self) -> None:
        update_woz_crc(self.data)

    def read_qtrack_raw(self, qtrack: int) -> tuple[bytes, int]:
        tmap_index = qtrack_to_tmap_index(qtrack)
        trk_index = self.tmap[tmap_index]
        if trk_index == WOZ_TMAP_EMPTY:
            empty = fill_empty_track()
            return empty, len(empty) * 8
        require(trk_index < WOZ_TMAP_SIZE, "track index out of range")
        if self.version == 1:
            offset = self.trks_offset + trk_index * WOZ1_TRACK_BYTES
            require(offset + WOZ1_TRACK_BYTES <= len(self.data), "WOZ1 track extends past EOF")
            track = self.data[offset:offset + WOZ1_TRACK_BYTES]
            length = read_le16(track, WOZ1_TRK_OFFSET)
            if length == 0 or length > WOZ1_TRK_OFFSET:
                length = WOZ1_TRK_OFFSET
            bit_count = read_le16(track, WOZ1_TRK_OFFSET + 2)
            if bit_count == 0 or bit_count > length * 8:
                bit_count = length * 8
            return bytes(track[:length]), bit_count
        descriptor = self.trks_offset + trk_index * WOZ_TRKV2_BYTES
        start_block = read_le16(self.data, descriptor)
        block_count = read_le16(self.data, descriptor + 2)
        bit_count = read_le32(self.data, descriptor + 4)
        if start_block == 0 or block_count == 0 or bit_count == 0:
            empty = fill_empty_track()
            return empty, len(empty) * 8
        length = (bit_count + 7) // 8
        require(length <= block_count * WOZ_BLOCK_BYTES, "WOZ2 bit count exceeds blocks")
        offset = start_block * WOZ_BLOCK_BYTES
        require(offset + length <= len(self.data), "WOZ2 BITS extends past EOF")
        return bytes(self.data[offset:offset + length]), bit_count

    def read_qtrack(self, qtrack: int) -> bytes:
        raw, _ = self.read_qtrack_raw(qtrack)
        return raw

    def read_qtrack_lss(self, qtrack: int) -> bytes:
        raw, bit_count = self.read_qtrack_raw(qtrack)
        stream = applewin_lss_bytes(raw, bit_count)
        require(stream, "WOZ bitstream did not produce readable Disk II bytes")
        return stream

    def next_track_index(self) -> int:
        next_index = 0
        for mapped in self.tmap:
            if mapped != WOZ_TMAP_EMPTY:
                require(mapped < WOZ_TMAP_SIZE - 1, "no free WOZ track indexes")
                next_index = max(next_index, mapped + 1)
        return next_index

    def write_qtrack(self, qtrack: int, payload: bytes) -> None:
        require(not self.read_only, "cannot write write-protected WOZ")
        require(payload, "cannot write empty track")
        require(len(payload) <= 8192, "track exceeds PL stream buffer")
        tmap_index = qtrack_to_tmap_index(qtrack)
        trk_index = self.tmap[tmap_index]
        existing = trk_index != WOZ_TMAP_EMPTY
        if not existing:
            trk_index = self.next_track_index()
        require(trk_index < WOZ_TMAP_SIZE, "track index out of range")
        if self.version == 1:
            self.write_woz1(tmap_index, trk_index, existing, payload)
        else:
            self.write_woz2(tmap_index, trk_index, existing, payload)
        self.write_crc()

    def write_woz1(self, tmap_index: int, trk_index: int, existing: bool, payload: bytes) -> None:
        require(len(payload) <= WOZ1_TRK_OFFSET, "WOZ1 track payload too large")
        offset = self.trks_offset + trk_index * WOZ1_TRACK_BYTES
        if existing:
            require(offset + WOZ1_TRACK_BYTES <= len(self.data), "WOZ1 existing track outside file")
            track = bytearray(self.data[offset:offset + WOZ1_TRACK_BYTES])
            bytes_used = read_le16(track, WOZ1_TRK_OFFSET)
            bit_count = read_le16(track, WOZ1_TRK_OFFSET + 2)
            if bytes_used != len(payload) or bit_count == 0:
                bit_count = len(payload) * 8
        else:
            require(self.trks_offset + self.trks_size == len(self.data), "WOZ1 TRKS is not final")
            require(offset == len(self.data), "WOZ1 new track is not appendable")
            track = bytearray(WOZ1_TRACK_BYTES)
            bit_count = len(payload) * 8
            map_tmap_entry(self.tmap, tmap_index, trk_index)
            self.sync_tmap()
            self.data.extend(bytes(WOZ1_TRACK_BYTES))
            self.update_trks_size(self.trks_size + WOZ1_TRACK_BYTES)
        track[:len(payload)] = payload
        write_le16(track, WOZ1_TRK_OFFSET, len(payload))
        write_le16(track, WOZ1_TRK_OFFSET + 2, bit_count)
        self.data[offset:offset + WOZ1_TRACK_BYTES] = track

    def write_woz2(self, tmap_index: int, trk_index: int, existing: bool, payload: bytes) -> None:
        descriptor = self.trks_offset + trk_index * WOZ_TRKV2_BYTES
        require(descriptor + WOZ_TRKV2_BYTES <= self.trks_offset + self.trks_size,
                "WOZ2 descriptor outside TRKS")
        if existing:
            start_block = read_le16(self.data, descriptor)
            block_count = read_le16(self.data, descriptor + 2)
            bit_count = read_le32(self.data, descriptor + 4)
            require(start_block != 0 and block_count != 0, "bad WOZ2 descriptor")
            require(len(payload) <= block_count * WOZ_BLOCK_BYTES, "WOZ2 write exceeds existing blocks")
            if bit_count == 0 or ((bit_count + 7) // 8) != len(payload):
                bit_count = len(payload) * 8
            write_le32(self.data, descriptor + 4, bit_count)
            write_len = len(payload)
        else:
            old_file_size = len(self.data)
            require(self.trks_offset + self.trks_size == len(self.data), "WOZ2 TRKS is not final")
            padded_len = (len(payload) + WOZ_BLOCK_BYTES - 1) & ~(WOZ_BLOCK_BYTES - 1)
            aligned_file_size = (len(self.data) + WOZ_BLOCK_BYTES - 1) & ~(WOZ_BLOCK_BYTES - 1)
            self.data.extend(bytes(aligned_file_size - len(self.data)))
            start_block = aligned_file_size // WOZ_BLOCK_BYTES
            block_count = padded_len // WOZ_BLOCK_BYTES
            write_le16(self.data, descriptor, start_block)
            write_le16(self.data, descriptor + 2, block_count)
            write_le32(self.data, descriptor + 4, len(payload) * 8)
            map_tmap_entry(self.tmap, tmap_index, trk_index)
            self.sync_tmap()
            self.data.extend(bytes(padded_len))
            self.update_trks_size(self.trks_size + (aligned_file_size - old_file_size) + padded_len)
            write_len = padded_len
        data_offset = start_block * WOZ_BLOCK_BYTES
        if write_len > len(payload):
            block = bytearray(write_len)
            block[:len(payload)] = payload
            self.data[data_offset:data_offset + write_len] = block
        else:
            self.data[data_offset:data_offset + len(payload)] = payload


def test_c_constants_still_match() -> None:
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    hdl_source = DISK2_CARD_SV.read_text(encoding="utf-8")
    expected = {
        "DISK2_REG_TRACK_BIT_OFFSET": 0x0F,
        "DISK2_REG_TRACK_BIT_TIMING": 0x15,
        "DISK2_REG_TRACK_SEAM": 0x16,
        "DISK2_TRACK_STREAM_BYTES": DISK2_TRACK_STREAM_BYTES,
        "DISK2_WOZ_HEADER_SIZE": WOZ_HEADER_SIZE,
        "DISK2_WOZ_INFO_CHUNK_SIZE": WOZ_INFO_SIZE,
        "DISK2_WOZ_INFO_OPTIMAL_BIT_TIMING_OFFSET": 39,
        "DISK2_WOZ1_TRACK_BYTES": WOZ1_TRACK_BYTES,
        "DISK2_WOZ1_TRK_OFFSET": WOZ1_TRK_OFFSET,
        "DISK2_WOZ_EMPTY_TRACK_BYTES": WOZ_EMPTY_TRACK_BYTES,
        "DISK2_WOZ_BLOCK_BYTES": WOZ_BLOCK_BYTES,
        "DISK2_WOZ_TRKV2_BYTES": WOZ_TRKV2_BYTES,
        "DISK2_APPLEWIN_RAND_MAX": APPLEWIN_RAND_MAX,
        "DISK2_APPLEWIN_RAND_3_10": APPLEWIN_RAND_3_10,
    }
    for name, value in expected.items():
        match = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)U", source)
        require(match is not None, f"{name} not found in disk2_service.c")
        require(int(match.group(1), 0) == value, f"{name} changed in C without updating tests")
    for name in (
        "prepare_woz_track_stream",
        "prepare_standard_track_stream",
        "prepare_track_stream",
        "disk2_service_wozscan_loaded_track",
        "disk2_service_wozscan_file_track",
    ):
        require((f"static int {name}" in source) or (f"int {name}" in source),
                f"{name} missing; WOZ and standard track preparation should stay split")
    require("stream.raw_bits ? DISK2_TRACK_INFO_RAW_BITS_BIT : 0U" in source,
            "load_track should commit raw-bit mode from prepared stream metadata")
    require("qtrack_to_track" in source and "/ 4U" in source,
            "standard images should map the PL qtrack to whole tracks in PS")
    require("woz_qtrack_to_tmap_index" in source,
            "WOZ TMAP selection should remain PS-owned")
    require("DISK2_WOZ_SCAN_SEAM_BITS 256U" in source and
            "bit_cells = bit_count + DISK2_WOZ_SCAN_SEAM_BITS;" in source and
            "woz_raw_bit_at(raw, bit_pos % bit_count)" in source,
            "WOZ scan diagnostics should decode one circular track plus a seam window")
    load_match = re.search(r"static int load_track\(.*?^}\n\nstatic int read_loaded_track_ddr",
                           source, re.S | re.M)
    require(load_match is not None, "load_track body not found")
    load_body = load_match.group(0)
    require("DISK2_LOAD_STALE_REQUEST" in source,
            "PS track loading must detect and discard stale head requests")
    require("track_request_matches(drive, qtrack)" in load_body,
            "load_track should revalidate the live PL request before committing a track")
    require(load_body.find("disk2_reg_write(DISK2_REG_TRACK_INFO, 0U)") <
            load_body.find("rc = stage_track_to_ddr(stream.length)"),
            "load_track should invalidate PL track metadata before overwriting DDR staging")
    for name in (
        "track_woz_q",
        "woz_head_window_q",
        "woz_latch_delay_q",
        "woz_bit_cell_tick",
        "woz_seam_arm_q",
        "woz_write_pending_q",
    ):
        require(name in hdl_source,
                f"{name} missing; WOZ must remain a runtime raw-bit LSS path in PL")
    require("woz_lss_suspended_q" not in hdl_source and
            "WOZ_SIGNIFICANT_BIT_CELLS" not in hdl_source,
            "WOZ LSS must run continuously instead of using first-100-cell idle suspension")
    require("else if (woz_read_mode) begin" in hdl_source,
            "WOZ read LSS path should not be gated off by idle suspension")
    for name in (
        "D2_REG_TRACK_BIT_COUNT",
        "D2_REG_TRACK_BIT_OFFSET",
        "D2_REG_TRACK_BIT_TIMING",
        "D2_REG_TRACK_SEAM",
    ):
        require(name in hdl_source,
                f"{name} missing from disk2_card.sv; raw WOZ metadata is required")


def test_woz1_read_existing_and_empty_track() -> None:
    payload = bytes(0x80 | i for i in range(64))
    image = WozImage(make_woz1([(0, payload, 509)]))
    raw, bit_count = image.read_qtrack_raw(0)
    require(raw == payload, "WOZ1 mapped track did not stage raw BITS")
    require(bit_count == 509, "WOZ1 mapped track did not preserve bitCount")
    empty = image.read_qtrack(5)
    require(len(empty) == WOZ_EMPTY_TRACK_BYTES, "WOZ1 empty track length is wrong")
    require(empty[:8] == bytes.fromhex("05 84 CD 41 04 A0 02 AB"),
            "WOZ empty track does not match AppleWin's srand(1) pattern")
    require(empty != bytes([0xFF]) * len(empty), "WOZ empty track should not be all FF")


def test_woz_read_stages_raw_bits_and_lss_recovers_unaligned_stream() -> None:
    stream = b"\xFF\xFF\xD5\xAA\x96\xDE\xAA\xEB\xFF\xD5\xAA\xAD\x96\xAA"
    raw, bit_count = pack_woz_bits(stream, prefix_zero_bits=3, ff_sync_zero_bits=2)
    require(raw != stream, "test setup should not be a byte-aligned raw stream")
    require(raw.find(b"\xD5\xAA\x96") < 0, "raw shifted WOZ data should hide the address prologue")

    image1 = WozImage(make_woz1([(0, raw, bit_count)]))
    require(image1.read_qtrack(0) == raw, "WOZ1 read path must stage raw BITS, not converted bytes")
    require(image1.read_qtrack_lss(0).startswith(stream[:-1]),
            "WOZ1 AppleWin LSS model lost the prologue stream")

    image2 = WozImage(make_woz2([(0, 0, raw, bit_count, None)]))
    require(image2.read_qtrack(0) == raw, "WOZ2 read path must stage raw BITS, not converted bytes")
    require(image2.read_qtrack_lss(0).startswith(stream[:-1]),
            "WOZ2 AppleWin LSS model lost the prologue stream")


def test_woz_protected_sync_stream_survives_rotational_offset() -> None:
    sector = b"\xFF\xFF\xFF\xD5\xAA\x96\xFE\xED\xBE\xEF\xDE\xAA\xEB"
    sector += b"\xFF\xFF\xD5\xAA\xAD" + bytes([0x96, 0xCB, 0xA7, 0xFF])
    stream = sector * 4
    raw, bit_count = pack_woz_bits(stream, prefix_zero_bits=5, ff_sync_zero_bits=2)
    require(raw.find(b"\xD5\xAA\x96") < 0,
            "test setup should require LSS recovery rather than raw byte search")

    offset = applewin_track_switch_bit_offset(0, 0, bit_count)
    rotated = rotate_woz_bits(raw, bit_count, offset)
    recovered = applewin_lss_bytes_circular(rotated, bit_count, bit_count * 2)
    require(recovered.count(b"\xD5\xAA\x96") >= 3,
            "AppleWin LSS should keep finding address prologues after a +7 rotational offset")
    require(recovered.count(b"\xD5\xAA\xAD") >= 3,
            "AppleWin LSS should keep finding data prologues after a +7 rotational offset")


def test_applewin_circular_lss_catches_prologues_across_track_seam() -> None:
    stream = b"\xFF\xFF\xD5\xAA\x96\xDE\xAA\xEB\xFF\xFF\xD5\xAA\xAD\x96\xAA\xFF"
    raw, bit_count = pack_woz_bits(stream, prefix_zero_bits=3, ff_sync_zero_bits=2)
    found_seam_case = False

    for offset in range(1, bit_count):
        rotated = rotate_woz_bits(raw, bit_count, offset)
        one_revolution = applewin_lss_bytes(rotated, bit_count)
        circular = applewin_lss_bytes_circular(rotated, bit_count, bit_count * 2)
        if (one_revolution.count(b"\xD5\xAA\x96") == 0 and
                circular.count(b"\xD5\xAA\x96") >= 1):
            found_seam_case = True
            break

    require(found_seam_case,
            "WOZ diagnostics must account for AppleWin's circular LSS across the track seam")


def test_applewin_lss_state_survives_same_track_retag() -> None:
    sector = b"\xFF\xFF\xD5\xAA\x96\xFE\xED\xBE\xEF\xDE\xAA\xEB"
    sector += b"\xFF\xFF\xD5\xAA\xAD\x96\xCB\xA7\xFF"
    raw, bit_count = pack_woz_bits(sector * 6, prefix_zero_bits=7, ff_sync_zero_bits=2)

    split = bit_count // 3
    first, pos, head, shift, delay, latch = applewin_lss_run(raw, bit_count, split)
    second, *_ = applewin_lss_run(
        raw,
        bit_count,
        bit_count,
        start_offset=pos,
        head_window=head,
        shift=shift,
        latch_delay=delay,
        latch=latch)
    continuous, *_ = applewin_lss_run(raw, bit_count, split + bit_count)
    reset_second, *_ = applewin_lss_run(raw, bit_count, bit_count, start_offset=pos)

    require(first + second == continuous,
            "AppleWin carries WOZ shift/latch/head state across accesses to the same raw track")
    require(reset_second != second,
            "Resetting WOZ LSS during a same-track retag changes the AppleWin byte stream")


def test_applewin_check_mode_advances_bits_without_shifting_lss() -> None:
    stream = b"\xFF\xFF\xD5\xAA\x96\xDE\xAA\xEB\xFF\xD5\xAA\xAD\x96\xCB\xA7\xFF"
    raw, bit_count = pack_woz_bits(stream * 3, prefix_zero_bits=5, ff_sync_zero_bits=2)
    found_difference = False

    for skipped_cells in range(1, 64):
        applewin_read, *_ = applewin_lss_run(
            raw,
            bit_count,
            160,
            start_offset=skipped_cells)
        _, pos, head, shift, delay, latch = applewin_lss_run(
            raw,
            bit_count,
            skipped_cells)
        bad_read, *_ = applewin_lss_run(
            raw,
            bit_count,
            160,
            start_offset=pos,
            head_window=head,
            shift=shift,
            latch_delay=delay,
            latch=latch)
        if applewin_read != bad_read:
            found_difference = True
            break

    require(found_difference,
            "AppleWin advances the WOZ bit position in check mode without shifting the LSS")


def test_applewin_large_gap_keeps_only_significant_cells() -> None:
    stream = b"\xFF\xD5\xAA\x96\xDE\xAA\xEB\xFF\xD5\xAA\xAD\x96\xCB\xA7\xFF"
    raw, bit_count = pack_woz_bits(stream * 5, prefix_zero_bits=4, ff_sync_zero_bits=2)

    gap_cells = 430
    significant_cells = 100
    applewin_read, *_ = applewin_lss_run(
        raw,
        bit_count,
        significant_cells,
        start_offset=(gap_cells - significant_cells) % bit_count)
    continuous_read, *_ = applewin_lss_run(raw, bit_count, gap_cells)

    require(applewin_read != continuous_read,
            "AppleWin skips old WOZ bit cells after a long non-disk gap")


def test_applewin_track_switch_bit_offset() -> None:
    require(applewin_track_switch_bit_offset(0, 0, 52480) == 7,
            "initial WOZ track load should use AppleWin's +7 bit offset")
    require(applewin_track_switch_bit_offset(1000, 50000, 52500) == 1057,
            "WOZ track switch should scale rotational position before +7")
    require(applewin_track_switch_bit_offset(49999, 50000, 50000) == 0,
            "WOZ track switch should wrap when +7 passes the new bit count")


def test_applewin_track_seam_detection() -> None:
    raw, bit_count = pack_woz_bits(
        b"\xD5\xAA" + bytes([0xFF]) * 6 + b"\xD5\xAA\xAD",
        ff_sync_zero_bits=2)
    start, run = applewin_track_seam(raw, bit_count)
    require((start, run) == (16, 6),
            "WOZ seam detection must match AppleWin's longest FF/10 scan")


def test_applewin_deferred_stepper_half_phase_vectors() -> None:
    vectors = [
        (0, 0xC, (0, 0)),
        (1, 0xC, (1, 3)),
        (2, 0x6, (2, 3)),
        (3, 0x9, (3, 7)),
        (10, 0x8, (11, 22)),
        (10, 0x2, (9, 18)),
        (0, 0x8, (0, 0)),
        (79, 0x1, (79, 158)),
    ]

    for phase, magnets, expected in vectors:
        require(applewin_stepper_result(phase, magnets) == expected,
                f"AppleWin deferred stepper vector mismatch phase={phase} magnets={magnets:X}")


def test_woz_info_optimal_bit_timing_matches_applewin() -> None:
    data = make_header(2)
    data += chunk(b"INFO", info_payload(optimal_bit_timing=36))
    data += chunk(b"TMAP", bytes([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE))
    data += chunk(b"TRKS", bytes(WOZ_TRKV2_TABLE_BYTES))
    update_woz_crc(data)
    image = WozImage(data)
    require(image.optimal_bit_timing == 36,
            "WOZ2 INFO optimalBitTiming should be preserved for PL bit timing")

    image_v1 = WozImage(make_empty_woz1())
    require(image_v1.optimal_bit_timing == 32,
            "WOZ1 should use AppleWin's fixed 5.25 optimal bit timing")


def test_woz1_write_existing_preserves_bit_count_and_crc() -> None:
    image = WozImage(make_woz1([(0, bytes([0xC1]) * 32, 257)]))
    before_crc = read_le32(image.data, 8)
    replacement = bytes([0xC2]) * 32
    image.write_qtrack(0, replacement)
    assert_woz_crc(image.data)
    require(read_le32(image.data, 8) != before_crc, "WOZ1 CRC did not change after write")
    require(image.read_qtrack(0) == replacement, "WOZ1 existing write did not persist")
    meta = image.trks_offset + WOZ1_TRK_OFFSET
    require(read_le16(image.data, meta) == len(replacement), "WOZ1 bytesUsed not updated")
    require(read_le16(image.data, meta + 2) == 257, "WOZ1 bitCount should be preserved")


def test_woz1_write_new_track_appends_and_maps_neighbors() -> None:
    image = WozImage(make_empty_woz1())
    original_size = len(image.data)
    payload = b"\xD5\xAA\x96\xFF\xD5\xAA\xAD"
    image.write_qtrack(8, payload)
    assert_woz_crc(image.data)
    require(len(image.data) == original_size + WOZ1_TRACK_BYTES, "WOZ1 new track did not append")
    require(image.trks_size == WOZ1_TRACK_BYTES, "WOZ1 TRKS size was not extended")
    tmap_index = qtrack_to_tmap_index(8)
    require(image.tmap[tmap_index - 1] == 0 and image.tmap[tmap_index] == 0 and image.tmap[tmap_index + 1] == 0,
            "WOZ1 new track did not map neighboring quarter tracks")
    require(image.read_qtrack(8) == payload, "WOZ1 new track readback failed")


def test_woz2_read_existing_uses_bit_count() -> None:
    payload = bytes(0x80 | ((i * 3) & 0x7F) for i in range(123))
    image = WozImage(make_woz2([(2, 0, payload, (len(payload) * 8) - 3, 13)]))
    readback, bit_count = image.read_qtrack_raw(0)
    require(readback == payload, "WOZ2 read did not stage full raw byte span")
    require(bit_count == (len(payload) * 8) - 3, "WOZ2 read did not preserve bitCount")


def test_woz2_write_existing_preserves_descriptor_and_padding() -> None:
    payload = bytes([0x91]) * 300
    image = WozImage(make_woz2([(3, 0, payload, len(payload) * 8, 2)]))
    descriptor = image.trks_offset + 3 * WOZ_TRKV2_BYTES
    original_descriptor = bytes(image.data[descriptor:descriptor + WOZ_TRKV2_BYTES])
    start = read_le16(image.data, descriptor) * WOZ_BLOCK_BYTES
    replacement = bytes([0xA2]) * 300
    image.write_qtrack(0, replacement)
    assert_woz_crc(image.data)
    require(bytes(image.data[descriptor:descriptor + WOZ_TRKV2_BYTES]) == original_descriptor,
            "WOZ2 existing write changed descriptor")
    require(image.read_qtrack(0) == replacement, "WOZ2 existing raw write did not persist")
    require(image.data[start + len(replacement):start + 512] == bytes([0x77]) * (512 - len(replacement)),
            "WOZ2 existing write should preserve block padding")


def test_woz2_write_existing_preserves_raw_bit_count() -> None:
    stream = b"\xFF\xD5\xAA\x96\xDE\xAA\xEB\xFF\xD5\xAA\xAD"
    raw, bit_count = pack_woz_bits(stream, prefix_zero_bits=5, ff_sync_zero_bits=2)
    image = WozImage(make_woz2([(4, 0, raw, bit_count, 1)]))
    require(len(raw) != len(stream), "test setup should force a stream/raw length difference")
    require(image.read_qtrack_lss(0).startswith(stream[:-1]),
            "WOZ2 setup did not decode to the expected stream")

    descriptor = image.trks_offset + 4 * WOZ_TRKV2_BYTES
    start_block = read_le16(image.data, descriptor)
    block_count = read_le16(image.data, descriptor + 2)
    replacement = bytearray(len(raw))
    replacement[:] = raw
    replacement[0] ^= 0x01
    image.write_qtrack(0, replacement)
    assert_woz_crc(image.data)
    require(read_le16(image.data, descriptor) == start_block, "WOZ2 write moved an existing track")
    require(read_le16(image.data, descriptor + 2) == block_count, "WOZ2 write changed existing block count")
    require(read_le32(image.data, descriptor + 4) == bit_count,
            "WOZ2 raw write should preserve existing bitCount when byte span is unchanged")
    require(image.read_qtrack(0) == bytes(replacement), "WOZ2 raw write did not read back")


def test_woz2_write_new_track_appends_zero_padded_blocks() -> None:
    image = WozImage(make_empty_woz2())
    original_size = len(image.data)
    payload = bytes(0x80 | ((i * 5) & 0x7F) for i in range(600))
    image.write_qtrack(12, payload)
    assert_woz_crc(image.data)
    descriptor = image.trks_offset
    start_block = read_le16(image.data, descriptor)
    block_count = read_le16(image.data, descriptor + 2)
    bit_count = read_le32(image.data, descriptor + 4)
    require(start_block == original_size // WOZ_BLOCK_BYTES, "WOZ2 new track start block is wrong")
    require(block_count == 2, "WOZ2 new track block count should be padded to 2 blocks")
    require(bit_count == len(payload) * 8, "WOZ2 new track bit count is wrong")
    require(len(image.data) == original_size + (block_count * WOZ_BLOCK_BYTES),
            "WOZ2 new track did not append padded block range")
    start = start_block * WOZ_BLOCK_BYTES
    require(bytes(image.data[start:start + len(payload)]) == payload, "WOZ2 new track payload mismatch")
    require(image.data[start + len(payload):start + block_count * WOZ_BLOCK_BYTES] ==
            bytes(block_count * WOZ_BLOCK_BYTES - len(payload)),
            "WOZ2 new track padding must be written as zeroes")
    tmap_index = qtrack_to_tmap_index(12)
    require(image.tmap[tmap_index - 1] == 0 and image.tmap[tmap_index] == 0 and image.tmap[tmap_index + 1] == 0,
            "WOZ2 new track did not map neighboring quarter tracks")


def test_woz2_write_new_track_after_unaligned_trks() -> None:
    tmap = bytes([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE)
    data = make_header(2)
    data += chunk(b"INFO", info_payload() + b"\x00")
    data += chunk(b"TMAP", tmap)
    data += chunk(b"TRKS", bytes(WOZ_TRKV2_TABLE_BYTES))
    update_woz_crc(data)
    image = WozImage(data)
    original_size = len(image.data)
    require(original_size % WOZ_BLOCK_BYTES != 0, "test image should start unaligned")
    payload = b"\xD5\xAA\x96\xDE"
    image.write_qtrack(4, payload)
    assert_woz_crc(image.data)
    descriptor = image.trks_offset
    start_block = read_le16(image.data, descriptor)
    block_count = read_le16(image.data, descriptor + 2)
    aligned_size = (original_size + WOZ_BLOCK_BYTES - 1) & ~(WOZ_BLOCK_BYTES - 1)
    require(start_block == aligned_size // WOZ_BLOCK_BYTES, "WOZ2 unaligned append start block is wrong")
    require(block_count == 1, "WOZ2 unaligned append block count is wrong")
    require(image.trks_size == WOZ_TRKV2_TABLE_BYTES + (aligned_size - original_size) + WOZ_BLOCK_BYTES,
            "WOZ2 unaligned append TRKS size is wrong")
    require(image.data[original_size:aligned_size] == bytes(aligned_size - original_size),
            "WOZ2 unaligned append did not zero pre-track padding")


def test_pl_woz_write_uses_post_access_q_state() -> None:
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("wire woz_q6_mode = woz_io_access ? q6_after_access : q6_q;" in source,
            "WOZ PL write path must decide the current cell mode from post-access Q6")
    require("wire woz_q7_mode = woz_io_access ? q7_after_access : q7_q;" in source,
            "WOZ PL write path must decide the current cell mode from post-access Q7")
    require("wire woz_write_mode =\n"
            "        (q7_q && !q6_q) ||\n"
            "        (woz_io_access && q7_after_access && !q6_after_access);" in source,
            "WOZ PL must write both while the pre-access AppleWin sequencer state is "
            "dataShiftWrite and on a dataLoadWrite-to-dataShiftWrite transition")
    require("(q6_q && !q7_q) ||\n        (woz_io_access &&\n         woz_q6_mode &&\n         !woz_q7_mode)" in source,
            "WOZ PL must consume a Q6-check-to-read transition when leaving AppleWin check mode")
    require("wire woz_read_mode =\n        !woz_load_write_protect_access &&\n        !woz_q7_mode &&\n        !woz_q6_mode;" in source,
            "WOZ PL must not shift a read bit during the write-protect/check transition cycle")
    require("wire selected_track_loaded =" in source and
            "wire exact_drive_loaded =\n        selected_track_loaded &&\n        (loaded_qtrack_q == current_qtrack);" in source and
            "function automatic logic woz_alias_hit(input logic [7:0] qtrack);" in source,
            "WOZ PL must separate exact head match from PS-published WOZ TMAP aliases")
    require("logic       woz_alias_drive_q [0:1];" in source and
            "woz_alias_drive_q[0] <= woz_alias_hit(drive_qtrack_q[0]);" in source and
            "wire woz_alias_loaded =\n        selected_track_loaded &&\n        woz_alias_drive_q[drive_select_q];" in source,
            "WOZ PL must register TMAP alias hits before the stream/prefetch hot path")
    require("wire active_drive_loaded = track_woz_q ? woz_alias_loaded : exact_drive_loaded;" in source,
            "WOZ PL must use PS-published TMAP aliases for loaded-track validity")
    require("wire stream_track_loaded = active_drive_loaded;" in source,
            "WOZ stream data and prefetch must not run on stale non-aliased tracks")
    require("woz_empty_track_fallback" not in source,
            "WOZ empty-track behavior must be generated by the PS stream, not synthesized in PL")
    require("wire woz_track_stream_ready = woz_alias_loaded;" in source,
            "WOZ PL must only stream PS-published TMAP aliases")
    require("wire woz_stream_active =" in source and
            "track_woz_q &&\n        woz_track_stream_ready;" in source,
            "WOZ bitstream must rotate only for mapped PS-provided streams")
    require("if (trk_index == DISK2_WOZ_TMAP_EMPTY) {\n        *length_out = fill_woz_empty_track(buf);" in
            DISK2_SERVICE_C.read_text(encoding="utf-8"),
            "PS service must provide AppleWin-style empty WOZ tracks as regular streams")
    require("(stream_track_loaded && stream_line_hit_q)" in source,
            "WOZ stream must not feed 0xFF while an adjacent selected-drive track is being retagged")
    require("if (stream_track_loaded && !write_req_q && !prefetch_req_q && !prefetch_resp_pending_q)" in source,
            "WOZ prefetch must continue during adjacent selected-drive track retags")
    require("wire woz_data_load_access =" in source,
            "WOZ PL write path must explicitly identify write-latch loads")
    require("if (woz_write_mode)" in source,
            "WOZ PL bit-cell writer must use the explicit AppleWin sequencer write-mode wire")
    require("if (!write_stall_v && !woz_data_load_access)" in source and
            "woz_shift_q <= {woz_shift_q[6:0], 1'b0};" in source,
            "WOZ PL must preserve a newly loaded write byte after finishing the previous write cell")
    require("write_stall_v ||\n"
            "                        (!woz_write_mode &&\n"
            "                         woz_latch_load_mode &&\n"
            "                         woz_write_started_q)" in source,
            "WOZ PL must hold bit offset while the loaded write byte is staged")
    require("woz_bit_accum_q <= woz_accum_plus_saturated;" in source and
            "AppleWin does not call GetBitCellDelta() while the" in source,
            "WOZ PL must carry dataLoadWrite elapsed time forward instead of "
            "dropping bit cells before the next dataShiftWrite")
    track_commit = source.split("track_loaded_q <= 1'b1;", 1)[1].split("prefetch_valid_q <= '0;", 1)[0]
    woz_commit = track_commit.split("if (as_common.wdata[2]) begin", 1)[1].split("end else begin", 1)[0]
    require("woz_head_window_q <= 4'd0;" in woz_commit and
            "woz_bit_accum_q <= 16'd0;" in woz_commit,
            "WOZ track commit must match AppleWin by resetting head window and timing remainder")
    require("woz_shift_q <= 8'h00;" not in woz_commit and
            "woz_latch_delay_q <= 4'd0;" not in woz_commit and
            "disk_latch_q <= 8'hFF;" not in woz_commit,
            "WOZ track commit must preserve AppleWin's shift register, latch delay, and latch byte")
    q6_high_block = source.split("IO_Q6_HIGH: begin", 1)[1].split("IO_Q7_LOW:", 1)[0]
    require("woz_head_window_q" not in q6_high_block,
            "WOZ write-protect/load-state access must not reset the AppleWin drive head window")
    require("woz_cached_valid_q" not in q6_high_block and "woz_cached_ready_q" not in q6_high_block,
            "WOZ write-protect/load-state access must not discard the active raw-bit cache")


def test_pl_write_info_preserves_ps_register_contract() -> None:
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("as_common.wdata[16] == write_dirty_drive_q" in source,
            "PS dirty clear still expects dirty drive at WRITE_INFO bit 16")
    require("as_common.wdata[15:8] == write_dirty_qtrack_q" in source,
            "PS dirty clear still expects dirty qtrack at WRITE_INFO bits 15:8")
    require("disk_latch_q,\n                        7'h00,\n                        write_dirty_drive_q,\n                        write_dirty_qtrack_q," in source,
            "WRITE_INFO readback must keep latch in bits 31:24, drive in bit 16, and qtrack in bits 15:8")


def test_ps_retags_same_woz_physical_track_without_restaging() -> None:
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    require("static uint8_t loaded_qtrack(uint32_t track_info)" in source,
            "PS service must decode the PL loaded qtrack from readback bits")
    require("static void publish_woz_alias_range(const disk2_image_info_t *info, uint8_t qtrack)" in source,
            "PS service must publish the WOZ TMAP alias range to PL")
    require("DISK2_REG_WOZ_ALIAS_RANGE" in source and "info->woz_tmap[i] == trk_index" in source,
            "WOZ alias publication must be based on the image TMAP, not a fixed nearby-track guess")
    require("woz_qtracks_share_image_track(info, loaded_qtrack(track_info), qtrack)" in source,
            "PS service must detect adjacent WOZ qtracks that map to the same TRK entry")
    require("publish_woz_alias_range(info, qtrack);\n        disk2_reg_write(DISK2_REG_TRACK_INFO, commit_word);" in source,
            "same-TRK WOZ retags must refresh aliases before committing track metadata")
    require("disk2_reg_write(DISK2_REG_TRACK_INFO, commit_word);" in source and
            "return 0;\n    }\n\n    memset(g_track_buf" in source,
            "same-TRK WOZ retags should commit metadata before the DDR staging path")
    require("read_loaded_track_ddr(g_scan_buf, length)" in source,
            "WOZ scan diagnostics should read the live PL staging region instead of service globals")
    match = re.search(
        r"woz_qtracks_share_image_track\(info, loaded_qtrack\(track_info\), qtrack\)"
        r".*?return 0;",
        source, re.S)
    require(match is not None,
            "same-TMAP retag block not found in load_track")
    block = match.group(0)
    require("write_info_has_pending(disk2_reg_read(DISK2_REG_WRITE_INFO))" in block and
            "DISK2_LOAD_STALE_REQUEST" in block,
            "same-TRK WOZ retag must defer while PL writeback is dirty or busy; "
            "AppleWin keeps the dirty live track image instead of discarding "
            "pending write bits during ReadTrack")


def test_standard_images_do_not_use_woz_alias_fast_path() -> None:
    """AppleWin keeps WOZ raw-bit tracks live across TMAP aliases, but treats
    standard DSK/DO/PO/NIB media as whole staged tracks. Standard media must
    not use the WOZ retag shortcut or publish qtrack alias windows, otherwise
    the PL can keep reading stale non-WOZ track data after a head move.
    """
    ps_source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    pl_source = DISK2_CARD_SV.read_text(encoding="utf-8")

    require("static uint8_t qtracks_share_image_track" not in ps_source and
            "return qtracks_share_image_track" not in ps_source,
            "standard and WOZ track-sharing tests must stay separate")
    require("if (info->format == DISK2_IMAGE_WOZ &&\n"
            "        (track_info & DISK2_TRACK_INFO_LOADED_BIT) != 0U &&" in ps_source,
            "the no-restage loaded-track fast path must be gated to WOZ only")
    require("if (stream.raw_bits != 0U) {\n"
            "        publish_woz_alias_range(info, qtrack);\n"
            "    } else {\n"
            "        publish_woz_alias_range(NULL, qtrack);\n"
            "    }" in ps_source,
            "standard prepared tracks must clear the WOZ alias range")
    require("wire active_drive_loaded = track_woz_q ? woz_alias_loaded : exact_drive_loaded;" in pl_source,
            "PL must require exact qtrack match for standard image streams")


def test_pl_write_fifo_accounts_simultaneous_push_pop_once() -> None:
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("write_fifo_pop_v =" in source and "write_fifo_push_v =" in source,
            "Disk II write FIFO must compute pop and push before updating counters")
    require("write_fifo_count_q <= write_fifo_count_q +\n                    {4'd0, write_fifo_push_v} -\n                    {4'd0, write_fifo_pop_v};" in source,
            "Disk II write FIFO count must update once for simultaneous push/pop")
    require("write_fifo_count_q <= write_fifo_count_q - 5'd1;" not in source,
            "Disk II write FIFO must not decrement count in a separate assignment")
    require("write_fifo_count_q <= write_fifo_count_q + 5'd1;" not in source,
            "Disk II write FIFO must not increment count in a separate assignment")


def test_pl_stepper_matches_applewin_deferred_motion() -> None:
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("wire stepper_io_access = (ab_io_read || ab_io_write) && (io_idx <= IO_PHASE3_ON);" in source,
            "Disk II stepper timer must be blocked only by another phase access")
    require("step_delay_q <= 4'd10;" in source,
            "Disk II stepper motion must be deferred by AppleWin's 10-cycle delay")
    require("adjacent_quick_off =" in source and "(addr_delta == 4'd2 || addr_delta == 4'd6)" in source,
            "Disk II stepper must cancel rapid adjacent phase-off pairs")
    require("stepper_result(" in source and "magnets == 4'hC" in source and
            "next_qtrack = phase + phase + 8'd1;" in source,
            "Disk II WOZ stepper must preserve AppleWin half-phase head positions")
    require("ab_read.sss_en && step_pending_q && !stepper_io_access" in source,
            "Disk II deferred stepper event must age during non-stepper I/O")


def test_rejects_bad_chunks_and_write_protect() -> None:
    bad = make_empty_woz2()
    write_le32(bad, WOZ_HEADER_SIZE + 4, len(bad) + 1)
    try:
        WozImage(bad)
    except TestFailure:
        pass
    else:
        raise TestFailure("parser accepted chunk past EOF")

    protected = make_header(2)
    protected += chunk(b"INFO", info_payload(write_protected=1))
    protected += chunk(b"TMAP", bytes([WOZ_TMAP_EMPTY] * WOZ_TMAP_SIZE))
    protected += chunk(b"TRKS", bytes(WOZ_TRKV2_TABLE_BYTES))
    update_woz_crc(protected)
    image = WozImage(protected)
    require(image.read_only, "write-protected WOZ was not marked read-only")
    try:
        image.write_qtrack(0, b"nope")
    except TestFailure:
        pass
    else:
        raise TestFailure("write-protected WOZ accepted a write")


def test_applewin_shift_write_round_trips_through_lss() -> None:
    """A nibble pattern written via AppleWin's WOZ shift-write loop and read
    back via the LSS must reproduce the written pattern.

    Pins down the DataShiftWriteWOZ <-> DataLatchReadWOZ round trip
    (Disk.cpp:1413, Disk.cpp:1536). The PL's bit-level write loop must
    produce a stream that the same PL's read loop recovers.

    Uses 10-bit sync FFs (8 bits of FF + 2 padding zeros) to resync the LSS
    to the newly-written nibble boundary, matching the way DOS 3.3 RWTS
    paces $C08D loads relative to $C08C reads.
    """
    pristine_stream = b"\xFF" * 96
    raw_init, bit_count = pack_woz_bits(pristine_stream, ff_sync_zero_bits=2)

    pristine_read = applewin_lss_bytes_circular(raw_init, bit_count, bit_count * 2)
    require(pristine_read.count(0xFF) >= 48,
            "pristine sync-FF track should read back as FFs through the LSS")

    latch_loads = [
        (0xFF, 10), (0xFF, 10), (0xFF, 10), (0xFF, 10),
        (0xD5, 8), (0xAA, 8), (0x96, 8),
        (0xFE, 8), (0xFF, 8), (0xFF, 8), (0xFF, 8),
        (0xDE, 8), (0xAA, 8), (0xEB, 8),
    ]
    total_bits = sum(cells for _, cells in latch_loads)

    written_raw, end_offset = applewin_shift_write(
        raw_init, bit_count, start_offset=11, latch_loads=latch_loads)

    require(written_raw != raw_init,
            "shift-write must modify the raw stream")
    require(end_offset == (11 + total_bits) % bit_count,
            f"shift-write must advance bit_offset by exactly {total_bits} bits")

    read_back = applewin_lss_bytes_circular(written_raw, bit_count, bit_count * 4)
    require(b"\xD5\xAA\x96\xFE" in read_back,
            "address prologue + volume should round-trip via the LSS")
    require(b"\xDE\xAA\xEB" in read_back,
            "address epilogue should round-trip via the LSS")


def test_applewin_shift_write_is_bit_addressed_not_byte_aligned() -> None:
    """The PL's write loop must update individual bits (mask-write), not whole
    bytes. Shifting a single byte onto a non-byte-aligned offset must leave
    the surrounding bits untouched.
    """
    raw = bytearray(b"\xFF" * 8)
    bit_count = 64

    written, end_offset = applewin_shift_write(
        raw, bit_count, start_offset=3, latch_loads=[(0x00, 8)])
    require(end_offset == 11, "single-byte write should advance bit_offset by 8")

    expected = bytearray(b"\xFF" * 8)
    expected[0] = 0xE0
    expected[1] = 0x1F
    require(bytes(written) == bytes(expected),
            "shift-write must mask-update bits without disturbing neighbors")


def test_load_track_same_tmap_retag_applies_woz_bit_offset_bump() -> None:
    """The same-TMAP retag path must apply the quarter-track offset bump.

    AppleWin applies +7 to bit_offset on every quarter-track move, including
    adjacent qtracks that share a TMAP entry (Disk.cpp:333-392).
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(
        r"woz_qtracks_share_image_track\(info, loaded_qtrack\(track_info\), qtrack\)"
        r".*?return 0;",
        source, re.S)
    require(match is not None,
            "same-TMAP retag block not found in load_track")
    block = match.group(0)
    require(
        "DISK2_REG_TRACK_BIT_OFFSET" in block,
        "same-TMAP retag must write the +7 bit_offset bump to PL "
        "(Disk.cpp:377-381)"
    )


def test_applewin_shift_write_round_trips_a_full_sector_data_field() -> None:
    """End-to-end: 256-byte sector → 6-and-2 GCR encode → AppleWin shift-write
    onto a sync-FF WOZ track → AppleWin LSS read → 6-and-2 decode → original
    256 bytes.

    Pins down the AppleWin write+read round trip for a real sector. Any
    bit-level error in the shift_write or lss_bytes models would corrupt
    the 6-and-2 decoding and surface as a test failure here.
    """
    sector = bytes(((i * 17) ^ ((i >> 3) * 13) ^ 0x5A) & 0xFF for i in range(256))
    encoded = encode_6and2(sector)
    require(len(encoded) == 343, "6-and-2 encoded sector should be 343 bytes")

    # ~50000 bits, comparable to a real 5.25" track. The full sector + headers
    # is ~2832 bits, well under the track length so writes don't wrap.
    pristine = b"\xFF" * 600
    raw_init, bit_count = pack_woz_bits(pristine, ff_sync_zero_bits=2)
    require(bit_count >= 4000,
            f"test track must be long enough for the write, got {bit_count}")

    sequence: list[tuple[int, int]] = []
    sequence += [(0xFF, 10)] * 4              # sync FF prologue
    sequence += [(b, 8) for b in b"\xD5\xAA\xAD"]  # data prologue
    sequence += [(b, 8) for b in encoded]          # 343 GCR bytes
    sequence += [(b, 8) for b in b"\xDE\xAA\xEB"]  # data epilogue

    written_raw, end_offset = applewin_shift_write(
        raw_init, bit_count, start_offset=0, latch_loads=sequence)
    require(written_raw != raw_init,
            "shift-write must modify the raw stream")
    expected_offset = sum(cells for _, cells in sequence) % bit_count
    require(end_offset == expected_offset,
            f"end_offset should be {expected_offset}, got {end_offset}")

    read_back = applewin_lss_bytes_circular(written_raw, bit_count, bit_count * 4)
    pos = read_back.find(b"\xD5\xAA\xAD")
    require(pos >= 0,
            "data prologue must be findable in LSS read after the write")
    decoded = decode_6and2(read_back[pos + 3:pos + 3 + 343])
    require(decoded == sector,
            "AppleWin write + LSS read round trip must preserve sector data "
            "byte-for-byte")


def test_applewin_shift_write_wraps_correctly_at_bit_count_boundary() -> None:
    """When a write straddles bit_offset = bit_count - 1, the bits must wrap
    to 0 cleanly. AppleWin's IncBitStream (Disk.cpp:1252-1271) wraps
    bit_offset and resets m_byte = 0; bit_mask = 1<<7.
    """
    raw = bytearray(b"\xFF" * 16)
    bit_count = 100  # wraps mid-byte

    written, end_offset = applewin_shift_write(
        raw, bit_count, start_offset=96, latch_loads=[(0xAA, 8)])
    require(end_offset == 4,
            f"writing 8 bits from offset 96 in a 100-bit track must end at "
            f"offset 4, got {end_offset}")
    for bit_index in range(8):
        bit_pos = (96 + bit_index) % 100
        expected = (0xAA >> (7 - bit_index)) & 1
        actual = woz_bit_at(written, bit_pos)
        require(actual == expected,
                f"bit {bit_index} (position {bit_pos}) expected {expected}, "
                f"got {actual}")


def test_applewin_shift_write_does_not_modify_write_protected_track() -> None:
    """AppleWin's DataShiftWriteWOZ (Disk.cpp:1545-1550) and DataLoadWriteWOZ
    (Disk.cpp:1517-1522) early-return without modifying floppy.m_trackimage
    when the disk is write-protected, advancing only m_bitOffset.

    The PL gates the actual byte-write on `!drive_read_only`, so for a WP
    disk no bits land in the DDR track buffer either.
    """
    payload = b"\xD5\xAA\x96\xFE\xFF\xDE\xAA\xEB"
    raw = b"\x55" * 32
    bit_count = 256

    written, end_offset = applewin_shift_write(
        raw, bit_count, start_offset=11,
        latch_loads=[(b, 8) for b in payload],
        write_protected=True)
    require(written == raw,
            "WP-protected write must not modify a single bit of the track")
    require(end_offset == (11 + 8 * len(payload)) % bit_count,
            "WP-protected write must still advance bit_offset by elapsed cells")


def test_applewin_session_first_load_advances_over_elapsed_cells() -> None:
    """AppleWin's DataLoadWriteWOZ (Disk.cpp:1524-1525) advances bit_offset by
    the elapsed bit cells on the FIRST call (when !m_writeStarted). This
    ensures the first byte written lands at the right rotational position
    instead of overwriting the cells the LSS would have shifted through.
    """
    raw = bytearray(b"\xFF" * 16)
    bit_count = 100

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),     # $C08F: enter write mode (Q7=1)
        (5, SOFT_C8D, 0xA5),     # 5 bit cells later: STA $C08D loads 0xA5
    ]
    bits, bit_offset, shift_reg, latch, write_started = applewin_woz_session(
        raw, bit_count, accesses)

    require(shift_reg == 0xA5, "shift_reg must hold the loaded byte after first load")
    require(write_started, "writeStarted must be true after first load")
    require(latch == 0xA5, "floppy latch must hold the loaded byte")
    require(bit_offset == 5,
            f"first load must advance bit_offset by elapsed cells (5), got "
            f"{bit_offset}")
    # And the 5 bits emitted before the load were shift_reg=0 zeros, written
    # at offsets 0..4 over the all-FF track.
    for i in range(5):
        require(woz_bit_at(bits, i) == 0,
                f"bit {i} should have been written as 0 (shift_reg pre-load was 0)")
    require(woz_bit_at(bits, 5) == 1,
            "bit 5 must remain 1 (first load advanced past it without writing)")


def test_applewin_session_subsequent_load_does_not_advance_bits() -> None:
    """After m_writeStarted is true, DataLoadWriteWOZ (Disk.cpp:1524) skips
    the UpdateBitStreamPosition call. The shift register is just reloaded;
    bit_offset only advances via the next dataShiftWrite drain.
    """
    raw = bytearray(b"\xFF" * 16)
    bit_count = 128

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),
        (3, SOFT_C8D, 0xAA),
        # Subsequent load WITHOUT going through dataShiftWrite first: should
        # only update shift_reg, not advance bit_offset.
        (4, SOFT_C8D, 0x96),
    ]
    bits, bit_offset, shift_reg, latch, write_started = applewin_woz_session(
        raw, bit_count, accesses)

    # First load advanced 3, second load was already in dataLoadWrite (no Q6
    # toggle to dataShiftWrite between them) so neither the drain nor a
    # second advance ran.
    require(bit_offset == 3,
            f"second back-to-back load in dataLoadWrite must not advance "
            f"bit_offset, got {bit_offset}")
    require(shift_reg == 0x96,
            "shift_reg must hold the second byte loaded")
    require(write_started, "writeStarted must remain set across loads")


def test_applewin_session_emits_loaded_byte_msb_first_during_shift_write() -> None:
    """A typical RWTS write loop alternates STA $C08D (load) with LDA $C08C
    (transition to dataShiftWrite, drain on next access). After 8 bit cells
    in dataShiftWrite, all 8 bits of the loaded byte should be on disk
    MSB-first.
    """
    raw = bytearray(b"\x00" * 16)
    bit_count = 100
    byte_to_write = 0xD5

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),                 # enter write mode, write_started=False
        (0, SOFT_C8D, byte_to_write),         # STA $C08D loads 0xD5; first load
        (0, SOFT_C8C, None),                  # LDA $C08C: transition to dataShiftWrite
        (8, SOFT_C8D, 0xFF),                  # 8 cells later: drain emits 8 bits of D5,
                                              # then load 0xFF
    ]
    bits, bit_offset, _shift_reg, _latch, _ws = applewin_woz_session(
        raw, bit_count, accesses)

    # The 8 bits of 0xD5 should appear at offsets 0..7 (no elapsed cells
    # before the first load, so first-load advance was 0).
    for i in range(8):
        expected = (byte_to_write >> (7 - i)) & 1
        actual = woz_bit_at(bits, i)
        require(actual == expected,
                f"bit {i} of loaded 0xD5 should be {expected}, got {actual}")
    require(bit_offset == 8,
            f"after writing one byte bit_offset should be 8, got {bit_offset}")


def test_applewin_session_counts_data_load_dwell_as_shift_write_time() -> None:
    """AppleWin does not call GetBitCellDelta() on the C08C transition from
    dataLoadWrite to dataShiftWrite. The next dataShiftWrite drain therefore
    includes cycles spent between the load and the shift-mode transition.
    """
    raw = bytearray(b"\x00" * 16)
    bit_count = 100
    byte_to_write = 0xA6

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),
        (0, SOFT_C8D, byte_to_write),
        (4, SOFT_C8C, None),
        (4, SOFT_C8D, 0xFF),
    ]
    bits, bit_offset, _shift_reg, _latch, _ws = applewin_woz_session(
        raw, bit_count, accesses)

    for i in range(8):
        expected = (byte_to_write >> (7 - i)) & 1
        actual = woz_bit_at(bits, i)
        require(actual == expected,
                f"bit {i} of loaded 0xA6 should include dataLoad dwell and "
                f"be {expected}, got {actual}")
    require(bit_offset == 8,
            f"dataLoad dwell plus shift dwell should write 8 bits, got "
            f"bit_offset={bit_offset}")


def test_applewin_session_q7_low_resets_write_started() -> None:
    """SetSequencerFunction (Disk.cpp:2139-2140) resets m_writeStarted when
    writeMode (Q7) goes to 0. The next data load (after Q7 returns high)
    must therefore advance bit_offset over the elapsed cells again, not
    silently skip them.
    """
    raw = bytearray(b"\xFF" * 32)
    bit_count = 256

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),         # enter write mode (Q6=0, Q7=1 = dataShiftWrite)
        (0, SOFT_C8D, 0xAA),         # STA $C08D: first load (advance 0); writeStarted=True
        (5, SOFT_C8E, None),         # $C08E: Q7=0; checkWP path advances bit_offset by 5;
                                     # SetSequencerFunction resets writeStarted.
        (3, SOFT_C8F, None),         # back to write mode; SetSequencerFunction's
                                     # checkWP-exit advance consumes 3 cells.
        (4, SOFT_C8D, 0x96),         # writeStarted=False (was reset on Q7=0);
                                     # LoadWriteProtect advances 4, reset_lss();
                                     # then DataLoadWriteWOZ loads 0x96, sets writeStarted.
    ]
    bits, bit_offset, shift_reg, latch, write_started = applewin_woz_session(
        raw, bit_count, accesses)

    require(shift_reg == 0x96, "shift_reg must hold the second byte")
    require(write_started, "writeStarted must be set after the second load")
    # Total advance: 5 (checkWP read advance) + 3 (checkWP exit advance) +
    # 4 (LoadWriteProtect first-load advance) = 12.
    require(bit_offset == 12,
            f"bit_offset should advance by 5+3+4=12 over the Q7-low cycle "
            f"(matches AppleWin re-resetting m_writeStarted), got {bit_offset}")


def test_applewin_session_write_protected_disk_does_not_modify_track() -> None:
    """DataShiftWriteWOZ (Disk.cpp:1545) and DataLoadWriteWOZ (Disk.cpp:1517)
    both early-return on write-protected disks without modifying
    floppy.m_trackimage. The bit_offset still advances.
    """
    pristine = b"\xD5\xAA\x96" + b"\xFE" * 13
    raw = bytearray(pristine)
    bit_count = 128

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8F, None),
        (0, SOFT_C8D, 0xA5),
        (0, SOFT_C8C, None),
        (16, SOFT_C8D, 0x96),
        (0, SOFT_C8C, None),
        (8, SOFT_C8E, None),
    ]
    bits, _bit_offset, _shift_reg, _latch, _ws = applewin_woz_session(
        raw, bit_count, accesses, write_protected=True)
    require(bytes(bits) == bytes(pristine),
            "WP disk must not have any bit modified by an attempted write "
            "session")


def test_pl_resets_write_started_on_q7_low() -> None:
    """AppleWin's SetSequencerFunction (Disk.cpp:2139-2140) resets
    m_writeStarted whenever Q7 (writeMode) goes to 0. The PL must mirror
    this: each time a soft-switch access leaves Q7 low, woz_write_started_q
    must be cleared.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require(
        "if (!q7_after_access)\n                    woz_write_started_q <= 1'b0;"
        in source,
        "PL must clear woz_write_started_q whenever a Disk II soft-switch "
        "access leaves Q7 low (Disk.cpp:SetSequencerFunction:2139-2140)"
    )


def test_pl_data_load_invalidates_track_seam_on_each_load() -> None:
    """AppleWin's DataLoadWriteWOZ (Disk.cpp:1533) sets
    floppy.m_longestSyncFFBitOffsetStart = -1 on every load, invalidating
    the track-seam metadata so AddTrackSeamJitter doesn't fire on freshly-
    written bits. The PL clears woz_seam_run_q to 0, which makes
    `woz_seam_run_q > 16'd110` false and disables the seam_arm path.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    match = re.search(r"if \(woz_data_load_access\) begin(.*?)end\b",
                      source, re.S)
    require(match is not None,
            "woz_data_load_access body not found in PL source")
    body = match.group(1)
    require("woz_seam_run_q <= 16'd0;" in body,
            "PL must zero woz_seam_run_q on every woz_data_load_access "
            "(matches Disk.cpp:1533: m_longestSyncFFBitOffsetStart = -1)")
    require("woz_seam_arm_q <= 1'b0;" in body,
            "PL must clear woz_seam_arm_q on every woz_data_load_access so "
            "an in-flight seam slip doesn't apply during a write")


def test_pl_woz_track_commit_preserves_write_started() -> None:
    """AppleWin's ReadTrack() preserves m_writeStarted; SetSequencerFunction()
    clears it only when Q7/write mode goes low (Disk.cpp:2139-2140). A WOZ
    same-track retag or track load must not turn the next dataLoadWrite into a
    first write, because that would skip elapsed cells and corrupt writes.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    match = re.search(
        r"D2_REG_TRACK_INFO: begin(.*?)endcase",
        source, re.S)
    require(match is not None, "TRACK_INFO register block not found")
    block = match.group(1)
    commit_match = re.search(
        r"track_loaded_q <= 1'b1;.*?if \(as_common\.wdata\[2\]\) begin(.*?)"
        r"end else begin",
        block,
        re.S)
    require(commit_match is not None, "WOZ TRACK_INFO commit branch not found")
    require("woz_write_started_q <= 1'b0;" not in commit_match.group(1),
            "WOZ TRACK_INFO commit must preserve writeStarted; only Q7-low "
            "soft-switch accesses may clear it")
    require("prefetch_resp_pending_q <= 1'b0;\n"
            "                            cache_patch_pending_q <= 1'b0;\n"
            "                            write_fifo_head_q <= 4'd0;\n"
            "                            write_fifo_tail_q <= 4'd0;\n"
            "                            write_fifo_count_q <= 5'd0;\n"
            "                            disk_write_pending_q <= 1'b0;\n"
            "                            woz_write_pending_q <= 1'b0;\n"
            "                            write_req_q <= 1'b0;" in block,
            "WOZ TRACK_INFO commit must clear resident-track writeback state "
            "before replacement track data becomes active")


def test_pl_writes_only_when_drive_not_read_only() -> None:
    """The PL bit-cell write path must gate the byte-write on
    `!drive_read_only` so a WP-protected WOZ image cannot be modified.
    The gate also prevents write-protected media from raising the dirty flag.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    match = re.search(
        r"if \(woz_write_mode\) begin(.*?)end else if \(woz_load_write_protect_access\)",
        source, re.S)
    require(match is not None, "WOZ write-mode tick block not found")
    block = match.group(1)
    require(
        "active_drive_loaded && !drive_read_only && cached_v[8]" in block,
        "WOZ write-mode tick must gate the bit-write on !drive_read_only "
        "(WP disks must never have their DDR track buffer modified)"
    )


def test_applewin_session_check_wp_to_write_mode_advances_pending_cells() -> None:
    """When transitioning out of checkWriteProtAndInitWrite (Q6=1, Q7=0)
    into ANY other mode, AppleWin's SetSequencerFunction
    (Disk.cpp:2142-2152) advances bit_offset by the elapsed cells via
    UpdateBitStreamPosition. E7 copy protection depends on this transition
    accounting for every cell processed during the write-protect check.
    """
    raw = bytearray(b"\xFF" * 32)
    bit_count = 256

    accesses: list[tuple[int, int, int | None]] = [
        (0, SOFT_C8E, None),         # Q7=0: enter readSequencing
        (0, SOFT_C8D, None),         # Q6=1: enter checkWriteProtAndInitWrite
        (10, SOFT_C8F, None),        # 10 cells later, Q7=1: exit checkWP via the
                                     # SetSequencerFunction transition path.
    ]
    bits, bit_offset, _shift_reg, _latch, _ws = applewin_woz_session(
        raw, bit_count, accesses)

    require(bit_offset == 10,
            f"checkWP-exit transition must advance bit_offset by 10 cells, "
            f"got {bit_offset}")
    # No bits should have been written -- this was a check-WP-then-prepare
    # sequence with no shift_reg load.
    require(bytes(bits) == bytes(b"\xFF" * 32),
            "no bits should be written during a check-WP / init-write "
            "transition (no shift_reg load happened)")


def test_pl_continuous_lss_matches_applewin_burst_for_wp_check_advance() -> None:
    """The PL's continuous LSS matches AppleWin's write-protect transition
    advance: while in WP-check (Q6=1, Q7=0) bit-cell ticks fire and run
    the seam_slip / next_bit_offset block, advancing bit_offset every
    bit cell. So when we exit WP-check, the cells have already been
    accounted for. AppleWin instead does an explicit
    UpdateBitStreamPosition burst at the transition.

    This test pins down that the PL DOES advance bit_offset during
    woz_load_write_protect_access (it reaches the seam_slip block; the
    inner read/write/load handlers are skipped but skip_bit_advance does
    NOT fire, so the bottom block ticks bit_offset by 1).
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    # The skip-advance gate must NOT include woz_load_write_protect_access:
    # bits must advance during WP-check so the transition accounts for them.
    skip_match = re.search(
        r"skip_bit_advance_v =\s*"
        r"write_stall_v \|\|\s*"
        r"\(!woz_write_mode &&\s*"
        r"woz_latch_load_mode &&\s*"
        r"woz_write_started_q\);",
        source)
    require(skip_match is not None,
            "skip_bit_advance must only fire for PL write backpressure or "
            "dataLoadWrite + writeStarted, NOT during "
            "checkWriteProtAndInitWrite -- otherwise bit_offset would freeze "
            "during WP-check")
    require("woz_load_write_protect_access" not in skip_match.group(0),
            "skip_bit_advance must not include the write-protect check mode")


def test_pl_clears_write_dirty_on_track_info_clear() -> None:
    """TRACK_INFO clear must discard dirty metadata before staged track data
    is replaced or ejected. Resident-track metadata paired with replacement
    bytes could flush data to the wrong disk location.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    match = re.search(
        r"D2_REG_TRACK_INFO: begin\s*\n\s*if \(as_common\.wstrb\[0\] && "
        r"as_common\.wdata\[0\] == 1'b0\) begin(.*?)end else if",
        source, re.S)
    require(match is not None,
            "TRACK_INFO clear branch (wdata[0]=0) not found")
    clear_branch = match.group(1)
    require(
        "write_dirty_q <= 1'b0;" in clear_branch,
        "PL must clear write_dirty_q in the TRACK_INFO clear branch -- "
        "staging must not retain dirty metadata for the resident track"
    )


def test_load_track_rechecks_write_pending_before_clearing_track_info() -> None:
    """Even with the PL safety net, PS load_track should defer when a new
    write came in during prepare_track_stream so the dirty data has a
    chance to be flushed (not just dropped). Re-checking dirty or busy just
    before the TRACK_INFO clear catches the race; returning
    DISK2_LOAD_STALE_REQUEST lets the next poll cycle flush first.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int load_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "load_track not found")
    body = match.group(0)
    parts = body.split("return 0;\n    }\n", 1)
    require(len(parts) == 2,
            "could not split same-TMAP fast path from full-stage path")
    full_stage = parts[1]

    clear_pos = full_stage.find("disk2_reg_write(DISK2_REG_TRACK_INFO, 0U)")
    require(clear_pos != -1, "full-stage path must clear TRACK_INFO")

    # Look at code that runs BEFORE the TRACK_INFO clear in the full-stage
    # path. It must check WRITE_INFO's dirty/busy bits and bail out if set.
    pre_clear = full_stage[:clear_pos]
    has_pending_check = (
        "write_info_has_pending" in pre_clear and
        "DISK2_WRITE_INFO_DIRTY_BIT" in source and
        "DISK2_WRITE_INFO_BUSY_BIT" in source and
        "DISK2_REG_WRITE_INFO" in pre_clear and
        "DISK2_LOAD_STALE_REQUEST" in pre_clear
    )
    require(has_pending_check,
            "load_track full-stage path must re-check WRITE_INFO's dirty/"
            "busy bits before clearing TRACK_INFO and return "
            "DISK2_LOAD_STALE_REQUEST if a write raced into the prepare "
            "window -- otherwise pending staged-track writes can be discarded by "
            "staging and the next flush sees a structurally shortened track")


def test_woz_mount_enables_writeback_when_image_and_file_allow() -> None:
    """Writable WOZ images should mount writable by default, while the INFO
    write-protect bit and filesystem open failures still force read-only.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    start = source.find("if (format == DISK2_IMAGE_WOZ) {")
    require(start != -1, "WOZ probe branch not found")
    end = source.find("\n    } else {", start)
    require(end != -1, "WOZ probe branch end not found")
    branch = source[start:end]
    require("g_woz_image_write_protected[drive] = info.read_only;" in branch,
            "WOZ INFO write-protect state must be preserved before checking "
            "filesystem writability")
    require("if (info.read_only == 0U)" in branch,
            "WOZ mount must only probe RW open for images not marked "
            "write-protected")
    require("FA_READ | FA_WRITE" in branch,
            "WOZ mount path must verify the file is writable before "
            "publishing it as rw")
    require("g_woz_write_enable[drive] = 1U;" in branch,
            "writable WOZ images should enable writeback by default")
    require("info.read_only = 1U;" in branch,
            "WOZ mount must fall back to read-only when RW open fails")


def test_woz_writeback_runtime_toggle_preserves_media_write_protect() -> None:
    """The runtime WOZ write toggle remains useful for disabling writes and
    must still refuse images marked write-protected by their INFO chunk.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    require("static uint8_t g_woz_write_enable[DISK2_DRIVE_COUNT];" in source,
            "WOZ writeback needs per-drive runtime enable state")
    require("g_woz_image_write_protected[drive] = info.read_only;" in source,
            "WOZ INFO write-protect state must be preserved separately from "
            "the effective read_only state")
    require("int disk2_service_set_woz_write_enable" in source,
            "WOZ writeback must be controllable by the service API")
    setter = source[source.find("int disk2_service_set_woz_write_enable"):]
    setter = setter[:setter.find("uint8_t disk2_service_get_woz_write_enable")]
    require("g_woz_image_write_protected[drive] != 0U" in setter,
            "runtime enable must still refuse images marked write-protected "
            "by their WOZ INFO chunk")
    require("FA_READ | FA_WRITE" in setter,
            "runtime enable must verify the mounted WOZ file is writable "
            "before publishing it as rw to the PL")


def test_woz_mount_clears_stale_idle_dirty_latch() -> None:
    """Mounting or clearing a drive must not leave a stale PL dirty latch for
    that drive. A stale latch can immediately force a structural reject on the
    next mount and make WOZ writeback appear permanently off.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    helper_match = re.search(
        r"static uint8_t clear_drive_dirty_if_idle\(.*?\n}\n\nstatic int stage_track_to_ddr",
        source,
        re.S)
    require(helper_match is not None,
            "drive clear path must have a helper for stale idle dirty latches")
    helper = helper_match.group(0)
    require("disk2_reg_read(DISK2_REG_WRITE_INFO)" in helper,
            "stale dirty clear helper must inspect PL WRITE_INFO")
    require("DISK2_WRITE_INFO_DIRTY_BIT" in helper and
            "DISK2_WRITE_INFO_BUSY_BIT" in helper,
            "stale dirty clear helper must distinguish dirty from busy")
    require("write_info_drive(write_info) != drive" in helper,
            "stale dirty clear helper must only clear the requested drive")
    require("ack_dirty_track(drive, write_info_qtrack(write_info))" in helper,
            "stale idle dirty latch must be acknowledged with the PL qtrack")

    clear_match = re.search(
        r"static void clear_drive\(uint8_t drive\)(.*?)\n}\n\nstatic int probe_woz",
        source,
        re.S)
    require(clear_match is not None, "clear_drive body not found")
    clear_body = clear_match.group(1)
    require("(void)clear_drive_dirty_if_idle(drive);" in clear_body,
            "clear_drive must clear stale idle dirty state before resetting "
            "software drive state")


def test_wozwrite_on_clears_disabled_stale_dirty_state() -> None:
    """After a safety reject or manual off, the user must be able to turn WOZ
    writes back on. If the PL still has an idle dirty bit for that disabled
    drive, the enable path should acknowledge it instead of returning -4.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    setter_match = re.search(
        r"int disk2_service_set_woz_write_enable\(.*?\n}\n\nuint8_t disk2_service_get_woz_write_enable",
        source,
        re.S)
    require(setter_match is not None,
            "disk2_service_set_woz_write_enable body not found")
    setter = setter_match.group(0)

    require("dirty_drive = write_info_drive(write_info);" in setter and
            "dirty_qtrack = write_info_qtrack(write_info);" in setter,
            "WOZ write toggle must decode dirty drive/qtrack through the "
            "shared WRITE_INFO helpers")
    require("g_woz_image_write_protected[drive] != 0U" in setter,
            "WOZ write enable must still refuse media write-protected images")
    require("g_woz_write_enable[drive] == 0U" in setter and
            "g_disk2_info[drive].read_only != 0U" in setter,
            "WOZ write enable must identify stale dirty state from a disabled "
            "software write path")
    require("(write_info & DISK2_WRITE_INFO_BUSY_BIT) == 0U" in setter,
            "WOZ write enable must not acknowledge a dirty latch while PL is "
            "actively writing it")
    require("ack_dirty_track(dirty_drive, dirty_qtrack);" in setter and
            "write_info = 0U;" in setter,
            "WOZ write enable must clear disabled stale dirty state before "
            "the generic dirty check")
    require("if ((write_info & DISK2_WRITE_INFO_DIRTY_BIT) != 0U) {\n"
            "        return -4;\n"
            "    }" in setter,
            "WOZ write enable must still refuse real pending dirty writes")


def test_uart_has_wozwrite_debug_command() -> None:
    """The WOZ write path should remain observable and controllable through
    an explicit UART command that names the drive and mode.
    """
    source = UART_CONTROL_C.read_text(encoding="utf-8")
    require("disk2 wozwrite d1|d2 <on|off|status>" in source,
            "UART help/usage must expose the explicit WOZ write debug command")
    require('str_ieq(argv[1], "wozwrite")' in source,
            "process_disk2_command must handle disk2 wozwrite")
    require("disk2_service_set_woz_write_enable(drive, enable)" in source,
            "disk2 wozwrite must use the service-level opt-in API")


def test_woz_flush_logging_captures_dirty_metadata() -> None:
    """When unsafe WOZ writes are enabled, successful flush attempts need
    enough metadata to diagnose corruption: dirty qtrack, length, bit count,
    write counter, WRITE_INFO/TRACK_INFO snapshots, and immediate readback
    verification against the bytes just flushed.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    require("Disk II WOZ flush #" in source,
            "WOZ write debug mode should log each flush attempt")
    require("Disk II WOZ verify" in source,
            "WOZ write debug mode should verify each successful flush by "
            "reading the qtrack back from the file")
    for token in [
        "qtrack",
        "length",
        "bit_count",
        "snap_write_count",
        "post_write_count",
        "post_write_info",
        "DISK2_REG_TRACK_INFO",
        "first_buffer_diff",
        "buffer_crc32",
        "read_woz_qtrack_as_stream(drive, qtrack, g_scan_buf",
    ]:
        require(token in source,
                f"WOZ flush logging must include {token}")


def test_woz_flush_refuses_structural_prologue_regressions() -> None:
    """WOZ write debugging must not let a bad PL capture silently destroy an
    image. Before writing a WOZ qtrack back, compare old/new address and data
    prologue counts and abort if the captured stream lost structure.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int flush_dirty_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "flush_dirty_track not found")
    body = match.group(0)
    require("Disk II WOZ structure" in source,
            "WOZ debug flush must log old/new prologue structure")
    require("analyze_woz_track(g_scan_buf, verify_bit_count" in body and
            "analyze_woz_track(g_track_buf, bit_count" in body,
            "WOZ flush must analyze both original and captured track streams")
    require("woz_after_scan.addr16_count < woz_before_scan.addr16_count" in body,
            "WOZ flush must reject lost address prologues")
    require("woz_after_scan.data_count < woz_before_scan.data_count" in body,
            "WOZ flush must reject lost data prologues")
    require("return DISK2_WOZ_STRUCTURE_REJECT;" in body,
            "WOZ flush must abort instead of writing a structurally regressed track")


def test_woz_structure_reject_is_acknowledged_once() -> None:
    """A structural WOZ reject is a permanent safety failure for that capture,
    not a transient race. The service must clear the PL dirty latch and disable
    WOZ writes for the drive; otherwise it logs the same rejected qtrack forever.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    require("#define DISK2_WOZ_STRUCTURE_REJECT (-22)" in source,
            "WOZ structural reject should be a named result code")
    require("static void ack_dirty_track(uint8_t drive, uint8_t qtrack)" in source,
            "dirty-track acknowledgement should be centralized")
    require("static void disable_woz_write_after_reject" in source,
            "WOZ structural reject should disable further write attempts")

    poll_match = re.search(
        r"void disk2_service_poll\(void\)(.*?)\n}\n\nint disk2_service_set_image_path",
        source, re.S)
    require(poll_match is not None, "disk2_service_poll body not found")
    poll_body = poll_match.group(1)
    require("rc == DISK2_WOZ_STRUCTURE_REJECT" in poll_body,
            "poll loop must handle permanent structural rejects specially")
    reject_index = poll_body.find("rc == DISK2_WOZ_STRUCTURE_REJECT")
    reject_body = poll_body[reject_index:reject_index + 320]
    require("ack_dirty_track(dirty_drive, dirty_qtrack)" in reject_body,
            "structural reject must acknowledge the dirty PL latch")
    require("disable_woz_write_after_reject(dirty_drive, dirty_qtrack)" in reject_body,
            "structural reject must disable unsafe WOZ writeback")

    disable_match = re.search(
        r"static void disable_woz_write_after_reject\(.*?\n}\n\nstatic int flush_dirty_track",
        source, re.S)
    require(disable_match is not None, "disable_woz_write_after_reject body not found")
    disable_body = disable_match.group(0)
    require("g_woz_write_enable[drive] = 0U;" in disable_body,
            "reject handler must turn off WOZ writeback")
    require("g_disk2_info[drive].read_only = 1U;" in disable_body,
            "reject handler must publish the drive as read-only")
    require("publish_drive(drive);" in disable_body,
            "reject handler must republish the read-only state")


def test_woz_dirty_flush_treats_same_tmap_alias_as_current() -> None:
    """AppleWin keeps the live WOZ track image active across quarter-track
    positions that map to the same TMAP/TRKS entry. The PS flush policy must
    not commit a dirty WOZ image just because the head retagged from q68 to
    q69 while the motor is still spinning; that captures half-written tracks.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    helper_match = re.search(
        r"static uint8_t dirty_track_is_current\(.*?\n}\n\nstatic void publish_woz_alias_range",
        source, re.S)
    require(helper_match is not None,
            "dirty_track_is_current helper not found")
    helper_body = helper_match.group(0)
    require("DISK2_TRACK_INFO_MATCH_BIT" in helper_body,
            "dirty-track current check must require the PL-loaded track to match")
    require("dirty_qtrack == current_qtrack" in helper_body,
            "dirty-track current check must preserve exact-qtrack behavior")
    require("woz_qtracks_share_image_track(info, dirty_qtrack, current_qtrack)" in helper_body,
            "WOZ dirty flush must treat same-TMAP qtrack aliases as the live current track")

    poll_match = re.search(
        r"void disk2_service_poll\(void\)(.*?)\n}\n\nint disk2_service_set_image_path",
        source, re.S)
    require(poll_match is not None, "disk2_service_poll body not found")
    poll_body = poll_match.group(1)
    require("dirty_track_is_current(track_info, dirty_drive, dirty_qtrack)" in poll_body,
            "poll loop must use the WOZ-aware dirty current helper before flushing")


def test_pl_woz_io_access_requires_loaded_woz_track() -> None:
    """woz_io_access must require the WOZ track to be staged
    (woz_track_stream_ready) so soft-switch accesses on a non-WOZ disk
    don't accidentally trigger WOZ-specific state changes (data-load,
    write-started reset, seam invalidation, etc.).
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require(
        "wire woz_io_access =\n"
        "        (ab_io_read || ab_io_write) &&\n"
        "        drive_spinning &&\n"
        "        track_woz_q &&\n"
        "        woz_track_stream_ready;" in source,
        "woz_io_access must combine ab_io, spinning, track_woz, and a "
        "loaded-track gate so it never fires on stale or non-WOZ media"
    )


def _existing_branch(body: str) -> str:
    """Extract the body of an `if (existing != 0U) { ... } else {` branch."""
    match = re.search(r"if \(existing != 0U\) \{(.*?)\n    \} else \{",
                      body, re.S)
    require(match is not None, "existing-track branch not found")
    return match.group(1)


def test_woz2_writeback_does_not_silently_rewrite_bit_count() -> None:
    """When the PS-staged byte length does not match (bit_count + 7) / 8 of
    the existing on-disk WOZ2 descriptor, AppleWin's WOZ2 writer asserts and
    refuses the write (DiskImageHelper.cpp:1380-1388: _ASSERT(0); return;).

    The appletini WOZ2 writer must not silently override bit_count with
    length * 8 in the existing-track branch, because that destroys the
    original sub-byte bit_count metadata (e.g., for tracks authored by
    Applesauce with non-byte-aligned bit counts). The new-track allocation
    branch is allowed to set bit_count = length * 8 since there is no
    pre-existing value to preserve.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int write_woz2_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "write_woz2_track not found")
    branch = _existing_branch(match.group(0))
    require(
        "bit_count = length * 8U;" not in branch,
        "WOZ2 existing-track writeback must reject mismatched length rather "
        "than rewriting bit_count (matches AppleWin's "
        "DiskImageHelper.cpp:1380-1388)"
    )


def test_woz1_writeback_does_not_silently_rewrite_bit_count() -> None:
    """Same as test_woz2_writeback_does_not_silently_rewrite_bit_count, but
    for the WOZ1 path. AppleWin's WOZ1 writer warns and continues
    (DiskImageHelper.cpp:1252-1259); we take the safer route of refusing
    the write so the on-disk metadata is never corrupted.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int write_woz1_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "write_woz1_track not found")
    branch = _existing_branch(match.group(0))
    require(
        "bit_count = (uint16_t)(length * 8U);" not in branch,
        "WOZ1 existing-track writeback must reject mismatched length rather "
        "than rewriting bit_count silently"
    )


def test_load_track_freezes_lss_before_capturing_old_bit_offset() -> None:
    """For WOZ track switches, load_track must clear TRACK_INFO (which
    freezes the LSS in PL) before reading DISK2_REG_TRACK_BIT_OFFSET, so
    the captured value is a stable snapshot rather than a moving target.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int load_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "load_track not found")
    body = match.group(0)
    # Find the full-stage path (after the same-TMAP fast path's `return 0`).
    parts = body.split("return 0;\n    }\n", 1)
    require(len(parts) == 2,
            "could not split same-TMAP fast path from full-stage path")
    full_stage = parts[1]
    clear_pos = full_stage.find("disk2_reg_write(DISK2_REG_TRACK_INFO, 0U)")
    read_pos = full_stage.find("disk2_reg_read(DISK2_REG_TRACK_BIT_OFFSET)")
    require(clear_pos != -1 and read_pos != -1,
            "full-stage path must clear TRACK_INFO and read BIT_OFFSET")
    require(clear_pos < read_pos,
            "TRACK_INFO clear must precede BIT_OFFSET read so the LSS is "
            "frozen at the moment of capture")


def test_prepare_woz_track_stream_does_not_read_pl_track_state() -> None:
    """The resident bit_offset must be captured by load_track after it has
    cleared TRACK_INFO, not by prepare_woz_track_stream which doesn't know
    when (or whether) the LSS is frozen. Likewise the resident bit_count must
    come from the PS-cached g_loaded_track_bit_count, not from re-reading
    the PL register.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(
        r"static int prepare_woz_track_stream\(.*?\n\}\n", source, re.S)
    require(match is not None, "prepare_woz_track_stream not found")
    body = match.group(0)
    require("DISK2_REG_TRACK_BIT_OFFSET" not in body,
            "prepare_woz_track_stream must not read DISK2_REG_TRACK_BIT_OFFSET "
            "(racy against the live LSS); load_track does it under a "
            "TRACK_INFO clear")
    require("DISK2_REG_TRACK_BIT_COUNT" not in body,
            "prepare_woz_track_stream must not read DISK2_REG_TRACK_BIT_COUNT "
            "(use the PS-cached g_loaded_track_bit_count instead)")


def test_flush_dirty_track_aborts_on_ddr_read_race() -> None:
    """flush_dirty_track must snapshot stream_write_count before the DDR
    read and re-check (busy + write_count) afterward. If either changed, a
    new PL write raced the read and we must abort so the next poll cycle
    retries with the current staging contents.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    match = re.search(r"static int flush_dirty_track\(.*?\n\}\n", source, re.S)
    require(match is not None, "flush_dirty_track not found")
    body = match.group(0)
    snap_pos = body.find("DISK2_REG_WRITE_COUNT")
    staging_pos = body.find("read_loaded_track_ddr")
    require(snap_pos != -1 and staging_pos != -1,
            "flush must reference DISK2_REG_WRITE_COUNT and "
            "read_loaded_track_ddr")
    require(snap_pos < staging_pos,
            "flush must snapshot DISK2_REG_WRITE_COUNT before reading DDR staging")
    after_staging = body[staging_pos:]
    require("DISK2_WRITE_INFO_BUSY_BIT" in after_staging,
            "flush must re-read DISK2_WRITE_INFO and check busy after the "
            "staging read")
    require(after_staging.count("DISK2_REG_WRITE_COUNT") >= 1,
            "flush must re-read DISK2_REG_WRITE_COUNT after the staging read "
            "and compare it to the snapshot")


def test_pl_keeps_per_drive_head_positions() -> None:
    """The PL must maintain independent head positions for drives 1 and 2,
    so that switching drives (via $C0EA / $C0EB) doesn't lose the inactive
    drive's qtrack.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("logic [7:0] drive_phase_q [0:1];" in source,
            "PL must store drive 1 and 2 m_phase positions separately")
    require("logic [7:0] drive_qtrack_q [0:1];" in source,
            "PL must store drive 1 and 2 quarter-track positions separately")
    require("drive_phase_q[drive_select_q]" in source and
            "drive_qtrack_q[drive_select_q]" in source,
            "stepper must update only the selected drive's head position")
    select_match = re.search(
        r"IO_DRIVE1: begin.*?end\s+IO_DRIVE2: begin.*?end",
        source, re.S)
    require(select_match is not None,
            "drive-select case arms not found")
    select_block = select_match.group(0)
    require("drive_phase_q" not in select_block and
            "drive_qtrack_q" not in select_block,
            "drive-select must not clobber either drive's head position")


def test_pl_pipelines_prefetch_cache_patch_for_timing() -> None:
    """Writes must not update the wide prefetch data array directly from the
    Apple bus access decode cone. Capturing a one-cycle cache patch keeps the
    Disk II write-through behavior but removes the failing addr-to-CE path.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("logic        cache_patch_pending_q;" in source and
            "logic [20:0] cache_patch_line_q;" in source and
            "logic [7:0]  cache_patch_byte_q;" in source and
            "logic [2:0]  cache_patch_offset_q;" in source,
            "Disk II cache patch must be captured in registered staging")
    require("if (cache_patch_pending_q) begin" in source and
            "prefetch_data_q[i] <= line_with_byte(" in source,
            "prefetch cache data must be patched only from the registered "
            "cache patch stage")
    require(source.count("prefetch_data_q[i] <= line_with_byte(") == 1,
            "write paths must not directly update prefetch_data_q from "
            "Apple bus decode")
    require(source.count("cache_patch_pending_q <= 1'b1;") >= 2,
            "both WOZ and standard Disk II writes must queue cache patches")


def test_pl_pipelines_woz_weak_random_refill_for_timing() -> None:
    """The weak-bit random refill must not recompute the LCG from the WOZ
    byte/bit hit path. The stream path should only request a refill; the next
    random value is calculated from registered state on the following clock.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("woz_weak_refill_rand_q" not in source,
            "WOZ weak random refill must not stage a wide value from the "
            "WOZ bit decode path")
    require("woz_weak_refill_stage_q" in source and
            "woz_weak_lcg_lo_q" in source and
            "woz_weak_lcg_hi_q" in source,
            "WOZ weak random refill must be split across registered stages")
    require("woz_weak_next_rand_q[15:0] * WOZ_WEAK_RAND_MUL18" in source and
            "woz_weak_next_rand_q[31:16] * WOZ_WEAK_RAND_MUL18" in source,
            "WOZ weak random LCG must compute split partial products")
    require("woz_weak_lcg_lo_q[31:0] +" in source and
            "{woz_weak_lcg_hi_q[15:0], 16'h0000} +" in source,
            "WOZ weak random LCG must combine registered partial products")
    require("woz_weak_next_rand_bit_q <= woz_weak_rand_bit(woz_weak_next_rand_q);" in source,
            "WOZ weak random bit must be computed after the next random word "
            "has registered")
    require(source.count("woz_weak_refill_pending_q <= 1'b1;") >= 2,
            "WOZ stream paths must request weak random refill instead of "
            "directly computing it")


def test_pl_woz_tick_uses_registered_cache_only_for_timing() -> None:
    """The bit-cell tick must not pull the prefetch hit/data mux into the latch
    update path. A missed pre-cache is invalid for the current tick; write mode
    may only stage the current line into registered retry state.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    tick_index = source.find("if (woz_bit_cell_tick) begin")
    require(tick_index >= 0, "WOZ bit-cell tick block not found")
    byte_index = source.find("byte_v = cached_v[7:0];", tick_index)
    require(byte_index >= 0, "WOZ cached byte assignment not found")

    cache_select = source[tick_index:byte_index]
    else_index = cache_select.find("end else begin")
    require(else_index >= 0, "WOZ tick cache-miss path not found")
    fallback = cache_select[else_index:]

    require("cached_v = {1'b0, 8'hFF};" in fallback,
            "WOZ tick cache miss must produce an invalid byte")
    require("current_line_data" not in fallback and
            "line_byte(" not in fallback,
            "WOZ tick cache miss must not use the combinational prefetch data "
            "mux as the current bit-cell byte")


def test_pl_woz_write_cache_miss_retries_without_consuming_bit() -> None:
    """A WOZ write tick with no registered cached byte must not consume the
    write shift bit or advance the raw bit offset. The timing-safe behavior is
    to register the current cache line for retry, then write it on a later
    cycle.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")
    require("automatic logic write_stall_v;" in source and
            "automatic logic cache_retry_v;" in source,
            "WOZ bit-cell path must have explicit write-stall and cache-retry "
            "state")
    require("woz_write_mode &&\n"
            "                            active_drive_loaded &&\n"
            "                            !drive_read_only &&\n"
            "                            stream_line_hit_q" in source,
            "WOZ write cache miss must stage the current line only for a "
            "loaded writable track")
    require("woz_cached_ready_q <= 1'b1;" in source and
            "woz_cached_byte_q <= disk_next_byte;" in source,
            "WOZ write cache miss must register the retry byte instead of "
            "using it in the same tick")
    require("if (!write_stall_v && !woz_data_load_access)\n"
            "                            woz_shift_q <= {woz_shift_q[6:0], 1'b0};" in source,
            "WOZ write stall must not consume the current shift bit")
    require("skip_bit_advance_v =\n"
            "                        write_stall_v ||" in source,
            "WOZ write stall must not advance the raw bit offset")
    require("if (!cache_retry_v) begin\n"
            "                        woz_cached_ready_q <= 1'b0;" in source,
            "WOZ retry cache must survive the stalled tick")


TESTS = [
    test_c_constants_still_match,
    test_woz1_read_existing_and_empty_track,
    test_woz_read_stages_raw_bits_and_lss_recovers_unaligned_stream,
    test_woz_protected_sync_stream_survives_rotational_offset,
    test_applewin_circular_lss_catches_prologues_across_track_seam,
    test_applewin_lss_state_survives_same_track_retag,
    test_applewin_check_mode_advances_bits_without_shifting_lss,
    test_applewin_large_gap_keeps_only_significant_cells,
    test_applewin_track_switch_bit_offset,
    test_applewin_track_seam_detection,
    test_applewin_deferred_stepper_half_phase_vectors,
    test_woz_info_optimal_bit_timing_matches_applewin,
    test_woz1_write_existing_preserves_bit_count_and_crc,
    test_woz1_write_new_track_appends_and_maps_neighbors,
    test_woz2_read_existing_uses_bit_count,
    test_woz2_write_existing_preserves_descriptor_and_padding,
    test_woz2_write_existing_preserves_raw_bit_count,
    test_woz2_write_new_track_appends_zero_padded_blocks,
    test_woz2_write_new_track_after_unaligned_trks,
    test_pl_woz_write_uses_post_access_q_state,
    test_pl_write_info_preserves_ps_register_contract,
    test_ps_retags_same_woz_physical_track_without_restaging,
    test_standard_images_do_not_use_woz_alias_fast_path,
    test_pl_write_fifo_accounts_simultaneous_push_pop_once,
    test_pl_stepper_matches_applewin_deferred_motion,
    test_rejects_bad_chunks_and_write_protect,
    test_applewin_shift_write_round_trips_through_lss,
    test_applewin_shift_write_is_bit_addressed_not_byte_aligned,
    test_load_track_same_tmap_retag_applies_woz_bit_offset_bump,
    test_woz2_writeback_does_not_silently_rewrite_bit_count,
    test_woz1_writeback_does_not_silently_rewrite_bit_count,
    test_load_track_freezes_lss_before_capturing_old_bit_offset,
    test_prepare_woz_track_stream_does_not_read_pl_track_state,
    test_flush_dirty_track_aborts_on_ddr_read_race,
    test_pl_keeps_per_drive_head_positions,
    test_applewin_shift_write_round_trips_a_full_sector_data_field,
    test_applewin_shift_write_wraps_correctly_at_bit_count_boundary,
    test_applewin_shift_write_does_not_modify_write_protected_track,
    test_applewin_session_first_load_advances_over_elapsed_cells,
    test_applewin_session_subsequent_load_does_not_advance_bits,
    test_applewin_session_emits_loaded_byte_msb_first_during_shift_write,
    test_applewin_session_counts_data_load_dwell_as_shift_write_time,
    test_applewin_session_q7_low_resets_write_started,
    test_applewin_session_write_protected_disk_does_not_modify_track,
    test_pl_resets_write_started_on_q7_low,
    test_pl_data_load_invalidates_track_seam_on_each_load,
    test_pl_woz_track_commit_preserves_write_started,
    test_pl_writes_only_when_drive_not_read_only,
    test_applewin_session_check_wp_to_write_mode_advances_pending_cells,
    test_pl_continuous_lss_matches_applewin_burst_for_wp_check_advance,
    test_pl_clears_write_dirty_on_track_info_clear,
    test_load_track_rechecks_write_pending_before_clearing_track_info,
    test_woz_mount_enables_writeback_when_image_and_file_allow,
    test_woz_writeback_runtime_toggle_preserves_media_write_protect,
    test_woz_mount_clears_stale_idle_dirty_latch,
    test_wozwrite_on_clears_disabled_stale_dirty_state,
    test_uart_has_wozwrite_debug_command,
    test_woz_flush_logging_captures_dirty_metadata,
    test_woz_flush_refuses_structural_prologue_regressions,
    test_woz_structure_reject_is_acknowledged_once,
    test_woz_dirty_flush_treats_same_tmap_alias_as_current,
    test_pl_woz_io_access_requires_loaded_woz_track,
    test_pl_pipelines_prefetch_cache_patch_for_timing,
    test_pl_pipelines_woz_weak_random_refill_for_timing,
    test_pl_woz_tick_uses_registered_cache_only_for_timing,
    test_pl_woz_write_cache_miss_retries_without_consuming_bit,
]


def main() -> int:
    failures = []
    for test in TESTS:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} WOZ tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} WOZ tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
