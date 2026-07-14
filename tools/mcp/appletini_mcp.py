#!/usr/bin/env python3
"""Appletini MCP server -- drive a real Apple II from an MCP client.

Bridges MCP tools onto the Appletini's UART command console:

    MCP client -> this server -> serial (UART0) -> ARM firmware
                -> FPGA fabric -> Apple II bus pins

Everything here rides the firmware's existing `cmd>` console (mrd/mwr,
bustail, sswatch, status, ...). The highest-value trick: the
apple_cycle_egress keeps write-mirror shadows of the Apple's main and
aux memory banks in DDR (0x3F100000 / 0x3F110000), so this server can
read the live text screen -- and any RAM the running software has
written -- without touching the Apple bus at all.

Configuration (environment):
    APPLETINI_PORT   serial port (e.g. COM5 or /dev/ttyUSB0)  [required]
    APPLETINI_BAUD   baud rate                                [115200]

Run:
    pip install mcp pyserial
    python appletini_mcp.py            # stdio transport

Register with Claude Code:
    claude mcp add appletini -e APPLETINI_PORT=COM5 -- python tools/mcp/appletini_mcp.py

Note: the firmware console is a single shared channel. Close any
terminal program holding the port before starting this server.
"""

from __future__ import annotations

import os
import re
import threading

import serial  # pyserial
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("appletini")

# ---------------------------------------------------------------------------
# Serial console transport
# ---------------------------------------------------------------------------

PROMPT = b"cmd> "

# Firmware register map (CARD_CTRL base 0x40000000, word index * 4).
REG_SOFTSW = 0x40000008
REG_DEADLINE_MISS = 0x40000050
REG_MACHINE_MODE = 0x40000180
REG_AUX_PROVIDE = 0x40000184
REG_RAMWORKS_EN = 0x40000188
REG_LOST_CYCLES = 0x40000198
REG_WQ_DROPS = 0x4000019C
REG_AUX_PROBE = 0x400001A8

# apple_cycle_egress write-mirror shadows of Apple memory (DDR).
SHADOW_MAIN = 0x3F100000
SHADOW_AUX = 0x3F110000

_lock = threading.Lock()
_ser: serial.Serial | None = None


def _port() -> serial.Serial:
    global _ser
    if _ser is not None and _ser.is_open:
        return _ser
    name = os.environ.get("APPLETINI_PORT")
    if not name:
        raise RuntimeError("set APPLETINI_PORT (e.g. COM5) in the environment")
    baud = int(os.environ.get("APPLETINI_BAUD", "115200"))
    _ser = serial.Serial(name, baud, timeout=0.25)
    return _ser


def _read_until_prompt(ser: serial.Serial, timeout_s: float) -> bytes:
    import time
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = ser.read(256)
        if chunk:
            buf += chunk
            if buf.endswith(PROMPT):
                return bytes(buf)
        elif buf.endswith(PROMPT):
            return bytes(buf)
    return bytes(buf)


def console(command: str, timeout_s: float = 3.0) -> str:
    """Run one firmware console command, return its output text."""
    with _lock:
        ser = _port()
        ser.reset_input_buffer()
        # A bare CR either redraws the cmd> prompt (already in command
        # mode) or is ignored in single-key mode; ':' enters command
        # mode from single-key mode. Do both, then sync on the prompt.
        ser.write(b"\r")
        if not _read_until_prompt(ser, 0.4).endswith(PROMPT):
            ser.write(b":")
            _read_until_prompt(ser, 0.6)
        ser.write(command.encode("ascii") + b"\r")
        raw = _read_until_prompt(ser, timeout_s)
    text = raw.decode("ascii", errors="replace")
    # Strip the echoed command and the trailing prompt.
    text = text.rsplit("cmd> ", 1)[0]
    lines = [ln.rstrip("\r") for ln in text.split("\n")]
    if lines and command in lines[0]:
        lines = lines[1:]
    return "\n".join(ln for ln in lines if ln.strip("\r\n \t"))


_MRD_LINE = re.compile(r"0x([0-9A-Fa-f]{8}): 0x([0-9A-Fa-f]{8})")


def read_words(addr: int, count: int) -> list[int]:
    """Read `count` 32-bit words via mrd (16 words per console call)."""
    words: list[int] = []
    while count > 0:
        n = min(count, 16)
        out = console(f"mrd 0x{addr:08X} {n}")
        got = _MRD_LINE.findall(out)
        if len(got) != n:
            raise RuntimeError(f"mrd 0x{addr:08X} {n}: unexpected reply:\n{out}")
        words.extend(int(v, 16) for _, v in got)
        addr += n * 4
        count -= n
    return words


def read_bytes(addr: int, length: int) -> bytes:
    start = addr & ~3
    end = (addr + length + 3) & ~3
    words = read_words(start, (end - start) // 4)
    blob = b"".join(w.to_bytes(4, "little") for w in words)
    off = addr - start
    return blob[off:off + length]


# ---------------------------------------------------------------------------
# Apple II text screen decoding
# ---------------------------------------------------------------------------

def _row_base(row: int) -> int:
    return 0x400 + (row % 8) * 0x80 + (row // 8) * 0x28


def _char(b: int) -> str:
    c = b & 0x7F
    if c < 0x20:
        c |= 0x40
    return chr(c) if 0x20 <= c <= 0x7E else "?"


# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

@mcp.tool()
def apple2_screen_text() -> str:
    """Read the Apple II's live text screen as plain text (40 or 80
    columns, chosen by the live 80COL soft switch). Works even when a
    graphics mode is displayed -- returns whatever is on the text page
    underneath, which is where ProDOS/BASIC error messages land."""
    sw = read_words(REG_SOFTSW, 1)[0]
    col80 = bool(sw & (1 << 9))
    rows = []
    for row in range(24):
        base = _row_base(row)
        main = read_bytes(SHADOW_MAIN + base, 40)
        if col80:
            aux = read_bytes(SHADOW_AUX + base, 40)
            line = "".join(_char(aux[i]) + _char(main[i]) for i in range(40))
        else:
            line = "".join(_char(b) for b in main)
        rows.append(line.rstrip())
    return "\n".join(rows)


@mcp.tool()
def apple2_peek(address: int, length: int = 16) -> str:
    """Read Apple II main-memory bytes (hex dump) from the write-mirror
    shadow. `address` is the Apple-side address (0x0000-0xFFFF).
    Caveat: the shadow mirrors WRITES since power-on; ROM regions and
    never-written RAM read as zero."""
    if not (0 <= address <= 0xFFFF) or length <= 0 or length > 512:
        return "address must be 0x0000-0xFFFF, length 1-512"
    data = read_bytes(SHADOW_MAIN + address, length)
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexs = " ".join(f"{b:02X}" for b in chunk)
        text = "".join(_char(b) if b else "." for b in chunk)
        lines.append(f"{address + i:04X}: {hexs:<47} {text}")
    return "\n".join(lines)


@mcp.tool()
def apple2_soft_switches() -> str:
    """Decode the live //e soft-switch state and RamWorks bank."""
    v = read_words(REG_SOFTSW, 1)[0]
    names = ["80STORE", "RAMRD", "RAMWRT", "ALTZP", "TEXT", "MIXED",
             "PAGE2", "HIRES", "ALTCHARSET", "80COL", "DHIRES",
             "LCBANK2", "LCRAM_READ", "LCRAM_WRITE"]
    on = [n for i, n in enumerate(names) if v & (1 << i)]
    bank = (v >> 14) & 0x7F
    return (f"raw=0x{v:08X}\nactive: {', '.join(on) if on else '(none)'}\n"
            f"ramworks bank: {bank}")


@mcp.tool()
def apple2_machine_status() -> str:
    """Machine identification and memory-provider state: machine mode,
    aux-provide, RamWorks enable, physical-aux-card probe result, plus
    the firmware's own status summary."""
    mode, provide, rw, probe = (read_words(REG_MACHINE_MODE, 1)[0],
                                read_words(REG_AUX_PROVIDE, 1)[0],
                                read_words(REG_RAMWORKS_EN, 1)[0],
                                read_words(REG_AUX_PROBE, 1)[0])
    status = console("status", timeout_s=5.0)
    return (f"machine mode: {mode}\naux provide: {provide & 1}\n"
            f"ramworks: {rw & 1}\n"
            f"aux probe: valid={bool(probe & 2)} card_present={bool(probe & 1)}\n"
            f"--- status ---\n{status}")


@mcp.tool()
def apple2_health() -> str:
    """Serve-path health counters. All three must stay zero on a
    healthy system: write-queue drops, serve deadline misses, and
    lost bus cycles."""
    drops = read_words(REG_WQ_DROPS, 1)[0]
    misses = read_words(REG_DEADLINE_MISS, 1)[0]
    lost = read_words(REG_LOST_CYCLES, 1)[0]
    verdict = "OK" if (drops | misses | lost) == 0 else "ATTENTION"
    return (f"write-queue drops: {drops}\ndeadline misses: {misses}\n"
            f"lost cycles: {lost}\nverdict: {verdict}")


@mcp.tool()
def apple2_bus_trace(count: int = 32, back_offset: int = 0) -> str:
    """Capture recent Apple bus cycles (address, data, R/W) from the
    trace ring. `back_offset` looks further into history (0 = most
    recent). Arms the ring if needed. Each line: R/W ADDR DATA."""
    if count < 1 or count > 128 or back_offset < 0:
        return "count must be 1-128, back_offset >= 0"
    console("bustail on")
    cmd = f"bustail {count}" + (f" {back_offset}" if back_offset else "")
    return console(cmd, timeout_s=6.0)


@mcp.tool()
def apple2_menu_key(key: str) -> str:
    """Send a navigation key to the Appletini config menu (not the
    Apple's keyboard): up|down|left|right|e|toggle|tab|shift-tab|
    pgup|pgdn|space|esc|menu. 'menu' opens/closes the config menu."""
    key = key.strip().lower()
    allowed = {"up", "down", "left", "right", "e", "toggle", "scanlines",
               "tab", "shift-tab", "pgup", "pgdn", "space", "esc", "menu"}
    if key not in allowed:
        return f"key must be one of: {', '.join(sorted(allowed))}"
    return console(f"nav {key}") or f"sent: {key}"


@mcp.tool()
def apple2_console(command: str) -> str:
    """Escape hatch: run any firmware UART console command verbatim and
    return its output (see 'help' for the list). CAUTION: 'reboot' and
    'reset' both restart the Appletini FIRMWARE (not the Apple II);
    'mwr' writes hardware registers."""
    if not command.strip():
        return "empty command"
    return console(command.strip(), timeout_s=8.0)


if __name__ == "__main__":
    mcp.run()
