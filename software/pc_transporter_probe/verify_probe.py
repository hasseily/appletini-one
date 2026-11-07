"""Run the 65C02 binary against a small PC Transporter register model."""

import argparse
from pathlib import Path

from py65.devices.mpu65c02 import MPU


LOAD_ADDRESS = 0x2000
MONITOR_ENTRY = 0x200C
RETURN_ADDRESS = 0xF000
COUT = 0xFDED
PRBYTE = 0xFDDA
MONZ = 0xFF69


class AppleBus(list):
    def __init__(self, fault):
        super().__init__([0] * 0x10000)
        self.fault = fault
        self.selected = False
        self.pointer = 0x071295
        self.cc = 0x0845
        self.ce = 0x0967
        self.pc_ram = {}
        self.ram_writes = 0
        self[COUT] = 0x60
        self[PRBYTE] = 0x60

    def ram_read(self, address):
        if address >= 0xC0000:
            return 0xFF
        default = (address ^ (address >> 8) ^ (address >> 16) ^ 0x76) & 0xFF
        return self.pc_ram.get(address, default)

    def ram_write(self, address, value):
        if address < 0xC0000:
            self.pc_ram[address] = value
            self.ram_writes += 1

    def increment(self):
        if self.fault == "low-carry":
            self.pointer = (self.pointer & 0xFFFF00) | ((self.pointer + 1) & 0xFF)
        else:
            self.pointer = (self.pointer + 1) & 0xFFFFFF

    def __getitem__(self, address):
        if not isinstance(address, int) or not 0xC000 <= address <= 0xCFFF:
            return super().__getitem__(address)
        if address == 0xC015:
            return 0x80 if self.fault == "internal-rom" else 0x00
        if address == 0xC500:
            self.selected = self.fault != "absent"
            return 0xFF if self.selected else 0xD7
        if address == 0xCFFF:
            if self.fault != "no-release":
                self.selected = False
            return 0xEE
        if not self.selected:
            return 0xEE
        if 0xCC00 <= address <= 0xCDFF:
            return self.ram_read((self.cc << 8) + (address - 0xCC00))
        if 0xCE00 <= address <= 0xCEFF:
            return self.ram_read((self.ce << 8) + (address - 0xCE00))
        if 0xCF00 <= address <= 0xCF02:
            return (self.pointer >> (8 * (address - 0xCF00))) & 0xFF
        if address in (0xCF03, 0xCF04):
            result = self.ram_read(self.pointer)
            if address == 0xCF03:
                self.increment()
            return result
        if address == 0xCF30:
            return 0x80 if self.fault == "pending" else 0x00
        if address == 0xCF31:
            return 0x00 if self.fault == "running" else 0x10
        return {
            0xCF05: self.cc >> 8,
            0xCF06: self.cc & 0xFF,
            0xCF0E: self.ce >> 8,
            0xCF0F: self.ce & 0xFF,
        }.get(address, 0xFF)

    def __setitem__(self, address, value):
        if not isinstance(address, int) or not 0xC000 <= address <= 0xCFFF:
            super().__setitem__(address, value)
            return
        if not self.selected:
            return
        if 0xCC00 <= address <= 0xCDFF:
            self.ram_write((self.cc << 8) + (address - 0xCC00), value)
        elif 0xCE00 <= address <= 0xCEFF:
            self.ram_write((self.ce << 8) + (address - 0xCE00), value)
        elif 0xCF00 <= address <= 0xCF02:
            shift = 8 * (address - 0xCF00)
            self.pointer = (self.pointer & ~(0xFF << shift)) | (value << shift)
        elif address in (0xCF03, 0xCF04):
            self.ram_write(self.pointer, value)
            if address == 0xCF03:
                self.increment()
        elif address == 0xCF05:
            if self.fault != "stale-cc-bank" or value != 3:
                self.cc = (self.cc & 0xFF) | (value << 8)
        elif address == 0xCF06:
            self.cc = (self.cc & 0xFF00) | value
        elif address == 0xCF0E:
            self.ce = (self.ce & 0xFF) | (value << 8)
        elif address == 0xCF0F:
            if self.fault != "stale-ce-page" or value != 0x81:
                self.ce = (self.ce & 0xFF00) | value


def verify(program, fault, code, *, monitor=False):
    bus = AppleBus(fault)
    old_registers = bus.pointer, bus.cc, bus.ce
    bus[LOAD_ADDRESS:LOAD_ADDRESS + len(program)] = program
    cpu = MPU(memory=bus, pc=MONITOR_ENTRY if monitor else LOAD_ADDRESS)
    cpu.a, cpu.x, cpu.y = 0x14, 0x25, 0x36
    cpu.p |= cpu.DECIMAL
    expected_flags = cpu.p & (0xC7 if monitor else 0xCF)
    old_cpu = cpu.a, cpu.x, cpu.y, expected_flags, cpu.sp
    if not monitor:
        cpu.stPushWord(RETURN_ADDRESS - 1)
    output = []
    for _ in range(10_000):
        if cpu.pc == (MONZ if monitor else RETURN_ADDRESS):
            break
        if cpu.pc == COUT:
            output.append(chr(cpu.a & 0x7F))
        elif cpu.pc == PRBYTE:
            output.append(f"{cpu.a:02X}")
        cpu.step()
    else:
        raise AssertionError(f"{fault}: probe did not return")

    assert bytes(bus[0x2003:0x2007]) == b"PCT5", fault
    assert bus[0x2007] == code, f"{fault}: status {bus[0x2007]:02X}, expected {code:02X}"
    assert "".join(output) == f"\rPCT5 RESULT {code:02X}\r", fault
    assert (cpu.a, cpu.x, cpu.y, cpu.p & 0xCF, cpu.sp) == old_cpu, fault
    assert (bus.pointer, bus.cc, bus.ce) == old_registers, fault
    assert not bus.selected or fault == "no-release", fault
    if fault in {"internal-rom", "absent", "running", "pending", "no-release"}:
        assert bus.ram_writes == 0, fault
    else:
        assert bus.ram_writes > 0, fault
        original = AppleBus(None)
        assert all(value == original.ram_read(address)
                   for address, value in bus.pc_ram.items()), fault
    name = "monitor entry" if monitor else fault or "correct model"
    print(f"{name}: status {code:02X}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    program = parser.parse_args().binary.read_bytes()
    for fault, code in (
        (None, 0x00),
        ("internal-rom", 0x01),
        ("absent", 0x02),
        ("running", 0x03),
        ("pending", 0x03),
        ("no-release", 0x05),
        ("low-carry", 0x11),
        ("stale-cc-bank", 0x21),
        ("stale-ce-page", 0x26),
    ):
        verify(program, fault, code)
    verify(program, None, 0, monitor=True)


if __name__ == "__main__":
    main()
