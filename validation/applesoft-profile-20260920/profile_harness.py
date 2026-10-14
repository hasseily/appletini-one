"""Run the original disk programs through GSSquared's debug client on Windows.

No tokenized program or ROM bytes are replaced. See report for capture boundaries.
"""
from __future__ import annotations
import argparse
import ctypes
import dataclasses
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import sys
import time

ROOT = Path(__file__).resolve().parent
GS = Path(r'C:\Users\hasse\source\repos\gssquared')
sys.path.insert(0, str(GS / 'clients/python/src'))
from gs2debug.client import Client
from gs2debug.types import BP_KIND_EXEC, EXEC_PAUSED, MEM_MAIN, EVT_STOPPED

SOCKET = str(Path(os.environ['TEMP']) / 'gs2-basic-profile.sock')
OUT = ROOT / 'captures'
OUT.mkdir(exist_ok=True)

class WindowsClient(Client):
    def connect(self, path):
        # CPython's Windows build omits AF_UNIX/connect address conversion, but
        # Winsock supports family 1. Reuse all upstream framing and commands.
        class SockaddrUnix(ctypes.Structure):
            _fields_ = [('family', ctypes.c_ushort), ('path', ctypes.c_char * 108)]
        raw = os.fsencode(path)
        if len(raw) >= 108:
            raise ValueError('debug socket path exceeds sockaddr_un')
        address = SockaddrUnix(1, raw)
        sock = socket.socket(1, socket.SOCK_STREAM)
        ws = ctypes.WinDLL('ws2_32', use_last_error=True)
        ws.connect.argtypes = [ctypes.c_size_t, ctypes.c_void_p, ctypes.c_int]
        ws.connect.restype = ctypes.c_int
        if ws.connect(sock.fileno(), ctypes.byref(address), ctypes.sizeof(address)):
            error = ws.WSAGetLastError()
            sock.close()
            raise OSError(error, 'Winsock AF_UNIX connect failed')
        self._sock = sock
        self._next_seq = 1
        self._handshaked = False
        self.stops = []
        self.on_event(self._save_event)

    def _save_event(self, event_id, seq, data):
        if event_id == EVT_STOPPED:
            reason, bp_id, pc, eaddr, value = struct.unpack_from('<IIIII', data)
            self.stops.append(dict(reason=reason, bp_id=bp_id, pc=pc,
                                   eaddr=eaddr, value=value))

    def request(self, type_word, payload=b'', *, timeout=20.0):
        return super().request(type_word, payload, timeout=timeout)

def connect():
    c = WindowsClient()
    c.connect(SOCKET)
    c.hello()
    return c

def registers(c):
    data = c.get_regs()
    cycle = struct.unpack_from('<Q', data)[0]
    pc, a, x, y, sp = struct.unpack_from('<HHHHH', data, 16)
    return dict(pc=pc, cycle=cycle, a=a, x=x, y=y, sp=sp, raw=data.hex())

def snapshot(c):
    zp = c.read_mem(MEM_MAIN, 0, 256)
    return dict(registers=registers(c), zero_page_hex=zp.hex(),
                basic_line=int.from_bytes(zp[0x75:0x77], 'little'),
                text_pointer=int.from_bytes(zp[0xb8:0xba], 'little'),
                text=c.video_text(mode=1, page=1).as_lines())

def wait_pause(c, timeout=60, label='run', budget_run=None):
    start = time.monotonic()
    next_progress = start + 15
    while time.monotonic() - start < timeout:
        # A breakpoint leaves STEP_INTO with zero pending instructions (mode 1).
        # Explicit PAUSE uses mode 2. Both are stopped here.
        if c.get_status().execution_mode != 0:
            return c.stops[-1] if c.stops else dict(pc=registers(c)['pc'])
        if budget_run and (OUT / (budget_run + '.json')).exists():
            c.pause()
            continue
        if time.monotonic() >= next_progress:
            r = registers(c)
            print(json.dumps(dict(progress=label, seconds=round(time.monotonic()-start),
                                  pc=hex(r['pc']), cycles=r['cycle'])), flush=True)
            next_progress += 15
        time.sleep(.05)
    c.pause()
    raise TimeoutError(f'{label} exceeded {timeout}s; snapshot={snapshot(c)}')

def paused(c):
    if c.get_status().execution_mode == 0:
        c.pause()
        wait_pause(c, 10, 'pause')

def breakpoint(c, address):
    return c.bp_set(kind=BP_KIND_EXEC, address=address)

def control(command, runid, limit=0):
    sequence_path = ROOT / 'control-sequence.txt'
    seq = int(sequence_path.read_text())+1 if sequence_path.exists() else 1
    sequence_path.write_text(str(seq))
    temporary = OUT / 'control.next'
    temporary.write_text(f'{seq} {command} {runid} {limit}\n', encoding='ascii')
    temporary.replace(OUT / 'control.txt')
    deadline = time.monotonic()+10
    while time.monotonic() < deadline:
        try:
            ack = json.loads((OUT/'ack.json').read_text())
            if ack['seq'] == seq:
                if ack['state'] == 'error':
                    raise RuntimeError(ack)
                return ack
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(.02)
    raise TimeoutError(f'Profiler did not acknowledge {seq} {command}')

def command_to_prompt(c, command, timeout=30):
    c.bp_clear_all()
    if registers(c)['pc'] == 0xd43f:
        c.step_into(1)
        time.sleep(.05)
    breakpoint(c, 0xd43f)
    c.paste_text(command+'\r')
    c.continue_()
    event = wait_pause(c, timeout, command)
    if registers(c)['pc'] != 0xd43f:
        raise RuntimeError((command, event, snapshot(c)))
    return event

def prepare(c, name):
    paused(c)
    # Re-enter command mode after a bounded/interactive previous run.
    if registers(c)['pc'] != 0xd43f:
        c.bp_clear_all()
        breakpoint(c, 0xd43f)
        c.reset(cold_start=False)
        c.continue_()
        wait_pause(c, 30, 'warm reset')
    command_to_prompt(c, 'TEXT:HOME')
    command_to_prompt(c, 'LOAD '+name)
    catalog = json.loads((ROOT/'inventory/inventory.json').read_text())
    entry = next(x for x in catalog['files'] if x['name'] == name)
    token_path = ROOT/'inventory'/entry['listing'].replace('.bas.txt', '.tokens.bin')
    expected = token_path.read_bytes()
    actual = c.read_mem(MEM_MAIN, 0x801, len(expected))
    if actual != expected:
        raise RuntimeError(f'Loaded token bytes mismatch for {name}: {actual[:20].hex()} vs {expected[:20].hex()}')
    c.bp_clear_all()
    breakpoint(c, 0xd912)
    c.paste_text('RUN\r')
    c.continue_()
    wait_pause(c, 30, 'RUN '+name)
    assert registers(c)['pc'] == 0xd912
    # Fix all five RND bytes, including the byte omitted by ROM cold start.
    c.write_mem(MEM_MAIN, 0xc9, bytes.fromhex('804fc75258'))
    return entry

def capture(c, name, limit=0, keyboard_stop=False, runid=None, timeout=1200):
    entry = prepare(c, name)
    runid = runid or entry['listing'].replace('.bas.txt', '').replace('.', '_')
    return capture_current(c, entry, runid, limit, keyboard_stop, timeout,
                           'RUN entry after DOS LOAD; fixed five-byte RND seed')

def capture_current(c, entry, runid, limit=0, keyboard_stop=False, timeout=1200,
                    boundary_note='Current stopped PC'):
    name = entry['name']
    c.bp_clear_all()
    breakpoint(c, 0xd43f)
    if keyboard_stop:
        breakpoint(c, 0xfd0c)
    start = snapshot(c)
    control('START', runid, limit)
    c.continue_()
    event = wait_pause(c, timeout, name, runid if limit else None)
    control('STOP', runid)
    end = snapshot(c)
    image = c.read_mem(MEM_MAIN, 0x2000, 0x4000)
    (OUT/(runid+'.graphics.bin')).write_bytes(image)
    result = dict(program=name, runid=runid, limit=limit, keyboard_stop=keyboard_stop,
                  boundary_note=boundary_note,
                  tokenized_sha256=entry['tokenized_sha256'], start=start, end=end,
                  stop_event=event, graphics_sha256=hashlib.sha256(image).hexdigest(),
                  finished_at_prompt=end['registers']['pc']==0xd43f)
    (OUT/(runid+'.run.json')).write_text(json.dumps(result, indent=2))
    meta = json.loads((OUT/(runid+'.json')).read_text())
    assert meta['histogram_instructions'] == meta['instructions']
    assert meta['histogram_cycles'] == meta['instruction_cycles']
    assert meta['accounted_cycles'] == meta['guest_cycle_span']
    assert meta['clock_mode_at_start'] == 0, meta
    print(json.dumps(dict(completed=name, runid=runid, cycles=meta['accounted_cycles'],
                          instructions=meta['instructions'], wall_s=meta['capture_wall_seconds'],
                          endpoint=hex(end['registers']['pc']),
                          reason=meta['reason'])), flush=True)
    return result

def main():
    p = argparse.ArgumentParser()
    p.add_argument('action', choices=['status', 'max', 'boot-prompt', 'run', 'quit'])
    p.add_argument('--name')
    p.add_argument('--limit', type=int, default=0)
    p.add_argument('--keyboard-stop', action='store_true')
    p.add_argument('--runid')
    args = p.parse_args()
    c = connect()
    try:
        if args.action == 'status':
            print(json.dumps(snapshot(c), indent=2))
        elif args.action == 'max':
            c.tap_key(66, mod=1)
            print('Shift-F9 sent: maximum/free-run clock')
        elif args.action == 'boot-prompt':
            paused(c)
            command_to_prompt(c, '')
            print(json.dumps(snapshot(c), indent=2))
        elif args.action == 'run':
            capture(c, args.name, args.limit, args.keyboard_stop, args.runid)
        elif args.action == 'quit':
            c.quit()
    finally:
        c.close()

if __name__ == '__main__':
    main()
