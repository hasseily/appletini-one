#!/usr/bin/env python3
"""Run the real IIe/boot/ProDOS/BASIC ROM paths behind native PR#3.

This is an instruction/MMU regression, not a cycle-accurate bus simulation.
Only the ARM policy acknowledgement is supplied by the fixture; the production
policy and its delayed-MMIO cases run in test_vtw_boot_start.py.
"""

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path

from py65.devices.mpu65c02 import MPU

ROOT = Path(__file__).resolve().parents[1]
ROM_SHA256 = "aab38a03ca8deabbb2f868733148c2efd6f655a59cd9c5d058ef3e0b7aa86a1a"
DISK_SHA256 = "398d333cb2ab92df9f8bb2cf64b946f2567116910eb8359cf4bdee5d4194f0fa"
PRODOS_SHA256 = "df45f0e82a2a96a8bddb6af408284f6d38160aaa100895e722e9f4bbe32f9e5b"
BASIC_SHA256 = "eddd5e529fcb0def3b667b8311072b29338b584b5b07beca200a1fbeaf91cc52"


def load_builder():
    spec = importlib.util.spec_from_file_location(
        "slot3_boot_builder", ROOT / "scripts/build_vtw_boot_start_rom.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def checked_bytes(path, digest):
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, f"fixture changed: {path}"
    return data


def sapling(disk, key, length, digest):
    index = disk[key * 512:(key + 1) * 512]
    blocks = [index[i] | index[i + 256] << 8 for i in range((length + 511) // 512)]
    assert all(0 <= block < len(disk) // 512 for block in blocks)
    data = b"".join(disk[block * 512:(block + 1) * 512] if block else bytes(512)
                    for block in blocks)[:length]
    assert hashlib.sha256(data).hexdigest() == digest
    return data


class AppleMemory:
    """IIe softswitches used by the real cold aux probe and slot-3 scan.

    Open auxiliary RAM and an empty external slot return $FF. INTCXROM,
    SLOTC3ROM, internal-C8 selection, ALTZP, 80STORE and PAGE2 follow the
    motherboard mapping; no test substitutes a result for the aux probe.
    """

    def __init__(self, rom, slot, c8, physical_aux=False, provided=False,
                 virtual_aux=False):
        self.rom, self.slot, self.c8 = rom, slot, bytearray(c8)
        self.ram, self.aux = bytearray(65536), bytearray(65536)
        self.physical_aux, self.provided = physical_aux, provided
        self.virtual_aux = virtual_aux
        self.intcx = self.slot3_external = self.altzp = self.store80 = self.page2 = False
        self.intc8 = False
        self.boot_mapping = False
        self.columns80 = self.altchar = False
        self.c00a_writes = 0
        self.events = []

    @property
    def aux_available(self):
        return self.physical_aux or self.provided or self.virtual_aux

    def __getitem__(self, address):
        if isinstance(address, slice):
            return [self[a] for a in range(*address.indices(65536))]
        address &= 65535
        if 0xC000 <= address < 0xC100:
            if address in (0xC000, 0xC010):
                return 0
            status = {0xC015: self.intcx, 0xC016: self.altzp,
                      0xC017: self.slot3_external, 0xC018: self.store80,
                      0xC01C: self.page2, 0xC01E: self.altchar,
                      0xC01F: self.columns80}
            if address in status:
                return 0x80 if status[address] else 0
            if address in (0xC054, 0xC055):
                self.page2 = address == 0xC055
            return 0
        if 0xC100 <= address < 0xC800:
            if self.intcx or 0xC300 <= address < 0xC400 and not self.slot3_external:
                if 0xC300 <= address < 0xC400:
                    self.intc8 = True
                return self.rom[address - 0xC000]
            if 0xC700 <= address < 0xC800:
                return self.slot[address - 0xC700]
            return 0xFF
        if 0xC800 <= address < 0xD000:
            if address == 0xCFFF:
                self.intc8 = False
            if self.boot_mapping and 0xC800 <= address < 0xC900:
                return self.c8[address - 0xC800]
            if self.boot_mapping and 0xCA00 <= address < 0xCA10:
                return self.ram[address]
            # The internal monitor sets INTCXROM before running C2E2/CA89.
            if self.intcx or self.intc8:
                return self.rom[address - 0xC000]
            return 0xFF
        if address >= 0xD000:
            return self.rom[address - 0xC000]
        use_aux = address < 0x200 and self.altzp or (
            0x400 <= address < 0x800 and self.store80 and self.page2)
        if use_aux:
            return self.aux[address] if self.aux_available else 0xFF
        return self.ram[address]

    def __setitem__(self, address, value):
        if isinstance(address, slice):
            indices = range(*address.indices(65536))
            assert len(indices) == len(value)
            for a, v in zip(indices, value):
                self[a] = v
            return
        address &= 65535
        value &= 255
        switches = {0xC000: ("store80", False), 0xC001: ("store80", True),
                    0xC006: ("intcx", False), 0xC007: ("intcx", True),
                    0xC008: ("altzp", False), 0xC009: ("altzp", True),
                    0xC00A: ("slot3_external", False), 0xC00B: ("slot3_external", True),
                    0xC00C: ("columns80", False), 0xC00D: ("columns80", True),
                    0xC00E: ("altchar", False), 0xC00F: ("altchar", True),
                    0xC054: ("page2", False), 0xC055: ("page2", True)}
        if address in switches:
            name, state = switches[address]
            setattr(self, name, state)
            if address == 0xC00A:
                self.c00a_writes += 1
            return
        if address == 0xC0F0:
            self.events.append(value)
            if value == 0x26:
                self.provided = False  # Existing PL physical-aux probe override.
            return
        if address >= 0xC100 and not 0xCA00 <= address < 0xCA10:
            return
        use_aux = address < 0x200 and self.altzp or (
            0x400 <= address < 0x800 and self.store80 and self.page2)
        if use_aux:
            if self.aux_available:
                self.aux[address] = value
        else:
            self.ram[address] = value


def run(cpu, until, limit=200000, hook=None):
    for _ in range(limit):
        if cpu.pc == until:
            return
        if hook:
            hook(cpu)
        cpu.step()
    raise AssertionError(f"real code did not return: PC=${cpu.pc:04X}")


def call(cpu, address, hook=None):
    cpu.sp = 0xFF
    cpu.stPushWord(0x01FF)  # RTS returns to $0200, outside all tested routines.
    cpu.pc = address
    run(cpu, 0x0200, hook=hook)


def cold_probe(cpu, memory):
    # Execute the complete motherboard cold-reset path, including C2E2/CA89
    # and its autostart slot-signature scan. Stop at the chosen card's entry.
    seen = set()
    cpu.pc = memory[0xFFFC] | memory[0xFFFD] << 8
    run(cpu, 0xC700, hook=lambda current: seen.add(current.pc))
    assert {0xFA62, 0xC2E2, 0xCA89, 0xFAB4} <= seen
    assert memory.intcx is False
    assert memory.slot3_external == (not memory.aux_available)


def boot_report(cpu, memory, symbols, selector, old=False):
    memory.boot_mapping = True
    memory.ram[0xCA00] = 0x70  # Actual boot ROM's SLOT16 scratch register.
    gate = symbols.get("VTW_BOOT_GATE_CONTROL")
    polls = 0
    if gate is not None:
        memory.c8[gate + 1 - 0xC800] = 1
        memory.c8[symbols["NATIVE_SLOT3_CONTROL"] + 1 - 0xC800] = 0

    def acknowledgement(current):
        nonlocal polls
        if gate is not None and current.pc == gate:
            polls += 1
            assert memory.events[-2:] == [0x31 if memory.physical_aux else 0x30, 0x23]
            assert polls <= 40, "native ROM did not resume after policy acknowledgement"
            if polls == 30:
                memory.provided = selector
                memory.c8[symbols["NATIVE_SLOT3_CONTROL"] + 1 - 0xC800] = int(selector)
                memory.c8[gate + 1 - 0xC800] = 0

    call(cpu, symbols["MACHINE_ID_REPORT"], acknowledgement)
    memory.boot_mapping = False
    memory.intc8 = False
    if not old:
        assert polls == 30
    return polls


def prodos_slot_scan(cpu, memory, prodos):
    memory.ram[0x2000:0x2000 + len(prodos)] = prodos
    memory.ram[0xBF99] = 0x86  # Slots 1, 2 and 7 already scanned on the supplied machine.
    memory.ram[0x0C] = 0xB0  # ProDOS's working machine flags before the slot scan.
    memory.ram[0x10:0x12] = bytes((0, 0xC3))
    memory.intcx = memory.intc8 = False
    cpu.p |= cpu.CARRY
    call(cpu, 0x2783)
    call(cpu, 0x276D)  # Actual end-of-scan publication of the working flags.
    return memory.ram[0xBF99]


def basic_pr3(cpu, memory, basic):
    memory.ram[0x2000:0x2000 + len(basic)] = basic
    # Execute BASIC.SYSTEM's own two relocation loops, then its slot-vector scan.
    cpu.pc, cpu.sp = 0x2047, 0xFF
    run(cpu, 0x2064)
    assert memory.ram[0xBE16:0xBE18] == bytes((0xEE, 0x9A))
    cpu.pc = 0x20FA
    run(cpu, 0x211B)
    vector = memory.ram[0xBE16] | memory.ram[0xBE17] << 8
    memory.ram[0xBE6B] = 3
    memory.ram[0xBE56] = 0x40
    memory.ram[0xBE57] = 0
    # Actual BASIC.SYSTEM PR# dispatcher: slot-vector lookup and device check.
    call(cpu, 0xB003)
    error = cpu.a if cpu.p & cpu.CARRY else 0
    output = memory.ram[0x36] | memory.ram[0x37] << 8
    if not error:
        assert output == 0xC300
        cpu.a = 0
        call(cpu, output)
    return vector, error, memory.columns80


def main():
    rom = checked_bytes(ROOT / "Assets/ROMs/Apple2e_Enhanced.rom", ROM_SHA256)
    # The production source embeds this exact ROM; check length and reset entry.
    assert len(rom) == 16384 and rom[-4:-2] == bytes((0x62, 0xFA))
    disk = checked_bytes(ROOT / "software/ProDOS_2_4_3.po", DISK_SHA256)
    prodos = sapling(disk, 7, 17128, PRODOS_SHA256)
    basic = sapling(disk, 42, 10240, BASIC_SHA256)
    builder = load_builder()
    slot, _reloc, c8, symbols = builder.build()
    assert builder.HEADER.read_bytes() == builder.header_text().encode("ascii")
    old_builder = importlib.util.spec_from_file_location(
        "retained_boot_builder", ROOT / "scripts/build_boot_menu_rom.py")
    old_module = importlib.util.module_from_spec(old_builder)
    old_builder.loader.exec_module(old_module)
    _old_slot, _old_reloc, old_c8, old_symbols = old_module.build()

    for physical_aux, configured_ram, old in ((False, True, True),
                                             (False, True, False),
                                             (True, True, False),
                                             (False, False, False)):
        memory = AppleMemory(rom, slot, old_c8 if old else c8, physical_aux)
        cpu = MPU(memory=memory)
        cold_probe(cpu, memory)
        initial_writes = memory.c00a_writes
        selector = not physical_aux and configured_ram
        boot_report(cpu, memory, old_symbols if old else symbols, selector, old=old)
        if old:
            memory.provided = True  # Old ARM policy supplies RAM after the ROM probe.
        if not selector:
            assert memory.c00a_writes == initial_writes, "boot must preserve physical/disabled C3 selection"
        bitmap = prodos_slot_scan(cpu, memory, prodos)
        vector, error, columns80 = basic_pr3(cpu, memory, basic)
        available = physical_aux or configured_ram and not old
        assert bitmap == (0x8E if available else 0x86)
        assert memory.ram[0xBF98] == (0xB2 if available else 0xB0)
        assert vector == (0xC300 if available else 0x9AEE)
        assert error == (0 if available else 3)
        assert columns80 == available
        if old:
            # The same cached vector survives later ProDOS C00A. PR#7 rebuilds
            # it, explaining both parts of the hardware report.
            memory[0xC00A] = 0
            assert memory.ram[0xBE16:0xBE18] == bytes((0xEE, 0x9A))
            assert prodos_slot_scan(cpu, memory, prodos) == 0x8E
            assert basic_pr3(cpu, memory, basic) == (0xC300, 0, True)
        else:
            # A warm PR#7 must retain the working result without another patch.
            assert prodos_slot_scan(cpu, memory, prodos) == bitmap
            assert basic_pr3(cpu, memory, basic) == (vector, error, columns80)
    print("PASS real IIe cold reset + C8 handoff + ProDOS 2.4.3 scan + BASIC PR#3")
    print("PASS negative control reproduces $86/$9AEE and PR#7 recovery; fixed first boot $8E/$C300")
    print("PASS physical auxiliary card and disabled Appletini RAM preserve native slot selection")

    # vTW's private aux RAM already exists before its ROM cold probe. The native
    # selector stays zero; its own cold ROM and PR#3 path still choose 80 columns.
    virtual = AppleMemory(rom, slot, c8, virtual_aux=True)
    cpu = MPU(memory=virtual)
    cold_probe(cpu, virtual)
    assert virtual.slot3_external is False
    native_c3_writes = virtual.c00a_writes
    virtual.boot_mapping = True
    virtual.ram[0xCA00] = 0x70
    virtual.c8[symbols["VTW_BOOT_GATE_CONTROL"] + 1 - 0xC800] = 0
    virtual.c8[symbols["NATIVE_SLOT3_CONTROL"] + 1 - 0xC800] = 0
    call(cpu, symbols["MACHINE_ID_REPORT"])
    assert virtual.events == [0x26, 0x31, 0x23]  # The probe sees private vTW RAM.
    assert virtual.c00a_writes == native_c3_writes
    virtual.boot_mapping = virtual.intc8 = False
    assert prodos_slot_scan(cpu, virtual, prodos) == 0x8E
    assert basic_pr3(cpu, virtual, basic) == (0xC300, 0, True)
    print("PASS vTW cold ROM with private aux RAM selects 80 columns without native C3 forcing")


if __name__ == "__main__":
    main()
