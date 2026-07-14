#!/usr/bin/env python3
"""Regression tests for Appletini Disk II DSK/PO/NIB handling.

These are host-side tests for the standard Disk II image contract implemented
by ps_sources/frontend/disk2_service.c. AppleWin's DiskImageHelper is the
behavioral reference for the DOS/ProDOS sector orders, 6-and-2 encoding,
generated track layout, and raw .nib passthrough sizing.

Run with:

    python scripts/test_disk2_standard.py
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
DISK2_SERVICE_C = REPO_ROOT / "ps_sources" / "frontend" / "disk2_service.c"
DISK2_CARD_SV = REPO_ROOT / "hdl" / "apple" / "disk2_card.sv"
DISK2_SOUND_PLAYER_SV = REPO_ROOT / "hdl" / "apple" / "disk2_sound_player.sv"
BUILD_DISK2_SOUND_ASSETS_PY = REPO_ROOT / "scripts" / "build_disk2_sound_assets.py"
DISK2_SOUND_PKG_SV = REPO_ROOT / "hdl" / "apple" / "disk2_sound_pkg.sv"
APPLE_TOP_SV = REPO_ROOT / "hdl" / "apple" / "apple_top.sv"
CARD_CONTROL_REGS_H = REPO_ROOT / "ps_sources" / "frontend" / "card_control_regs.h"
CONFIG_MENU_H = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.h"
CONFIG_MENU_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu.c"
CONFIG_MENU_HELP_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_help.c"
CONFIG_MENU_DEVICE_TABS_C = REPO_ROOT / "ps_sources" / "frontend" / "config_menu_device_tabs.c"
FRONTEND_MAIN_C = REPO_ROOT / "ps_sources" / "frontend" / "main.c"

SECTOR_BYTES = 256
SECTORS_PER_TRACK = 16
TRACK_DENIBBLIZED_SIZE = SECTOR_BYTES * SECTORS_PER_TRACK
NIB_TRACK_BYTES = 0x1A00
GENERATED_TRACK_BYTES = 6384
TRACK_STREAM_BYTES = 8192
TRACK_COUNT = 35
DEFAULT_VOLUME = 0xFE

DOS_ORDER = [0x00, 0x07, 0x0E, 0x06, 0x0D, 0x05, 0x0C, 0x04,
             0x0B, 0x03, 0x0A, 0x02, 0x09, 0x01, 0x08, 0x0F]
PRODOS_ORDER = [0x00, 0x08, 0x01, 0x09, 0x02, 0x0A, 0x03, 0x0B,
                0x04, 0x0C, 0x05, 0x0D, 0x06, 0x0E, 0x07, 0x0F]

GCR_6AND2 = [
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
]
GCR_DECODE = {value: index << 2 for index, value in enumerate(GCR_6AND2)}


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def u8(value: int) -> int:
    return value & 0xFF


def disk_order(is_prodos: bool) -> list[int]:
    return PRODOS_ORDER if is_prodos else DOS_ORDER


def physical_to_file_sector(physical_sector: int, is_prodos: bool) -> int:
    if physical_sector == 15:
        return 15
    return (physical_sector * (8 if is_prodos else 7)) % 15


def code44(value: int) -> bytes:
    return bytes((((value >> 1) & 0x55) | 0xAA, (value & 0x55) | 0xAA))


def decode44_pair(data: bytes | bytearray, pos: int) -> int:
    return u8(((data[pos] & 0x55) << 1) | (data[pos + 1] & 0x55))


def encode_6and2(sector: bytes | bytearray) -> bytes:
    require(len(sector) == SECTOR_BYTES, "sector must be 256 bytes")
    raw: list[int] = []
    offset = 0xAC
    while offset != 0x02:
        value = 0
        value = (value << 2) | ((sector[offset] & 0x01) << 1) | ((sector[offset] & 0x02) >> 1)
        offset = u8(offset - 0x56)
        value = (value << 2) | ((sector[offset] & 0x01) << 1) | ((sector[offset] & 0x02) >> 1)
        offset = u8(offset - 0x56)
        value = (value << 2) | ((sector[offset] & 0x01) << 1) | ((sector[offset] & 0x02) >> 1)
        offset = u8(offset - 0x53)
        raw.append(u8(value << 2))

    raw[-2] &= 0x3F
    raw[-1] &= 0x3F
    raw.extend(sector)
    require(len(raw) == 342, "6-and-2 raw block length changed")

    saved = 0
    encoded = bytearray()
    for value in raw:
        encoded.append(GCR_6AND2[(saved ^ value) >> 2])
        saved = value
    encoded.append(GCR_6AND2[saved >> 2])
    return bytes(encoded)


def decode_6and2(encoded: bytes | bytearray) -> bytes:
    require(len(encoded) >= 343, "encoded sector is too short")
    raw = [GCR_DECODE[encoded[i]] for i in range(343)]
    saved = 0
    for i in range(342):
        value = saved ^ raw[i]
        raw[i] = value
        saved = value

    sector = bytearray(SECTOR_BYTES)
    offset = 0xAC
    low_index = 0
    while offset != 0x02:
        low_bits = raw[low_index]
        low_index += 1
        if offset >= 0xAC:
            sector[offset] = (raw[0x56 + offset] & 0xFC) | ((low_bits & 0x80) >> 7) | ((low_bits & 0x40) >> 5)
        offset = u8(offset - 0x56)
        sector[offset] = (raw[0x56 + offset] & 0xFC) | ((low_bits & 0x20) >> 5) | ((low_bits & 0x10) >> 3)
        offset = u8(offset - 0x56)
        sector[offset] = (raw[0x56 + offset] & 0xFC) | ((low_bits & 0x08) >> 3) | ((low_bits & 0x04) >> 1)
        offset = u8(offset - 0x53)
    return bytes(sector)


def make_sector_track(track: int) -> bytes:
    out = bytearray(TRACK_DENIBBLIZED_SIZE)
    for sector in range(SECTORS_PER_TRACK):
        base = sector * SECTOR_BYTES
        for i in range(SECTOR_BYTES):
            out[base + i] = u8((track * 29) ^ (sector * 17) ^ (i * 7) ^ (i >> 3))
    return bytes(out)


def nibblize_track(sector_track: bytes | bytearray, track: int, is_prodos: bool) -> bytes:
    require(len(sector_track) == TRACK_DENIBBLIZED_SIZE, "sector track must be 4096 bytes")
    out = bytearray([0xFF] * NIB_TRACK_BYTES)
    pos = 0

    def put(value: int) -> None:
        nonlocal pos
        require(pos < NIB_TRACK_BYTES, "generated track overflowed NIB buffer")
        out[pos] = value
        pos += 1

    for _ in range(48):
        put(0xFF)
    for physical_sector in range(SECTORS_PER_TRACK):
        file_sector = physical_to_file_sector(physical_sector, is_prodos)
        checksum = DEFAULT_VOLUME ^ track ^ physical_sector

        out[pos:pos + 3] = b"\xD5\xAA\x96"
        pos += 3
        out[pos:pos + 2] = code44(DEFAULT_VOLUME)
        pos += 2
        out[pos:pos + 2] = code44(track)
        pos += 2
        out[pos:pos + 2] = code44(physical_sector)
        pos += 2
        out[pos:pos + 2] = code44(checksum)
        pos += 2
        out[pos:pos + 3] = b"\xDE\xAA\xEB"
        pos += 3

        for _ in range(6):
            put(0xFF)
        out[pos:pos + 3] = b"\xD5\xAA\xAD"
        pos += 3
        encoded = encode_6and2(sector_track[file_sector * SECTOR_BYTES:(file_sector + 1) * SECTOR_BYTES])
        out[pos:pos + len(encoded)] = encoded
        pos += len(encoded)
        out[pos:pos + 3] = b"\xDE\xAA\xEB"
        pos += 3
        for _ in range(27):
            put(0xFF)

    require(pos == GENERATED_TRACK_BYTES, f"generated track length changed: {pos}")
    return bytes(out[:pos])


def denibblize_track(nibbles: bytes | bytearray, is_prodos: bool, base_track: bytes | bytearray | None = None) -> bytes:
    require(len(nibbles) >= 512, "nibble stream is too short")
    sector_track = bytearray(base_track if base_track is not None else bytes(TRACK_DENIBBLIZED_SIZE))
    offset = 0
    sector = -1
    sectors_seen = 0
    length = len(nibbles)

    for _ in range((SECTORS_PER_TRACK * 2) + 1):
        byteval = [0, 0, 0]
        bytenum = 0
        loop = length
        while loop and bytenum < 3:
            value = nibbles[offset]
            offset = 0 if offset + 1 >= length else offset + 1
            loop -= 1
            if bytenum:
                byteval[bytenum] = value
                bytenum += 1
            elif value == 0xD5:
                byteval[bytenum] = value
                bytenum += 1

        if bytenum == 3 and byteval[1] == 0xAA:
            encoded = bytearray()
            temp_offset = offset
            for _ in range(384):
                encoded.append(nibbles[temp_offset])
                temp_offset = 0 if temp_offset + 1 >= length else temp_offset + 1

            if byteval[2] == 0x96:
                sector = decode44_pair(encoded, 4)
            elif byteval[2] == 0xAD:
                if 0 <= sector < SECTORS_PER_TRACK:
                    file_sector = physical_to_file_sector(sector, is_prodos)
                    decoded = decode_6and2(encoded[:343])
                    sector_track[file_sector * SECTOR_BYTES:(file_sector + 1) * SECTOR_BYTES] = decoded
                    sectors_seen |= 1 << sector
                sector = 0

    require(sectors_seen == 0xFFFF, f"expected all sectors, saw bitmap {sectors_seen:04X}")
    return bytes(sector_track)


def find_prologues(track: bytes | bytearray, prologue: bytes) -> list[int]:
    return [i for i in range(len(track) - 2) if bytes(track[i:i + 3]) == prologue]


@dataclass
class StandardImage:
    format_name: str
    data: bytearray

    @classmethod
    def with_tracks(cls, format_name: str, tracks: list[bytes]) -> "StandardImage":
        if format_name == "nib":
            return cls(format_name, bytearray().join(bytearray(t) for t in tracks))
        return cls(format_name, bytearray().join(bytearray(t) for t in tracks))

    @property
    def is_prodos(self) -> bool:
        return self.format_name == "po"

    @property
    def track_count(self) -> int:
        divisor = NIB_TRACK_BYTES if self.format_name == "nib" else TRACK_DENIBBLIZED_SIZE
        return len(self.data) // divisor

    def qtrack_to_track(self, qtrack: int) -> int:
        track = qtrack // 4
        return min(track, self.track_count - 1)

    def read_qtrack(self, qtrack: int) -> bytes:
        track = self.qtrack_to_track(qtrack)
        if self.format_name == "nib":
            start = track * NIB_TRACK_BYTES
            return bytes(self.data[start:start + NIB_TRACK_BYTES])
        start = track * TRACK_DENIBBLIZED_SIZE
        return nibblize_track(self.data[start:start + TRACK_DENIBBLIZED_SIZE], track, self.is_prodos)

    def write_qtrack(self, qtrack: int, stream: bytes) -> None:
        track = self.qtrack_to_track(qtrack)
        if self.format_name == "nib":
            require(len(stream) >= NIB_TRACK_BYTES, "NIB write must contain a full raw track")
            start = track * NIB_TRACK_BYTES
            self.data[start:start + NIB_TRACK_BYTES] = stream[:NIB_TRACK_BYTES]
            return
        start = track * TRACK_DENIBBLIZED_SIZE
        old_track = bytes(self.data[start:start + TRACK_DENIBBLIZED_SIZE])
        new_track = denibblize_track(stream, self.is_prodos, old_track)
        self.data[start:start + TRACK_DENIBBLIZED_SIZE] = new_track


def test_c_constants_and_source_contract() -> None:
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")
    expected = {
        "DISK2_NIB_TRACK_BYTES": NIB_TRACK_BYTES,
        "DISK2_TRACK_STREAM_BYTES": TRACK_STREAM_BYTES,
        "DISK2_SECTOR_BYTES": SECTOR_BYTES,
        "DISK2_DSK_SECTORS_PER_TRACK": SECTORS_PER_TRACK,
    }
    for name, value in expected.items():
        match = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+|\d+)U", source)
        require(match is not None, f"{name} not found in disk2_service.c")
        require(int(match.group(1), 0) == value, f"{name} changed without updating tests")

    gcr_match = re.search(r"static const uint8_t gcr_6and2\[64\]\s*=\s*\{(?P<body>.*?)\};", source, re.S)
    require(gcr_match is not None, "gcr_6and2 table not found")
    c_gcr = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", gcr_match.group("body"))]
    require(c_gcr == GCR_6AND2, "gcr_6and2 must match the AppleWin table")

    require('str_ieq(ext, ".nib")' in source, "NIB image detection is missing")
    require('str_ieq(ext, ".dsk")' in source, "DSK image detection is missing")
    require('str_ieq(ext, ".do")' in source, "DO image detection is missing")
    require('str_ieq(ext, ".po")' in source, "PO image detection is missing")
    require("(format == DISK2_IMAGE_PO) ? 1U : 0U" in source, "only PO should use ProDOS order")
    require("physical_sector * (is_prodos ? 8U : 7U)" in source, "sector skew formula changed")
    require(re.search(r"qtrack\)\s*/\s*4U", source) is not None,
            "standard-image qtrack mapping must remain quarter-track based")
    require("offset = (uint32_t)track * DISK2_NIB_TRACK_BYTES" in source, "NIB read/write must be raw track offset")
    require("offset = (uint32_t)track * DISK2_DSK_SECTORS_PER_TRACK * DISK2_SECTOR_BYTES" in source,
            "DSK/PO read/write must use denibblized 4096-byte track offset")


def test_standard_images_are_memory_cached_like_applewin() -> None:
    """AppleWin reads standard disk image files into ImageInfo::pImageBuffer
    and later services DSK/DO/PO/NIB track reads from that memory buffer.
    The firmware must do the same so standard-image stepping is not gated by
    SD/FatFs latency on every track load.
    """
    source = DISK2_SERVICE_C.read_text(encoding="utf-8")

    require("DISK2_STANDARD_MAX_IMAGE_BYTES" in source,
            "standard-image cache must have an explicit bounded image size")
    require("g_standard_image_buf[DISK2_DRIVE_COUNT]" in source,
            "standard images must have a per-drive memory image buffer")
    require("static int load_standard_image_cache" in source,
            "standard images must be read into memory at mount time")
    require("load_standard_image_cache(drive, &file, info.file_size)" in source,
            "probe_file must populate the standard-image cache before publishing media")
    require("static int read_standard_image_bytes" in source and
            "memcpy(buf, &g_standard_image_buf[drive][offset], len)" in source,
            "standard track reads must memcpy from the mounted image buffer")
    require("static int write_standard_image_bytes" in source and
            "memcpy(&g_standard_image_buf[drive][offset], buf, len)" in source and
            "return write_drive_bytes(drive, offset, buf, len);" in source,
            "standard writes must update the mounted image buffer before file writeback")

    sector_read = re.search(
        r"static int read_sector_physical_track\(.*?\n}\n", source, re.S)
    nib_read = re.search(
        r"static int read_nib_physical_track\(.*?\n}\n", source, re.S)
    require(sector_read is not None and nib_read is not None,
            "standard physical track read helpers not found")
    require("read_drive_bytes" not in sector_read.group(0) and
            "read_drive_bytes" not in nib_read.group(0),
            "standard physical track reads must not reopen/read the image file")


def test_disk2_sound_recal_and_volume_contract() -> None:
    disk2_card = DISK2_CARD_SV.read_text(encoding="utf-8")
    sound_player = DISK2_SOUND_PLAYER_SV.read_text(encoding="utf-8")
    sound_assets = BUILD_DISK2_SOUND_ASSETS_PY.read_text(encoding="utf-8")
    sound_pkg = DISK2_SOUND_PKG_SV.read_text(encoding="utf-8")
    apple_top = APPLE_TOP_SV.read_text(encoding="utf-8")
    regs = CARD_CONTROL_REGS_H.read_text(encoding="utf-8")
    config_h = CONFIG_MENU_H.read_text(encoding="utf-8")
    config_c = CONFIG_MENU_C.read_text(encoding="utf-8")
    help_c = CONFIG_MENU_HELP_C.read_text(encoding="utf-8")
    tabs = CONFIG_MENU_DEVICE_TABS_C.read_text(encoding="utf-8")
    frontend_main = FRONTEND_MAIN_C.read_text(encoding="utf-8")

    require("RECAL_REARM_QTRACK     = 8'd4" in disk2_card,
            "track-zero recal sound must not re-arm on tiny qtrack bounces")
    require(disk2_card.count("step_next[15:8] >= RECAL_REARM_QTRACK") == 2,
            "both immediate and delayed step paths must use the recal re-arm threshold")
    require("stepper_hits_track0_stop" in disk2_card and
            "if (track0_stop_hit && recal_armed)" in disk2_card,
            "track-zero recal sound must play only when the head hits the track-0 stop")
    require("sound_seek_distance_for_step" in disk2_card and
            "sound_step_valid_q" in disk2_card and
            "sound_step_track0_stop_q" in disk2_card and
            "sound_step_recal_armed_q" in disk2_card and
            "sound_event_q <= sound_event_for_step(\n"
            "                    sound_step_start_qtrack_q,\n"
            "                    sound_step_end_qtrack_q,\n"
            "                    sound_step_track0_stop_q,\n"
            "                    sound_step_recal_armed_q);" in disk2_card and
            disk2_card.count("sound_step_start_qtrack_q <= drive_qtrack_q[drive_select_q]") == 2 and
            disk2_card.count("sound_step_end_qtrack_q <= step_next[15:8]") == 2 and
            ".sound_seek_start_qtrack(disk2_sound_seek_start_qtrack)" in apple_top and
            ".sound_seek_distance(disk2_sound_seek_distance)" in apple_top,
            "Disk II seek sound events must stage starting/ending qtracks into the sound player")

    require('"DOOR_OPEN", "01_door_open_plus24dB_cap.wav"' in sound_assets and
            '"DOOR_CLOSE", "02_door_close_plus24dB_cap.wav"' in sound_assets and
            '"SEEK_34_0", "05_seek_34_0_plus24dB_cap.wav"' in sound_assets and
            '"SEEK_0_34", "06_seek_0_34_plus24dB_cap.wav"' in sound_assets,
            "Disk II sound assets must include door sounds and 05/06 seek beds")
    require("DISK2_SOUND_DOOR_OPEN" in sound_pkg and
            "DISK2_SOUND_DOOR_CLOSE" in sound_pkg and
            "DISK2_SOUND_SEEK_34_0" in sound_pkg and
            "DISK2_SOUND_SEEK_0_34" in sound_pkg,
            "generated Disk II sound package must expose only the active sound IDs")
    require("EVENT_SEEK_OUTWARD: select_event_sound = DISK2_SOUND_SEEK_0_34" in sound_player and
            "EVENT_SEEK_INWARD: select_event_sound = DISK2_SOUND_SEEK_34_0" in sound_player and
            "event_setup_valid_q" in sound_player and
            "event_calc_start_offset_q" in sound_player and
            "disk2_sound_seek_position_offset(\n"
            "                            event_pos_sound_id_q,\n"
            "                            event_pos_sample_start_pos_q)" in sound_player and
            "DISK2_SOUND_SEEK_FULL_QTRACK_DISTANCE" in sound_player and
            "seek_distance" in sound_player and
            "scaled_seek_length" not in sound_player and
            "function automatic logic [17:0] disk2_sound_seek_position_offset" in sound_pkg and
            "8'd140" in sound_pkg,
            "Disk II seek sounds must slice position-based 05/06 playback")

    require("#define CARD_CTRL_DISK2_SOUND_VOLUME_SHIFT     8U" in regs and
            "#define CARD_CTRL_DISK2_SOUND_EVENT_SHIFT      16U" in regs and
            "#define CARD_CTRL_DISK2_SOUND_EVENT_DOOR_OPEN  4U" in regs and
            "#define CARD_CTRL_DISK2_SOUND_EVENT_DOOR_CLOSE 5U" in regs and
            "#define CARD_CTRL_DISK2_SOUND_DEFAULT_VOLUME   5U" in regs and
            "#define CARD_CTRL_DISK2_SOUND_MAX_VOLUME       10U" in regs,
            "Disk II sound control register must document volume and menu events")
    require(".volume(disk2_sound_control_q[11:8])" in apple_top,
            "Disk II sound player must receive the software volume field")
    require("input  logic [3:0]         volume" in sound_player and
            "wire active = enable && (volume != 4'd0)" in sound_player and
            "4'd5: disk2_volume_scale_coeff = 10'd256" in sound_player and
            "default: disk2_volume_scale_coeff = 10'd512" in sound_player,
            "Disk II sound player must scale volume with 5=current and 10=double")
    require("logic signed [15:0] mix_q;" in sound_player and
            "logic signed [26:0] volume_product_q;" in sound_player and
            "function automatic logic signed [26:0] volume_product" in sound_player and
            "function automatic logic signed [15:0] sat_volume_product" in sound_player and
            "volume_product_q <= volume_product(mix_q, volume_q);" in sound_player and
            "audio_l <= sat_volume_product(volume_product_q);" in sound_player and
            "mix_q <= sat_add16(idle_mix, event_mix);" in sound_player,
            "Disk II sound volume scaling must be pipelined for timing")
    require("card_control_pack_disk2_sound_control" in frontend_main and
            "CARD_CTRL_DISK2_SOUND_DEFAULT_VOLUME" in frontend_main and
            "card_control_pulse_disk2_sound_event" in frontend_main and
            "set_disk2_sound_volume = control_set_disk2_sound_volume" in frontend_main,
            "PS control path must publish and update Disk II sound volume/events")

    require("void (*set_disk2_sound_volume)(void *ctx, uint8_t volume);" in config_h and
            "void (*play_disk2_sound_event)(void *ctx, uint8_t event);" in config_h and
            "uint8_t disk2_activity_visible;" in config_h and
            "uint8_t disk2_sound_volume;" in config_h,
            "config menu state/platform must carry Disk II visibility, sound volume/events")
    version_match = re.search(r"#define APPLETINI_CFG_VERSION\s+(\d+)U", config_c)
    require(version_match is not None and int(version_match.group(1)) >= 100 and
            "#define CONFIG_DEFAULT_DISK2_SOUND_VOLUME 5U" in config_c and
            "CONFIG_DISK2_SOUND_EVENT_DOOR_OPEN" in config_c and
            "CONFIG_DISK2_SOUND_EVENT_DOOR_CLOSE" in config_c and
            "play_disk2_sound_event" in config_c and
            'strcmp(key, "disk2.activity.visible") == 0' in config_c and
            'strcmp(key, "disk2.sound.volume") == 0' in config_c and
            '"disk2.activity.visible=%s\\n"' in config_c and
            '"disk2.sound.volume=%u\\n"' in config_c,
            "Disk II overlay visibility and sound volume must persist and menu disk changes must play door sounds")
    disk2_count = re.search(r"case CONFIG_TAB_DISK2:.*?case CONFIG_TAB_MOUSE:", config_c, re.S)
    require(disk2_count is not None and
            "return 5U;" in disk2_count.group(0) and
            "config_menu_apply_disk2_sound(menu)" in config_c and
            "menu->tab == CONFIG_TAB_DISK2 && menu->item_focus == 4U" in config_c,
            "Disk II tab must expose activity visibility and immediately apply the volume slider")
    require("hgr_draw_disk2_sound_volume_item" in tabs and
            '"Show drive activity overlay"' in tabs and
            '"Show activity overlay"' not in tabs and
            "menu->disk2_sound_volume" in tabs and
            "Supported: WOZ, NIB, DSK, DO, PO." in help_c and
            "cmui_slider(fb," in tabs,
            "Disk II tab must draw the activity toggle and volume control, with supported format help in the shared help panel")


def test_disk2_write_protect_visual_contract() -> None:
    config_h = CONFIG_MENU_H.read_text(encoding="utf-8")
    config_c = CONFIG_MENU_C.read_text(encoding="utf-8")
    tabs = CONFIG_MENU_DEVICE_TABS_C.read_text(encoding="utf-8")
    frontend_main = FRONTEND_MAIN_C.read_text(encoding="utf-8")

    require("uint8_t (*get_disk2_image_read_only)(void *ctx, uint8_t drive);" in config_h,
            "config menu platform must expose the effective Disk II read-only state")
    require("uint8_t read_only;" in config_c and
            "AM_RDO" in config_c and
            "entry.read_only" in config_c and
            "hgr_draw_item_with_lock_ex" in config_c and
            "config_menu_browser_is_disk2_target(menu->browser_target)" in config_c,
            "Disk II file picker entries must track and draw read-only locks")
    require("void hgr_draw_lock_icon" in config_c and
            "void hgr_draw_item_with_lock" in config_c and
            "if (locked == 0U) {\n        return;\n    }" in config_c,
            "boot-menu lock icon helpers must draw only the locked state")
    require("config_menu_disk2_read_only" in tabs and
            "hgr_draw_disk2_image_item" in tabs and
            "cmui_lock" in tabs and
            "menu->disk2_disk_paths[0][0] != '\\0'" in tabs and
            "menu->disk2_disk_paths[1][0] != '\\0'" in tabs,
            "Disk II selected-image rows must show lock state for non-empty images")
    require("menu_platform_get_disk2_image_read_only" in frontend_main and
            "disk2_service_get_image_info(drive, &info)" in frontend_main and
            "ui_draw_disk_lock_icon" in frontend_main and
            "ui_draw_disk_lock_icon(fb, x + 198, y + 3, present, write_protected)" in frontend_main and
            "if (present == 0U || locked == 0U) {\n        return;\n    }" in frontend_main,
            "bezel Disk II activity must leave lock space empty unless write-protected")


def test_sector_order_tables_match_applewin() -> None:
    require([physical_to_file_sector(i, False) for i in range(16)] == DOS_ORDER,
            "DOS sector order does not match AppleWin")
    require([physical_to_file_sector(i, True) for i in range(16)] == PRODOS_ORDER,
            "ProDOS sector order does not match AppleWin")


def test_6and2_round_trip_for_varied_sector_data() -> None:
    for seed in range(16):
        sector = bytes(u8((seed * 31) ^ (i * 13) ^ (i >> 1)) for i in range(SECTOR_BYTES))
        encoded = encode_6and2(sector)
        require(len(encoded) == 343, "encoded sector length changed")
        require(all(value in GCR_DECODE for value in encoded), "encoded sector contains non-GCR bytes")
        require(decode_6and2(encoded) == sector, f"6-and-2 round trip failed for seed {seed}")


def test_dsk_and_po_tracks_have_applewin_layout_and_decode_correctly() -> None:
    for is_prodos, name in ((False, "DSK"), (True, "PO")):
        sector_track = make_sector_track(track=7)
        stream = nibblize_track(sector_track, track=7, is_prodos=is_prodos)
        require(len(stream) == GENERATED_TRACK_BYTES, f"{name} generated track length changed")
        require(stream[:48] == bytes([0xFF]) * 48, f"{name} gap one changed")

        address_offsets = find_prologues(stream, b"\xD5\xAA\x96")
        data_offsets = find_prologues(stream, b"\xD5\xAA\xAD")
        require(len(address_offsets) == 16, f"{name} should have 16 address fields")
        require(len(data_offsets) == 16, f"{name} should have 16 data fields")

        for physical_sector, address in enumerate(address_offsets):
            fields = stream[address + 3:address + 11]
            volume = decode44_pair(fields, 0)
            track = decode44_pair(fields, 2)
            sector = decode44_pair(fields, 4)
            checksum = decode44_pair(fields, 6)
            require(volume == DEFAULT_VOLUME, f"{name} volume changed")
            require(track == 7, f"{name} address track changed")
            require(sector == physical_sector, f"{name} physical sector order changed")
            require(checksum == (DEFAULT_VOLUME ^ 7 ^ physical_sector), f"{name} address checksum changed")

            data = data_offsets[physical_sector] + 3
            decoded = decode_6and2(stream[data:data + 343])
            file_sector = disk_order(is_prodos)[physical_sector]
            expected = sector_track[file_sector * SECTOR_BYTES:(file_sector + 1) * SECTOR_BYTES]
            require(decoded == expected, f"{name} physical sector {physical_sector} decoded wrong file sector")


def test_dsk_and_po_writeback_round_trip_full_tracks() -> None:
    for format_name in ("dsk", "po"):
        tracks = [make_sector_track(track) for track in range(TRACK_COUNT)]
        image = StandardImage.with_tracks(format_name, tracks)

        qtrack = 10
        track = image.qtrack_to_track(qtrack)
        stream = image.read_qtrack(qtrack)
        require(denibblize_track(stream, image.is_prodos) == tracks[track],
                f"{format_name} read stream does not denibblize to original track")

        replacement = bytearray(make_sector_track(track))
        replacement[3 * SECTOR_BYTES:4 * SECTOR_BYTES] = bytes([0xA5]) * SECTOR_BYTES
        write_stream = nibblize_track(replacement, track, image.is_prodos)
        image.write_qtrack(qtrack, write_stream)

        start = track * TRACK_DENIBBLIZED_SIZE
        require(bytes(image.data[start:start + TRACK_DENIBBLIZED_SIZE]) == bytes(replacement),
                f"{format_name} writeback did not update the expected denibblized track")
        require(bytes(image.data[:start]) == b"".join(tracks[:track]),
                f"{format_name} writeback modified earlier tracks")
        require(bytes(image.data[start + TRACK_DENIBBLIZED_SIZE:]) == b"".join(tracks[track + 1:]),
                f"{format_name} writeback modified later tracks")


def test_nib_passthrough_read_and_write() -> None:
    tracks = []
    for track in range(TRACK_COUNT):
        tracks.append(bytes(u8(0x80 | ((track * 11 + i * 5) & 0x7F)) for i in range(NIB_TRACK_BYTES)))
    image = StandardImage.with_tracks("nib", tracks)

    qtrack = 13
    track = image.qtrack_to_track(qtrack)
    require(image.read_qtrack(qtrack) == tracks[track], "NIB read must be raw passthrough")

    replacement = bytes(u8(0x96 + ((i + track) % 0x40)) for i in range(NIB_TRACK_BYTES))
    image.write_qtrack(qtrack, replacement)
    start = track * NIB_TRACK_BYTES
    require(bytes(image.data[start:start + NIB_TRACK_BYTES]) == replacement, "NIB write must be raw passthrough")
    require(bytes(image.data[:start]) == b"".join(tracks[:track]), "NIB write modified earlier tracks")
    require(bytes(image.data[start + NIB_TRACK_BYTES:]) == b"".join(tracks[track + 1:]),
            "NIB write modified later tracks")


def test_pl_standard_stream_advance_matches_applewin_readwrite() -> None:
    """AppleWin's non-WOZ ReadWrite() advances the track byte by elapsed
    Apple cycles while spinning before the access, then advances once for the
    byte access. If the PL only advances one byte per $C0EC access, DSK/NIB
    media rotate far too slowly.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")

    require("STANDARD_SPIN_FIRST_IDLE = 6'd40" in source and
            "STANDARD_SPIN_SECOND_IDLE = 6'd23" in source and
            "STANDARD_SPIN_REPEAT_IDLE = 6'd32" in source,
            "standard media must encode AppleWin's delta > 40, delta >> 5 "
            "spin thresholds without a wide modulo path")
    require("wire standard_spin_tick =\n"
            "        standard_stream_active &&\n"
            "        ab_read.sss_en &&\n"
            "        !disk_stream_access &&\n"
            "        (standard_spin_countdown_q == 6'd1);" in source,
            "standard media must advance while spinning between $C0EC accesses")
    require("track_stream_pos_q <= stream_pos_next(track_stream_pos_q, track_length_q);" in source,
            "standard media idle spin must advance the staged nibble position")
    require("standard_spin_repeat_q ?\n"
            "                            STANDARD_SPIN_REPEAT_IDLE :\n"
            "                            STANDARD_SPIN_SECOND_IDLE" in source,
            "standard media must use AppleWin's first extra-byte threshold then "
            "the 32-cycle repeat cadence")
    require("stream_pos_next(active_stream_pos, track_length_q)" in source,
            "standard media must advance once after the current read/write byte")
    require("standard_spin_countdown_q <= STANDARD_SPIN_FIRST_IDLE;\n"
            "                standard_spin_repeat_q <= 1'b0;" in source,
            "standard media access must restart the AppleWin spin delta window")


def test_pl_standard_partial_read_matches_applewin_threshold() -> None:
    """AppleWin returns a shifted partial nibble and does not advance the byte
    pointer when consecutive non-WOZ reads are within 6 cycles.
    """
    source = DISK2_CARD_SV.read_text(encoding="utf-8")

    require("logic [2:0]  standard_read_gap_q;" in source,
            "PL must track the elapsed Apple-cycle gap since the last full "
            "standard-media read latch")
    require("standard_read_gap_q <= 3'd6" in source,
            "standard media partial-read threshold must match AppleWin's 6 cycles")
    require("standard_read_byte =\n"
            "        standard_partial_read ? (disk_next_byte >> standard_invalid_bits) : disk_next_byte;" in source,
            "standard media partial reads must return the shifted current nibble")
    require("standard_partial_read ?\n"
            "                    active_stream_pos :\n"
            "                    stream_pos_next(active_stream_pos, track_length_q)" in source,
            "standard media partial reads must not consume the current nibble")
    require("if (!standard_partial_read && active_drive_loaded)\n"
            "                        standard_read_gap_q <= 3'd0;" in source,
            "standard media partial reads must not update the last full-read cycle")


def test_qtrack_mapping_clamps_to_last_track() -> None:
    image = StandardImage.with_tracks("dsk", [make_sector_track(track) for track in range(TRACK_COUNT)])
    expected = {
        0: 0,
        1: 0,
        2: 0,
        3: 0,
        4: 1,
        10: 2,
        139: 34,
        159: 34,
    }
    for qtrack, track in expected.items():
        require(image.qtrack_to_track(qtrack) == track, f"qtrack {qtrack} mapped to wrong track")


def run() -> int:
    tests = [
        test_c_constants_and_source_contract,
        test_standard_images_are_memory_cached_like_applewin,
        test_disk2_sound_recal_and_volume_contract,
        test_disk2_write_protect_visual_contract,
        test_sector_order_tables_match_applewin,
        test_6and2_round_trip_for_varied_sector_data,
        test_dsk_and_po_tracks_have_applewin_layout_and_decode_correctly,
        test_dsk_and_po_writeback_round_trip_full_tracks,
        test_nib_passthrough_read_and_write,
        test_pl_standard_stream_advance_matches_applewin_readwrite,
        test_pl_standard_partial_read_matches_applewin_threshold,
        test_qtrack_mapping_clamps_to_last_track,
    ]
    failures = []
    for test in tests:
        try:
            test()
        except TestFailure as exc:
            failures.append((test.__name__, str(exc)))
            print(f"FAIL {test.__name__}: {exc}")
        else:
            print(f"PASS {test.__name__}")
    if failures:
        print(f"disk2 standard tests: {len(tests) - len(failures)} of "
              f"{len(tests)} passed; {len(failures)} failed")
        return 1
    print(f"disk2 standard tests: {len(tests)} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
