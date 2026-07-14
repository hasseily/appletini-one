#!/usr/bin/env python3
"""Upload FIRMWARE.BIN through the golden serial monitor.

The golden monitor accepts:

    rx <size> <crc32>

and then receives the file with XMODEM-CRC. This script provides the host
side so reset capture, command entry, CRC calculation, and XMODEM transfer
all happen in one process.
"""

from __future__ import annotations

import argparse
import sys
import time
import zlib
from pathlib import Path

try:
    import serial
    import serial.tools.list_ports
except ImportError:  # pragma: no cover - handled at runtime.
    serial = None


XMODEM_STX = 0x02
XMODEM_EOT = 0x04
XMODEM_ACK = 0x06
XMODEM_NAK = 0x15
XMODEM_CAN = 0x18
XMODEM_CRC_REQ = ord("C")
XMODEM_BLOCK_BYTES = 1024
XMODEM_PAD = 0x1A


def crc16_ccitt(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def print_target(data: bytes, quiet: bool) -> None:
    if quiet or not data:
        return
    text = data.decode("utf-8", errors="replace")
    encoding = sys.stdout.encoding or "utf-8"
    safe_text = text.encode(encoding, errors="replace").decode(encoding, errors="replace")
    sys.stdout.write(safe_text)
    sys.stdout.flush()


def read_some(ser: "serial.Serial") -> bytes:
    waiting = ser.in_waiting
    if waiting:
        return ser.read(waiting)
    return ser.read(1)


def recover_serial(ser: "serial.Serial", quiet: bool) -> bool:
    if not quiet:
        print("[host] serial transient error; reopening port")
    try:
        if ser.is_open:
            ser.close()
    except serial.SerialException:
        pass

    time.sleep(0.25)
    try:
        ser.open()
        ser.reset_input_buffer()
        return True
    except serial.SerialException as exc:
        if not quiet:
            print(f"[host] reopen failed: {exc}")
        return False


def write_bytes(ser: "serial.Serial", data: bytes, quiet: bool) -> bool:
    try:
        ser.write(data)
        ser.flush()
        return True
    except serial.SerialException:
        if not recover_serial(ser, quiet):
            return False
    try:
        ser.write(data)
        ser.flush()
        return True
    except serial.SerialException as exc:
        if not quiet:
            print(f"[host] write failed after reopen: {exc}")
        return False


def list_ports() -> int:
    if serial is None:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 2
    ports = list(serial.tools.list_ports.comports())
    if not ports:
        print("No serial ports found.")
        return 0
    for port in ports:
        print(f"{port.device}: {port.description}")
    return 0


def wait_for_monitor(ser: "serial.Serial", args: argparse.Namespace) -> None:
    buf = bytearray()
    deadline = time.monotonic() + args.catch_timeout
    next_probe = time.monotonic()

    ser.reset_input_buffer()
    if args.reboot_golden:
        write_bytes(ser, (args.reboot_command + "\r?\r").encode("ascii"), args.quiet)
    else:
        write_bytes(ser, b"?\r", args.quiet)

    while time.monotonic() < deadline:
        try:
            data = read_some(ser)
        except serial.SerialException:
            recover_serial(ser, args.quiet)
            data = b""
        if data:
            print_target(data, args.quiet)
            buf += data
            if len(buf) > 4096:
                del buf[: len(buf) - 4096]
            if b"[SER] cmd>" in buf:
                return

        now = time.monotonic()
        if now >= next_probe:
            write_bytes(ser, b"?\r", args.quiet)
            next_probe = now + args.probe_interval

    raise TimeoutError("golden monitor prompt was not seen")


def wait_for_xmodem_request(ser: "serial.Serial", timeout_s: float, quiet: bool) -> None:
    deadline = time.monotonic() + timeout_s
    buf = bytearray()
    protocol_seen = False
    marker = b"Protocol bytes will follow"

    while time.monotonic() < deadline:
        try:
            data = read_some(ser)
        except serial.SerialException:
            recover_serial(ser, quiet)
            continue
        if not data:
            continue
        print_target(data, quiet)
        buf += data
        if len(buf) > 4096:
            del buf[: len(buf) - 4096]

        if not protocol_seen:
            pos = bytes(buf).find(marker)
            if pos >= 0:
                del buf[: pos + len(marker)]
                protocol_seen = True

        if protocol_seen and XMODEM_CRC_REQ in buf:
            return

    raise TimeoutError("golden monitor did not request XMODEM-CRC")


def wait_for_reply(ser: "serial.Serial", timeout_s: float, quiet: bool) -> int:
    deadline = time.monotonic() + timeout_s
    text = bytearray()

    while time.monotonic() < deadline:
        try:
            data = read_some(ser)
        except serial.SerialException:
            recover_serial(ser, quiet)
            continue
        if not data:
            continue
        for byte in data:
            if byte in (XMODEM_ACK, XMODEM_NAK, XMODEM_CAN):
                print_target(bytes(text), quiet)
                return byte
            if byte not in (XMODEM_CRC_REQ,):
                text.append(byte)

    print_target(bytes(text), quiet)
    raise TimeoutError("timeout waiting for XMODEM reply")


def send_xmodem_1k(ser: "serial.Serial",
                   payload: bytes,
                   retries: int,
                   quiet: bool,
                   cancel_after_blocks: int | None = None) -> bool:
    total_blocks = (len(payload) + XMODEM_BLOCK_BYTES - 1) // XMODEM_BLOCK_BYTES
    if total_blocks == 0:
        total_blocks = 1

    block_no = 1
    offset = 0
    last_report = 0

    while block_no <= total_blocks:
        chunk = payload[offset : offset + XMODEM_BLOCK_BYTES]
        if len(chunk) < XMODEM_BLOCK_BYTES:
            chunk += bytes([XMODEM_PAD]) * (XMODEM_BLOCK_BYTES - len(chunk))

        crc = crc16_ccitt(chunk)
        packet = bytes(
            [
                XMODEM_STX,
                block_no & 0xFF,
                0xFF - (block_no & 0xFF),
            ]
        ) + chunk + bytes([(crc >> 8) & 0xFF, crc & 0xFF])

        attempt = 0
        while True:
            if not write_bytes(ser, packet, quiet):
                raise RuntimeError("serial write failed during XMODEM transfer")
            reply = wait_for_reply(ser, 10.0, quiet)
            if reply == XMODEM_ACK:
                break
            if reply == XMODEM_CAN:
                raise RuntimeError("target cancelled XMODEM transfer")
            attempt += 1
            if attempt > retries:
                raise RuntimeError(f"block {block_no} was not accepted")

        if cancel_after_blocks is not None and block_no >= cancel_after_blocks:
            if not write_bytes(ser, bytes([XMODEM_CAN, XMODEM_CAN, XMODEM_CAN]), quiet):
                raise RuntimeError("serial write failed during XMODEM cancel")
            return False

        offset += XMODEM_BLOCK_BYTES
        if block_no == total_blocks or block_no - last_report >= 64:
            sent = min(offset, len(payload))
            print(f"[host] sent {sent}/{len(payload)} bytes")
            last_report = block_no
        block_no += 1

    for _ in range(retries + 1):
        if not write_bytes(ser, bytes([XMODEM_EOT]), quiet):
            raise RuntimeError("serial write failed during XMODEM EOT")
        reply = wait_for_reply(ser, 10.0, quiet)
        if reply == XMODEM_ACK:
            return True
        if reply == XMODEM_CAN:
            raise RuntimeError("target cancelled after EOT")

    raise RuntimeError("target did not ACK EOT")


def read_after_update(ser: "serial.Serial", timeout_s: float, quiet: bool) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            data = read_some(ser)
        except serial.SerialException:
            recover_serial(ser, quiet)
            continue
        if data:
            print_target(data, quiet)


def parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser(description="Upload firmware through the golden serial monitor")
    ap.add_argument("firmware", nargs="?", default="FIRMWARE.BIN", help="firmware image to send")
    ap.add_argument("--port", default="COM3", help="golden firmware-update UART port")
    ap.add_argument("--baud", type=int, default=921600, help="UART baud rate")
    ap.add_argument("--list-ports", action="store_true", help="list serial ports and exit")
    ap.add_argument("--reboot-golden", action="store_true", help="send :reboot before catching golden")
    ap.add_argument("--reboot-command", default=":reboot", help="firmware command used by --reboot-golden")
    ap.add_argument("--catch-timeout", type=float, default=6.0, help="seconds to wait for golden prompt")
    ap.add_argument("--probe-interval", type=float, default=0.02, help="seconds between ? probes")
    ap.add_argument("--command-timeout", type=float, default=5.0, help="seconds to wait for rx handshake")
    ap.add_argument("--xmodem-retries", type=int, default=10, help="retries per XMODEM block")
    ap.add_argument("--wait-after", type=float, default=25.0, help="seconds to print target output after upload")
    ap.add_argument("--no-wait-after", action="store_true", help="exit immediately after XMODEM completes")
    ap.add_argument("--expected-crc32", help="override CRC32 argument sent to golden")
    ap.add_argument("--bad-crc", action="store_true", help="send an intentionally wrong CRC32")
    ap.add_argument("--cancel-after-blocks", type=int, help="send CAN after this many accepted XMODEM blocks")
    ap.add_argument("--quiet", action="store_true", help="suppress target serial output")
    return ap.parse_args()


def main() -> int:
    args = parse_args()

    if args.list_ports:
        return list_ports()
    if serial is None:
        print("pyserial is required: python -m pip install pyserial", file=sys.stderr)
        return 2

    firmware_path = Path(args.firmware)
    payload = firmware_path.read_bytes()
    crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    expected_crc32 = crc32
    if args.expected_crc32 is not None:
        expected_crc32 = int(args.expected_crc32, 0) & 0xFFFFFFFF
    if args.bad_crc:
        expected_crc32 = (crc32 ^ 0x00000001) & 0xFFFFFFFF

    print(f"[host] firmware={firmware_path} size={len(payload)} crc32=0x{crc32:08X}")
    if expected_crc32 != crc32:
        print(f"[host] sending expected crc32=0x{expected_crc32:08X}")
    print(f"[host] opening {args.port} @ {args.baud}")

    with serial.Serial(
        args.port,
        args.baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.05,
        write_timeout=2.0,
        xonxoff=False,
        rtscts=False,
        dsrdtr=False,
    ) as ser:
        wait_for_monitor(ser, args)
        command = f"rx {len(payload)} 0x{expected_crc32:08X}\r".encode("ascii")
        if not write_bytes(ser, command, args.quiet):
            raise RuntimeError("serial write failed while sending rx command")
        wait_for_xmodem_request(ser, args.command_timeout, args.quiet)
        completed = send_xmodem_1k(ser,
                                   payload,
                                   args.xmodem_retries,
                                   args.quiet,
                                   args.cancel_after_blocks)
        if completed:
            print("[host] XMODEM transfer complete")
        else:
            print("[host] XMODEM transfer cancelled by host")
        if not args.no_wait_after:
            read_after_update(ser, args.wait_after, args.quiet)

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[host] interrupted", file=sys.stderr)
        raise SystemExit(130)
