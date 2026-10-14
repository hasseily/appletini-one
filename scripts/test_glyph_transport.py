#!/usr/bin/env python3
"""Run the shipped 6502 SmartPort ROM against the real ARM glyph parser.

The FIFO/renderer backend is a host model; this does not prove board timing.
Requires py65 and a native C compiler (CC can select it).
"""
from pathlib import Path
import ctypes
import os
import shutil
import struct
import subprocess
import tempfile
import zlib

from py65.devices.mpu6502 import MPU
from test_smartport_rom import generated_roms

ROOT = Path(__file__).resolve().parents[1]
APPLY = ctypes.CFUNCTYPE(ctypes.c_uint8, ctypes.c_void_p,
                        ctypes.POINTER(ctypes.c_uint8))
STATUS = ctypes.CFUNCTYPE(None, ctypes.c_void_p,
                         ctypes.POINTER(ctypes.c_uint16),
                         ctypes.POINTER(ctypes.c_uint32))


class Backend(ctypes.Structure):
    _fields_ = [("context", ctypes.c_void_p), ("apply", APPLY), ("status", STATUS)]


class Card:
    def __init__(self, library, accelerated):
        self.library = library
        self.memory = bytearray(65536)
        slot, cont, _ = generated_roms()
        self.memory[0xC700:0xC800] = slot
        self.memory[0xC800:0xC800 + len(cont)] = cont
        self.input = bytearray()
        self.output = bytearray()
        self.frames = []
        self.accelerated = accelerated
        self.active = None
        self.generation = 0
        self.apply_callback = APPLY(self.apply)
        self.status_callback = STATUS(self.status)
        self.backend = Backend(None, self.apply_callback, self.status_callback)
        library.glyph_api_reset()

    def apply(self, context, rom):
        self.active = ctypes.string_at(rom, 4096) if rom else None
        self.generation += 1
        return 0

    def status(self, context, flags, generation):
        flags[0] = 1 | (2 if self.active is not None else 0)
        generation[0] = self.generation

    def __getitem__(self, address):
        if address == 0xCFF0:
            assert self.output, "ROM tried to read an empty response FIFO"
            return self.output[0]
        if address == 0xCFF1:
            return 0x80 | (0x20 if self.accelerated else 0)
        return self.memory[address & 65535]

    def __setitem__(self, address, value):
        value &= 255
        if address == 0xCFF0:
            self.input.append(value)
            assert len(self.input) <= 1024, "request exceeds FPGA FIFO"
        elif address == 0xCFF1:
            assert value == 2, "unexpected SmartPort command family"
            self.execute()
        elif address == 0xCFF2:
            assert self.output
            del self.output[0]
        elif address < 0xC000 or address >= 0xD000:
            self.memory[address & 65535] = value

    def execute(self):
        frame = bytes(self.input)
        self.frames.append(frame)
        self.input.clear()
        assert len(frame) >= 10
        command, count, unit = frame[:3]
        selector = frame[5]
        if unit != 0 or selector != 0x81:
            self.output = bytearray([0x21])
        elif command == 0:
            data = (ctypes.c_uint8 * 32)()
            self.library.glyph_api_status(data, ctypes.byref(self.backend))
            self.output = bytearray(b"\x00\x20\x00" + bytes(data))
        else:
            assert command == 4
            length = struct.unpack_from("<H", frame, 10)[0]
            assert length + 12 == len(frame)
            assert count == 3
            payload = (ctypes.c_uint8 * length).from_buffer_copy(frame[12:])
            result = self.library.glyph_api_execute(payload, length,
                                                    ctypes.byref(self.backend))
            self.output = bytearray([result])

    def call(self, command, payload=b"", selector=0x81, unit=0):
        # JSR C70D, inline command and parameter-list pointer; stop after RTS.
        self.memory[0x200:0x207] = bytes([0x20, 0x0D, 0xC7, command, 0, 3, 0xEA])
        # Include four deliberately nonzero padding bytes: ROM streams all nine.
        self.memory[0x300:0x309] = bytes([3, unit, 0, 0x12, selector,
                                               0xA5, 0x5A, 0x11, 0x22])
        if command == 4:
            self.memory[0x1200:0x1202 + len(payload)] = (
                struct.pack("<H", len(payload)) + payload)
        saved_zp = bytes(range(5))
        self.memory[0x42:0x47] = saved_zp
        cpu = MPU(memory=self, pc=0x200)
        cpu.sp = 0xEF
        for _ in range(40000):
            if cpu.pc == 0x206:
                assert cpu.sp == 0xEF
                assert self.memory[0x42:0x47] == saved_zp
                assert not self.output, "ROM left response bytes unread"
                assert bool(cpu.p & 1) == bool(cpu.a)
                return cpu.a
            cpu.step()
        raise AssertionError(f"SmartPort ROM did not return: PC=${cpu.pc:04X}")


def header(operation, transaction=1, offset=0, count=0):
    return struct.pack("<4sBBBBIHH", b"AGLY", 1, operation, 0, 0,
                       transaction, offset, count)


def test_card(library, accelerated):
    card = Card(library, accelerated)
    assert card.call(0) == 0
    assert card.memory[0x1200:0x1204] == b"AGLY"
    assert card.call(4, header(1)) == 0
    image = bytes((i * 29 + i // 8) & 255 for i in range(4096))
    for offset in range(0, 4096, 512):
        assert card.call(4, header(2, offset=offset, count=512) +
                         image[offset:offset + 512]) == 0
        assert len(card.frames[-1]) == 540
        assert card.frames[-1][6:10] == b"\xA5\x5A\x11\x22"
        assert card.active is None
    assert card.call(4, header(3) + struct.pack("<I", zlib.crc32(image))) == 0
    assert card.active == image
    assert card.call(0) == 0
    assert card.memory[0x1208] & 2
    assert card.call(4, header(5, transaction=0)) == 0
    assert card.active is None
    assert card.call(4, header(1), unit=1) == 0x21
    assert card.call(4, header(1), selector=0x82) == 0x21


def main():
    compiler = os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")
    if not compiler:
        raise RuntimeError("A native C compiler is required")
    with tempfile.TemporaryDirectory(prefix="glyph-transport-") as temporary:
        module = Path(temporary) / ("glyph.dll" if os.name == "nt" else "glyph.so")
        subprocess.run([compiler, "-shared", "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-O1", *([] if os.name == "nt" else ["-fPIC"]),
                        str(ROOT / "ps_sources/frontend/glyph_api.c"),
                        "-o", str(module)], check=True)
        library = ctypes.CDLL(str(module))
        library.glyph_api_execute.argtypes = [ctypes.POINTER(ctypes.c_uint8),
                                             ctypes.c_uint16,
                                             ctypes.POINTER(Backend)]
        library.glyph_api_execute.restype = ctypes.c_uint8
        library.glyph_api_status.argtypes = [ctypes.POINTER(ctypes.c_uint8),
                                            ctypes.POINTER(Backend)]
        test_card(library, False)
        test_card(library, True)
        if os.name == "nt":
            # Windows cannot remove a loaded DLL, even after the Python object dies.
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(library._handle))
    print("PASS shipped 6502 ROM -> glyph core: native/vTW, 8 chunks, CRC, restore, errors")


if __name__ == "__main__":
    main()
