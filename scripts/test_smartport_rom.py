#!/usr/bin/env python3
"""Source-level checks for the generated SmartPort slot ROM ABI."""

from __future__ import annotations

import importlib.util
from pathlib import Path

from py65.devices.mpu6502 import MPU


REPO_ROOT = Path(__file__).resolve().parents[1]
BUILDER_PATH = REPO_ROOT / "scripts" / "build_smartport_rom.py"
SLOT_MEM = REPO_ROOT / "hdl" / "apple" / "smartport_a2retronet_style_c700.mem"
CONT_MEM = REPO_ROOT / "hdl" / "apple" / "smartport_a2retronet_style_c800.mem"
HDL_SOURCES = REPO_ROOT / "hdl" / "hdl_sources.txt"


class TestFailure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def load_builder():
    spec = importlib.util.spec_from_file_location("build_smartport_rom", BUILDER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load build_smartport_rom.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_mem(path: Path) -> bytearray:
    return bytearray(int(item, 16) for item in path.read_text(encoding="ascii").split())


def generated_roms() -> tuple[bytearray, bytearray, dict[str, int]]:
    builder = load_builder()
    source = builder.preprocess_reference_source()
    slot, cont, symbols = builder.assemble_source(source, include_symbols=True)
    slot[7] = 0x00
    builder.apply_appletini_entries(slot, symbols)
    return slot, cont, symbols


def test_generated_mem_files_are_current() -> None:
    slot, cont, _symbols = generated_roms()

    require(read_mem(SLOT_MEM) == slot,
            "SmartPort C700 mem file must match build_smartport_rom.py output")
    require(read_mem(CONT_MEM) == cont,
            "SmartPort C800 mem file must match build_smartport_rom.py output")


def test_prodos_and_smartport_entries() -> None:
    slot = read_mem(SLOT_MEM)

    require(slot[0x00:0x08] == bytearray([0xC9, 0x20, 0xC9, 0x00,
                                          0xC9, 0x03, 0xC9, 0x00]),
            "SmartPort slot ROM must keep the standard signature bytes")
    require(slot[0x09] == 0x50,
            "SmartPort slot ROM must keep the autostart BVC opcode at Cn09")
    require(slot[0x0A:0x0D] == bytearray([0x4C, 0x23, 0xC7]),
            "Cn0A must be an executable ProDOS block-driver trampoline")
    require(slot[0x0D:0x10] == bytearray([0x4C, 0x26, 0xC7]),
            "Cn0D must be an executable SmartPort dispatcher trampoline")
    require(slot[0x57:0x5A] == bytearray([0x4C, 0x2D, 0xC7]),
            "Cn00 autostart BVC must land on a boot-entry trampoline")
    require(slot[0xB0:0xB8] == b"LINTXT\x4c\x10",
            "CnB0 must publish the side-effect-free overlay probe marker")
    require(slot[0xFF] == 0x0A,
            "CnFF must publish the Appletini ProDOS entry offset Cn0A")

    bvc_target = 0x0B + slot[0x0A]
    require(bvc_target == 0x57,
            "the Cn00 BVC displacement must target the boot trampoline")


def assert_block_helper_restores_addrh(cont: bytearray,
                                       symbols: dict[str, int],
                                       helper_label: str,
                                       page_label: str) -> None:
    helper_addr = symbols[helper_label]
    page_addr = symbols[page_label]
    helper_off = helper_addr - 0xC800
    addrh_zp = symbols["ADDRH"] & 0xFF
    expected = bytearray([
        0xA0, 0x00,                         # LDY #$00
        0x20, page_addr & 0xFF, page_addr >> 8,  # JSR page helper
        0xE6, addrh_zp,                     # INC ADDRH
        0x20, page_addr & 0xFF, page_addr >> 8,  # JSR page helper
        0xC6, addrh_zp,                     # DEC ADDRH
        0x60,                               # RTS
    ])
    require(cont[helper_off:helper_off + len(expected)] == expected,
            f"{helper_label} must restore ADDRH after the second page transfer")


def test_block_helpers_preserve_prodos_buffer_page() -> None:
    _slot, cont, symbols = generated_roms()

    assert_block_helper_restores_addrh(cont, symbols, "RDBLK1", "RDPAGE")
    assert_block_helper_restores_addrh(cont, symbols, "WRBLOCK", "WRPAGE")


class SmartportMemory:
    """64K memory with the card's side-effect-free DATA and explicit DPOP."""

    def __init__(self, ctrl: int, fifo: bytes):
        self.data = bytearray(65536)
        self.ctrl = ctrl
        self.fifo = list(fifo)
        self.data_reads = 0
        self.data_writes = 0
        self.ctrl_writes: list[int] = []
        self.pops = 0

    def __getitem__(self, address: int) -> int:
        address &= 0xFFFF
        if address == 0xCFF0:
            self.data_reads += 1
            return self.fifo[0] if self.fifo else 0
        if address == 0xCFF1:
            return self.ctrl
        return self.data[address]

    def __setitem__(self, address: int, value: int) -> None:
        address &= 0xFFFF
        if address == 0xCFF0:
            self.data_writes += 1
            return
        if address == 0xCFF1:
            self.ctrl_writes.append(value & 0xFF)
            return
        if address == 0xCFF2:
            if self.fifo:
                self.fifo.pop(0)
            self.pops += 1
            return
        self.data[address] = value & 0xFF


def run_rdbblk2(direct: bool) -> tuple[SmartportMemory, bytearray, int]:
    _slot, cont, symbols = generated_roms()
    pattern = bytearray(((i * 73) + 19) & 0xFF for i in range(512))
    memory = SmartportMemory(0xC0 if direct else 0x80,
                             b"" if direct else bytes(pattern))
    memory.data[0xC800:0xC800 + len(cont)] = cont

    destination = 0x1200
    if direct:
        memory.data[destination:destination + 512] = pattern
    memory.data[symbols["ADDRL"]] = destination & 0xFF
    memory.data[symbols["ADDRH"]] = destination >> 8

    sentinel = 0x0600
    mpu = MPU(memory=memory, pc=symbols["RDBLK2"])
    mpu.sp = 0xFD
    return_address = sentinel - 1
    memory.data[0x01FE] = return_address & 0xFF
    memory.data[0x01FF] = return_address >> 8

    for steps in range(20000):
        if mpu.pc == sentinel:
            return memory, pattern, steps
        mpu.step()
    raise TestFailure("RDBLK2 did not return within 20000 instructions")


def test_vtw_direct_block_rom_contract() -> None:
    direct_mem, pattern, direct_steps = run_rdbblk2(True)
    require(direct_mem.data[0x1200:0x1400] == pattern,
            "direct response must preserve the block already copied by PS")
    require(direct_mem.data_reads == 0 and direct_mem.pops == 0,
            "direct response must not touch the empty payload FIFO")
    require(direct_steps < 10,
            "direct response must return without entering the byte loop")

    fifo_mem, pattern, _steps = run_rdbblk2(False)
    require(fifo_mem.data[0x1200:0x1400] == pattern,
            "fallback response must copy all 512 FIFO bytes exactly")
    require(fifo_mem.data_reads == 512 and fifo_mem.pops == 512 and
            not fifo_mem.fifo,
            "fallback response must perform exactly 512 DATA/DPOP pairs")
    require(fifo_mem.data[generated_roms()[2]["ADDRH"]] == 0x12,
            "fallback response must restore the caller's buffer page")


def run_wrprep(ctrl: int) -> tuple[SmartportMemory, int]:
    _slot, cont, symbols = generated_roms()
    memory = SmartportMemory(ctrl, b"\x00" if (ctrl & 0x20) else b"")
    memory.data[0xC800:0xC800 + len(cont)] = cont
    source = 0x1200
    memory.data[source:source + 512] = bytes((i * 17) & 0xFF for i in range(512))
    memory.data[symbols["ADDRL"]] = source & 0xFF
    memory.data[symbols["ADDRH"]] = source >> 8

    sentinel = 0x0600
    mpu = MPU(memory=memory, pc=symbols["WRPREP"])
    mpu.a = 0x81
    mpu.sp = 0xFD
    return_address = sentinel - 1
    memory.data[0x01FE] = return_address & 0xFF
    memory.data[0x01FF] = return_address >> 8
    for steps in range(20000):
        if mpu.pc == sentinel:
            return memory, steps
        mpu.step()
    raise TestFailure("WRPREP did not return within 20000 instructions")


def test_vtw_write_preflight_rom_contract() -> None:
    direct, direct_steps = run_wrprep(0xE0)
    require(direct.ctrl_writes == [0x81] and direct.pops == 1,
            "direct vTW write must issue exactly one preflight")
    require(direct.data_writes == 0 and direct_steps < 30,
            "accepted direct write must omit the 512-byte payload")

    fallback, _steps = run_wrprep(0xA0)
    require(fallback.ctrl_writes == [0x81] and fallback.pops == 1,
            "rejected vTW direct write must still finish its preflight")
    require(fallback.data_writes == 512,
            "rejected vTW direct write must send the complete payload")

    native, _steps = run_wrprep(0x80)
    require(native.ctrl_writes == [] and native.pops == 0 and
            native.data_writes == 512,
            "native writes must keep the original payload-only flow")


def test_vivado_sources_include_active_smartport_roms() -> None:
    sources = HDL_SOURCES.read_text(encoding="utf-8")

    require("apple/smartport_a2retronet_style_c700.mem" in sources,
            "Vivado source list must include the active SmartPort C700 ROM")
    require("apple/smartport_a2retronet_style_c800.mem" in sources,
            "Vivado source list must include the active SmartPort C800 ROM")
TESTS = [
    test_generated_mem_files_are_current,
    test_prodos_and_smartport_entries,
    test_block_helpers_preserve_prodos_buffer_page,
    test_vtw_direct_block_rom_contract,
    test_vtw_write_preflight_rom_contract,
    test_vivado_sources_include_active_smartport_roms,
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
        print(f"{len(TESTS) - len(failures)} of {len(TESTS)} SmartPort ROM tests passed; "
              f"{len(failures)} failed")
        return 1
    print(f"{len(TESTS)} SmartPort ROM tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
