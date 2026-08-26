#!/usr/bin/env python3
"""Source and reference checks for the SSI-263/SC-01 hybrid design.

This test needs no Vivado install.  It checks facts that must stay true while
the native SSI-263 control path continues to use the SC-01 formant engine.

Run from any directory with:

    python scripts/test_ssi263_sc01_hybrid.py
"""

from __future__ import annotations

import hashlib
import re
import zlib
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APPLE_HDL = ROOT / "hdl" / "apple"
ROM = APPLE_HDL / "ssi263_sc02_rom.mem"
CORE = APPLE_HDL / "sc01a_digital_core.sv"
BACKEND = APPLE_HDL / "ssi263_formant_backend.sv"
BUS_WRAPPER = APPLE_HDL / "ssi263_bus_wrapper.sv"
FORMANT_PKG = APPLE_HDL / "ssi263_formant_pkg.sv"
MOCKINGBOARD = APPLE_HDL / "mockingboard.sv"
HDL_SOURCES = ROOT / "hdl" / "hdl_sources.txt"

SOURCE_ROM_BYTES = 2048
ACTIVE_ROM_BYTES = 512
SOURCE_ROM_CRC32 = 0xCC0A72EE
SOURCE_ROM_SHA256 = (
    "9c3bba73319e1ed3652c85dac19874df04cbb72e62fdd63d6cbd7b34ff81f941"
)
ACTIVE_ROM_CRC32 = 0xB60D893F
ACTIVE_ROM_SHA256 = (
    "101d129a5f104e6190f2eca518bbf9ef65bf4ff92684d29eba56d9641aa02b0a"
)

# These valid SSI-263 phones map to SC-01 STOP in the old compatibility map.
SC01_STOP_COLLAPSE = (
    0x04, 0x06, 0x12, 0x15, 0x17, 0x1E, 0x1F, 0x21, 0x22, 0x2A,
    0x2B, 0x2D, 0x2E, 0x31, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
)


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def read(path: Path) -> str:
    require(path.is_file(), f"missing required file: {path.relative_to(ROOT)}")
    return path.read_text(encoding="utf-8")


def without_sv_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", "", source, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", source)


def parse_mem(path: Path) -> tuple[bytes, str]:
    source = read(path)
    values: list[int] = []
    for line_no, raw_line in enumerate(source.splitlines(), 1):
        line = raw_line.split("//", 1)[0].strip()
        if not line:
            continue
        require(
            re.fullmatch(r"[0-9A-Fa-f]{2}", line) is not None,
            f"ROM line {line_no} must contain one byte, got {line!r}",
        )
        values.append(int(line, 16))
    return bytes(values), source


def rom_address(phone: int, selector: int) -> int:
    return ((phone & 0x3F) << 3) | (selector & 0x07)


def test_canonical_rom_and_addressing() -> None:
    active, source = parse_mem(ROM)
    require(
        len(active) == ACTIVE_ROM_BYTES,
        f"SSI ROM must contain {ACTIVE_ROM_BYTES} active bytes, got {len(active)}",
    )
    require(
        hashlib.sha256(active).hexdigest() == ACTIVE_ROM_SHA256,
        "active SSI ROM SHA-256 does not match the supplied ROM",
    )
    require(
        zlib.crc32(active) & 0xFFFFFFFF == ACTIVE_ROM_CRC32,
        "active SSI ROM CRC32 does not match the supplied ROM",
    )

    # The supplied 2 KiB ROM has a 512-byte table followed by zero fill.  This
    # rebuild checks the full source image, not only metadata in the .mem file.
    source_image = active + bytes(SOURCE_ROM_BYTES - len(active))
    require(
        hashlib.sha256(source_image).hexdigest() == SOURCE_ROM_SHA256,
        "rebuilt 2 KiB SSI ROM SHA-256 does not match ssi263a.bin",
    )
    require(
        zlib.crc32(source_image) & 0xFFFFFFFF == SOURCE_ROM_CRC32,
        "rebuilt 2 KiB SSI ROM CRC32 does not match ssi263a.bin",
    )

    rows = [active[offset:offset + 8] for offset in range(0, len(active), 8)]
    require(len(rows) == 64, "SSI ROM must contain 64 phone rows")
    require(len(set(rows)) == 64, "each SSI phone row must remain distinct")
    for phone in range(64):
        for selector in range(8):
            require(
                active[rom_address(phone, selector)] == rows[phone][selector],
                f"ROM address mismatch for phone {phone:02X}, selector {selector}",
            )

    # Keep the source identity visible to reviewers as well as checked above.
    require(SOURCE_ROM_SHA256.upper() in source.upper(),
            "ROM source SHA-256 must remain in the .mem header")
    require(f"{SOURCE_ROM_CRC32:08X}" in source.upper(),
            "ROM source CRC32 must remain in the .mem header")

    # Synthesis uses a package function so the ROM remains constant-folded
    # beside the existing SC-01 tables. Prove that copy is byte-for-byte the
    # canonical .mem artifact; this prevents two ROM truths from drifting.
    package = read(FORMANT_PKG)
    packed_rows = {
        int(phone, 16): int(row, 16)
        for phone, row in re.findall(
            r"6'h([0-9A-Fa-f]{2}):\s*ssi263_sc02_rom_row\s*=\s*"
            r"64'h([0-9A-Fa-f]{16})",
            package,
        )
    }
    default_match = re.search(
        r"default:\s*ssi263_sc02_rom_row\s*=\s*64'h([0-9A-Fa-f]{16})",
        package,
    )
    require(len(packed_rows) == 63 and default_match is not None,
            "embedded SSI ROM must define rows 00-3E and the 3F default")
    packed_rows[0x3F] = int(default_match.group(1), 16)
    embedded = b"".join(
        packed_rows[phone].to_bytes(8, byteorder="little")
        for phone in range(64)
    )
    require(embedded == active,
            "embedded SSI ROM rows do not match ssi263_sc02_rom.mem")


def test_native_rows_do_not_fall_back_to_sc01_stop() -> None:
    active, _ = parse_mem(ROM)

    # Selectors 5 and 6 are the native voice and fricative targets.  Every
    # phone lost to STOP by the old map carries real source energy in this ROM.
    for phone in SC01_STOP_COLLAPSE:
        voice = active[rom_address(phone, 5)] >> 4
        fricative = active[rom_address(phone, 6)] >> 4
        require(
            voice != 0 or fricative != 0,
            f"collapsed phone {phone:02X} unexpectedly has no native energy",
        )

    backend = without_sv_comments(read(BACKEND))
    core = without_sv_comments(read(CORE))
    package = without_sv_comments(read(FORMANT_PKG))
    require(
        "ssi263_to_sc01_audio_phone(" not in backend,
        "hybrid SSI audio must not select targets through the lossy SC-01 phone map",
    )
    require(
        "ssi263_sc02_target(phone" in core and
        "function automatic logic [7:0] ssi263_sc02_rom_byte" in package and
        "input logic [5:0] phone" in package and
        "input logic [2:0] selector" in package,
        "hybrid target logic must read the native phone/selector ROM function",
    )


def _balanced_end(source: str, opening: int) -> int:
    require(source[opening] == "(", "internal parser did not start on '('")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "(":
            depth += 1
        elif source[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    raise TestFailure("unterminated SystemVerilog instance")


def module_instances(source: str, module: str) -> list[str]:
    instances: list[str] = []
    cursor = 0
    header = re.compile(rf"\b{re.escape(module)}\s*#\s*\(")
    while match := header.search(source, cursor):
        parameter_open = source.find("(", match.start())
        parameter_end = _balanced_end(source, parameter_open)
        name_match = re.match(r"\s*([A-Za-z_]\w*)\s*\(",
                              source[parameter_end + 1:])
        require(name_match is not None,
                f"could not parse {module} instance after parameter list")
        port_open = parameter_end + 1 + name_match.end() - 1
        port_end = _balanced_end(source, port_open)
        instances.append(source[match.start():port_end + 1])
        cursor = port_end + 1
    return instances


def test_two_native_ssi_sockets() -> None:
    source = without_sv_comments(read(MOCKINGBOARD))
    instances = module_instances(source, "ssi263_voice")
    require(len(instances) == 2,
            f"Phasor must instantiate two speech sockets, got {len(instances)}")
    for index, block in enumerate(instances):
        require(
            re.search(r"\.SSI263_TYPE\s*\(\s*2\s*\)", block) is not None,
            f"speech socket {index} must model an SSI-263AP",
        )
        require(
            re.search(r"\.HAS_SC01\s*\(\s*1'b0\s*\)", block) is not None,
            f"speech socket {index} must expose SSI, not a Votrax bus socket",
        )
        require(
            re.search(r"\.xck_ce\s*\(", block) is not None,
            f"speech socket {index} must receive the SSI XCK clock enable",
        )


def _task_body(source: str, name: str) -> str:
    match = re.search(
        rf"\btask\s+automatic\s+{re.escape(name)}\b(?P<body>.*?)\bendtask\b",
        source,
        flags=re.DOTALL,
    )
    require(match is not None, f"missing task {name}")
    return match.group("body")


def test_ssi_phone_start_masks_switched_filter_history() -> None:
    source = without_sv_comments(read(BACKEND))
    play_body = _task_body(source, "play_phoneme")
    require(
        re.search(
            r"if\s*\(\s*!votrax\s*\)\s*begin\s*"
            r"invalidate_filter_history\s*\(\s*\)\s*;\s*end",
            play_body,
            flags=re.DOTALL,
        ) is not None,
        "each SSI phone must invalidate coefficient-dependent SC-01 IIR history",
    )
    require(
        "clear_filter_history();" not in play_body,
        "SSI phone start must not add a bulk clear to every history register",
    )

    invalidate_body = _task_body(source, "invalidate_filter_history")
    for name in (
        "f1_history_valid_q", "f2_history_valid_q", "f2n_history_valid_q",
        "f3_history_valid_q", "f4_history_valid_q", "fn_history_valid_q",
        "fx_history_valid_q", "presence_history_valid_q",
    ):
        require(
            re.search(rf"\b{re.escape(name)}\s*<=\s*[^;]*'b0+\s*;",
                      invalidate_body) is not None,
            f"phone-start invalidation must clear {name}",
        )

    require(
        "history_valid[0] ? x1 : 24'sd0" in source and
        "history_valid[1] ? x2 : 24'sd0" in source and
        "history_valid[2] ? x3 : 24'sd0" in source and
        "history_valid[0] ? y1 : 24'sd0" in source and
        "history_valid[1] ? y2 : 24'sd0" in source and
        "history_valid[2] ? y3 : 24'sd0" in source,
        "invalid stored taps must read as zero until overwritten",
    )
    for name, shift in (
        ("f1_history_valid_q", "{f1_history_valid_q[1:0], 1'b1}"),
        ("f2_history_valid_q", "{f2_history_valid_q[1:0], 1'b1}"),
        ("f2n_history_valid_q", "{f2n_history_valid_q[1:0], 1'b1}"),
        ("f3_history_valid_q", "{f3_history_valid_q[1:0], 1'b1}"),
        ("f4_history_valid_q", "{f4_history_valid_q[1:0], 1'b1}"),
        ("fn_history_valid_q", "{fn_history_valid_q[0], 1'b1}"),
    ):
        require(
            f"{name} <= {shift};" in source,
            f"normal filter shifts must validate newly written taps for {name}",
        )
    require(
        "fx_history_valid_q <= 1'b1;" in source and
        "presence_history_valid_q <= 1'b1;" in source,
        "one-sample histories must become valid after their first update",
    )


def test_timing_refactor_is_exhaustively_equivalent() -> None:
    source = without_sv_comments(read(CORE))
    load_body = _task_body(source, "load_phone")
    require(
        load_body.count("pitch_limit_q <=") == 1,
        "load_phone must select inflection before its sole pitch-limit write",
    )
    require(
        re.search(
            r"ssi_response_subticks_minus_one\s*=\s*"
            r"\{\s*~rate_inflection\[7:4\]\s*,\s*8'hFF\s*\}\s*;",
            source,
        ) is not None,
        "response reload must use the exact shallow RATE identity",
    )

    for rate in range(16):
        arithmetic = (16 - rate) * 256 - 1
        concatenated = (((~rate) & 0x0F) << 8) | 0xFF
        require(
            concatenated == arithmetic,
            f"response concat differs from arithmetic at RATE {rate}",
        )

    # Prove the consolidated load_phone selection matches every old branch
    # for all SSI words, modes, seed states, and retained transition targets.
    for function in range(4):
        for votrax in (False, True):
            transitioned = not votrax and function == 3
            for seeded in (False, True):
                for active_target in range(32):
                    active_bits = active_target << 6
                    for live in range(4096):
                        if transitioned and seeded:
                            old_selected = (live & ~0x07C0) | active_bits
                        else:
                            old_selected = live
                        old_seeded = seeded or (transitioned and not seeded)

                        new_selected = live
                        new_seeded = seeded
                        if transitioned:
                            if not seeded:
                                new_seeded = True
                            else:
                                new_selected = (live & ~0x07C0) | active_bits

                        require(
                            new_selected == old_selected and
                            new_seeded == old_seeded,
                            "consolidated pitch selection changed a load case",
                        )


def test_ff_is_not_a_hard_mute_code() -> None:
    source = without_sv_comments(read(BACKEND))
    require(
        re.search(r"FILTER_FREQ_SILENCE\s*=\s*8'hFF", source,
                  flags=re.IGNORECASE) is None,
        "FF is the fastest valid filter clock, not a silence sentinel",
    )
    comparisons = (
        r"filter_(?:freq|frequency)\s*==\s*(?:8'hFF|FILTER_FREQ_SILENCE)",
        r"(?:8'hFF|FILTER_FREQ_SILENCE)\s*==\s*filter_(?:freq|frequency)",
    )
    for pattern in comparisons:
        require(
            re.search(pattern, source, flags=re.IGNORECASE) is None,
            "audio mute logic must not treat filter frequency FF as silence",
        )


def test_hybrid_sources_are_in_the_build() -> None:
    sources = read(HDL_SOURCES).replace("\\", "/")
    require("apple/ssi263_sc02_rom.mem" in sources,
            "Vivado source list must include the native SSI ROM")
    require("apple/ssi263_xck_ce.sv" in sources,
            "Vivado source list must include the SSI XCK clock enable")
    require("apple/sc01a_digital_core.sv" in sources and
            "apple/ssi263_formant_backend.sv" in sources,
            "hybrid build must retain the SC-01 core and formant backend")


def test_response_and_phone_repeat_contract() -> None:
    core = without_sv_comments(read(CORE))
    backend = without_sv_comments(read(BACKEND))
    wrapper = without_sv_comments(read(BUS_WRAPPER))
    require(
        "ssi_response_subticks_left_q" in core and
        "ssi_response_slot_q" in core and
        "ssi_response_subticks_minus_one()" in core and
        "control_speed_step_q <= 6'd1;" in core and
        "rate_scaled_speed_step" not in core,
        "frame response must use the native sixteen-slot RATE counter",
    )
    require(
        "ssi_duration_frame_q <= 4'd0;\n"
        "                            ticks <= 5'd0;" in core,
        "the current SSI phone must repeat until the host replaces it",
    )
    require(
        "if (core_response_done) begin" in backend and
        "active_valid_q && core_response_done" not in backend,
        "frame response must keep running past an internal phone boundary",
    )
    require(
        "repeat_completed_ssi263" not in wrapper and
        "assign formant_rate_inflection" in wrapper and
        "ssi_reg == SSI_RATEINF" in wrapper and
        "!backend_start_q" in wrapper,
        "reg1/reg2 ACK must not restart audio and RATE must reach the live counter",
    )


@dataclass
class Ssi263ResponseReference:
    """Small public-timing model for CTL stop/restart tests.

    Counts use the effective clock after DIV2.  CTL stop clears the current
    response phase.  The next CTL falling edge starts a complete interval,
    independent of how long the device stayed stopped.
    """

    duration_phoneme: int = 0xC0
    rate_inflection: int = 0
    control: int = 0x80
    response_mode: int = 3
    active: bool = False
    request: bool = False
    elapsed: int = 0

    @property
    def rate(self) -> int:
        return (self.rate_inflection >> 4) & 0x0F

    @property
    def interval(self) -> int:
        frame_ticks = 4096 * (16 - self.rate)
        if self.response_mode == 1:
            return frame_ticks
        return (4 - self.response_mode) * frame_ticks

    def write_duration(self, value: int) -> None:
        self.duration_phoneme = value & 0xFF
        self.request = False
        self.elapsed = 0
        if not (self.control & 0x80):
            self.active = True

    def write_rate_inflection(self, value: int) -> None:
        self.rate_inflection = value & 0xFF

    def write_control(self, value: int) -> None:
        old_stopped = bool(self.control & 0x80)
        new_stopped = bool(value & 0x80)
        self.control = value & 0xFF
        if new_stopped:
            self.active = False
            self.request = False
            self.elapsed = 0
            return
        if old_stopped:
            requested_mode = (self.duration_phoneme >> 6) & 0x03
            if requested_mode != 0:
                self.response_mode = requested_mode
            self.active = True
            self.request = False
            self.elapsed = 0

    def advance(self, effective_xck_ticks: int) -> None:
        require(effective_xck_ticks >= 0, "tick count must not be negative")
        if not self.active:
            return
        total = self.elapsed + effective_xck_ticks
        if total >= self.interval:
            self.elapsed = total % self.interval
            self.request = True
        else:
            self.elapsed = total

    def acknowledge(self) -> None:
        self.request = False


def test_ctl_wait_restart_gets_a_full_response_interval() -> None:
    waits = (0, 1, 17, 4095, 65537, 400000)
    for mode in (1, 2, 3):
        for rate in (0, 5, 15):
            for wait in waits:
                model = Ssi263ResponseReference()
                model.write_rate_inflection(rate << 4)
                model.write_duration((mode << 6) | 0x0B)

                # Begin a phone, stop it part way through, then wait while CTL
                # holds the chip stopped.  The wait must not advance a hidden
                # duration phase used by the next phone.
                model.write_control(0x00)
                model.advance(max(1, model.interval // 3))
                model.write_control(0x80)
                model.advance(wait)
                require(model.elapsed == 0 and not model.request,
                        "CTL stop must hold the response phase reset")

                model.write_duration((mode << 6) | 0x2C)
                model.write_control(0x00)
                frame_ticks = 4096 * (16 - rate)
                expected = (frame_ticks if mode == 1 else
                            (4 - mode) * frame_ticks)
                require(model.interval == expected,
                        "reference interval does not match the SSI formula")
                model.advance(expected - 1)
                require(not model.request,
                        "request arrived before a full post-CTL interval")
                model.advance(1)
                require(model.request,
                        "request did not arrive at the full post-CTL interval")
                model.acknowledge()
                model.advance(expected - 1)
                require(not model.request,
                        "repeating response arrived before its next interval")
                model.advance(1)
                require(model.request,
                        "response timer stopped after the first interval")


TESTS = (
    test_canonical_rom_and_addressing,
    test_native_rows_do_not_fall_back_to_sc01_stop,
    test_two_native_ssi_sockets,
    test_ssi_phone_start_masks_switched_filter_history,
    test_timing_refactor_is_exhaustively_equivalent,
    test_ff_is_not_a_hard_mute_code,
    test_hybrid_sources_are_in_the_build,
    test_response_and_phone_repeat_contract,
    test_ctl_wait_restart_gets_a_full_response_interval,
)


def main() -> int:
    failures: list[str] = []
    for test in TESTS:
        try:
            test()
            print(f"PASS {test.__name__}")
        except (OSError, TestFailure) as exc:
            failures.append(f"{test.__name__}: {exc}")
            print(f"FAIL {test.__name__}: {exc}")
    if failures:
        print(f"SSI263 SC01 HYBRID FAIL ({len(failures)} checks)")
        return 1
    print("SSI263 SC01 HYBRID PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
