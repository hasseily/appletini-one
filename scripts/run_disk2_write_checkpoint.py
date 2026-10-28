#!/usr/bin/env python3
"""Check Disk II writes on the exact stock firmware, using owned test images."""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import sys
import time

import serial
from https_probe_host import serial_command
from run_net_transport_checkpoint import screen

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/disk2-stock-checkpoint'
sys.path.insert(0, str(ROOT / 'software/net_transport_checkpoint'))
from prodos_disk import read_root_files, validate_image

STOCK_HASH = 'cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2'
# speed, Disable DiskII acceleration, slot-6 slowdown mask
CASES = {1: ('1mhz', False, 0), 2: ('turbo', False, 0),
         3: ('turbo', True, 0), 4: ('turbo', False, 0x20),
         5: ('turbo', True, 0x20)}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def get(ftp, path):
    data = io.BytesIO()
    ftp.retrbinary('RETR ' + path, data.write)
    return data.getvalue()


def put(ftp, path, data):
    ftp.storbinary('STOR ' + path, io.BytesIO(data))
    if get(ftp, path) != data:
        raise RuntimeError('SD upload readback mismatch: ' + path)


def settings(original, disabled, mask, disk_path='"0:/D2CHECK/DISK.PO"'):
    values = {'smartport.disk.1.path': '"0:/D2CHECK/HOST.PO"',
              'smartport.disk.1.enabled': 'ON',
              'disk2.drive.1.path': disk_path,
              'disk2.drive.2.path': '""', 'disk2.slot6.enabled': 'ON',
              'vtw.disk2.acceleration.disabled': 'ON' if disabled else 'OFF',
              'vtw.slowdown.mask': hex(mask), 'vtw.slowdown.cycles': '256',
              'vtw.speed.mode': '2'}  # Boot natively; change speed at the test menu.
    text = original.decode('utf-8')
    for key, value in values.items():
        text, count = re.subn(r'(?m)^' + re.escape(key) + r'=[^\r\n]*',
                             key + '=' + value, text)
        if count != 1:
            raise RuntimeError('Expected exactly one saved setting: ' + key)
    return text.encode('utf-8')


def stock_fingerprints():
    """Read code words from two functions in the archived stock ARM ELF."""
    data = (ROOT / 'firmwares/firmware-F1.3.3b3/inputs/frontend.elf').read_bytes()
    assert data[:7] == b'\x7fELF\x01\x01\x01'
    offset = struct.unpack_from('<I', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 46)
    assert stride == 40
    sections = [struct.unpack_from('<10I', data, offset + stride * n) for n in range(count)]
    wanted = {'disk2_service_poll', 'smartport_service_poll'}
    result = {}
    for section in sections:
        if section[1] != 2:  # SHT_SYMTAB
            continue
        strings = sections[section[6]]
        names = data[strings[4]:strings[4] + strings[5]]
        for at in range(section[4], section[4] + section[5], section[9]):
            name_at, address, size, _, _, index = struct.unpack_from('<IIIBBH', data, at)
            name = names[name_at:names.find(b'\0', name_at)].decode('ascii')
            if name not in wanted:
                continue
            assert size >= 32 and address % 4 == 0
            code = sections[index]
            start = code[4] + address - code[3]
            result[name] = {'address': address, 'words': list(struct.unpack_from('<8I', data, start))}
    assert set(result) == wanted
    return result


def main():
    global OUT
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'case', 'restore'])
    parser.add_argument('--case', type=int, choices=CASES)
    parser.add_argument('--output-dir', type=Path, default=OUT)
    parser.add_argument('--nib', action='store_true', help='preserve the raw standard-media stream for write forensics')
    parser.add_argument('--decoder-patched', action='store_true', help='require the single-instruction stock decoder experiment')
    args = parser.parse_args()
    OUT = args.output_dir.resolve()
    OUT.mkdir(parents=True, exist_ok=True)
    log = []
    label = 'case-' + str(args.case) if args.action == 'case' else args.action
    original_path = OUT / 'original-settings.bin'
    report = {'action': args.action, 'firmware_rebuilt': False,
              'firmware_written': False, 'golden_written': False,
              'physical_scope': 'physical Appletini in ONE//e mode'}
    with serial.Serial('COM13', 921600, timeout=.05) as uart:
        def command(text, timeout=.5):
            response = serial_command(uart, text, timeout=timeout)
            log.append(response)
            return response

        def transfer(operation):
            command('ftp on', 1)
            try:
                with ftplib.FTP() as ftp:
                    ftp.connect('10.17.1.18', timeout=30)
                    ftp.login('anonymous', 'appletini-checkpoint')
                    return operation(ftp)
            finally:
                command('ftp off', 1)

        try:
            if args.action == 'prepare':
                if original_path.exists():
                    raise RuntimeError('Original settings already saved; do not overwrite the restore copy')

                def prepare(ftp):
                    receipt = get(ftp, '/FIRMWARE.OK')
                    if digest(receipt) != STOCK_HASH:
                        raise RuntimeError('SD stock updater receipt differs from exact F1.3.3b3')
                    original = get(ftp, '/appletini_cfg.txt')
                    original_path.write_bytes(original)
                    try:
                        ftp.cwd('/D2CHECK')
                    except ftplib.error_perm as exc:
                        if not str(exc).startswith('550'):
                            raise
                        ftp.mkd('/D2CHECK')
                    else:
                        raise RuntimeError('D2CHECK already exists; refusing to overwrite unknown media')
                    report['stock_receipt_sha256'] = digest(receipt)
                    report['original_settings_sha256'] = digest(original)
                transfer(prepare)
                # FTP on stock firmware may leave media unmounted; reboot before use.
                command('reboot', 15)
            elif args.action == 'case':
                if args.case is None:
                    raise RuntimeError('--case is required')
                speed, disabled, mask = CASES[args.case]
                original = original_path.read_bytes()
                remote_disk = '/D2CHECK/DISK.NIB' if args.nib else '/D2CHECK/DISK.PO'
                profile = settings(original, disabled, mask, '"0:' + remote_disk + '"')

                def stage(ftp):
                    put(ftp, '/D2CHECK/HOST.PO', (OUT / 'D2HOST.po').read_bytes())
                    put(ftp, remote_disk, (OUT / ('D2TEST.nib' if args.nib else 'D2TEST.po')).read_bytes())
                    put(ftp, '/appletini_cfg.txt', profile)
                transfer(stage)
                command('reboot', 15)
                time.sleep(12)
                initial = screen(command('vtw dump 400 400', 2))
                if 'STOCK DISK II WRITE CHECK' not in initial:
                    raise RuntimeError('Fresh stock write diagnostic menu not found:\n' + initial)
                if 'E10002' not in command('mrd 0x4000017C'):
                    raise RuntimeError('ONE//e bridge not active')
                command('vtw speed ' + speed)
                status = command('vtw status', 1)
                expected = 'speed=1 MHz' if speed == '1mhz' else 'speed=TURBO'
                for value in (expected, 'c074=0', 'session=ONE//e',
                              'd2accel=off' if disabled else 'd2accel=on',
                              f'slowdown mask=${mask | 8:03X} cycles=256'):
                    if value not in status:
                        raise RuntimeError('Unexpected live profile: ' + value)
                report.update(case=args.case, speed=speed, disk2_acceleration=not disabled,
                              slowdown_mask=mask | 8, slowdown_ticks=256, live_before=status,
                              profile_sha256=digest(profile))
                fingerprints = stock_fingerprints()
                for name, code in fingerprints.items():
                    words = command(f'mrd 0x{code["address"]:08X} 8', 1)
                    for index, expected_word in enumerate(code['words']):
                        address = code['address'] + index * 4
                        match = re.search(rf'0x{address:08X}:\s*0x([0-9A-F]{{8}})', words, re.I)
                        if not match or int(match[1], 16) != expected_word:
                            raise RuntimeError('Live ARM code differs from stock: ' + name)
                report['stock_live_code_fingerprints'] = fingerprints
                if args.decoder_patched:
                    patch = command('mrd 0x0015124C', 1)
                    if not re.search(r'0x0015124C:\s*0xE3A02000', patch, re.I):
                        raise RuntimeError('Expected decoder instruction patch is not running')
                    report['firmware_variant'] = 'stock F1.3.3b3 plus one decoder instruction correction'
                command(f'mwr 0x40000170 0x{ord(str(args.case)):02X}')
                started = time.monotonic()
                while time.monotonic() - started < 300:
                    time.sleep(5)
                    display = screen(command('vtw dump 400 400', 2))
                    if 'DONE; PRESS A KEY' in display:
                        report['screen'] = display
                        break
                    print(f'case {args.case}: running ({int(time.monotonic() - started)} s)', flush=True)
                else:
                    report['screen'] = display
                    raise RuntimeError('Disk test did not finish within 300 seconds')
                report['elapsed_host_seconds'] = time.monotonic() - started
                report['live_after'] = command('vtw status', 1)
                report['disk2_after'] = command('disk2 status', 1)
                report['smartport_after'] = command('sd status', 1)

                def collect(ftp):
                    files_by_volume = {}
                    for name, path in (('host', '/D2CHECK/HOST.PO'), ('disk2', remote_disk)):
                        data = get(ftp, path)
                        if name == 'disk2' and args.nib:
                            (OUT / f'{label}-disk2.nib').write_bytes(data)
                            from test_disk2_standard import denibblize_track
                            assert len(data) == 35 * 6656
                            data = b''.join(denibblize_track(data[t*6656:(t+1)*6656], True, t)
                                            for t in range(35))
                        (OUT / f'{label}-{name}.po').write_bytes(data)
                        info = {'sha256': digest(data)}
                        try:
                            validate_image(data)
                            files = read_root_files(data)
                            files_by_volume[name] = files
                            info.update(valid_volume=True, scratch_sha256=digest(files['SCRATCH']))
                        except (ValueError, KeyError) as exc:
                            info.update(valid_volume=False, error=str(exc))
                        report[name] = info
                    raw = files_by_volume['host'][f'CASE{args.case}.BIN']
                    (OUT / f'{label}.bin').write_bytes(raw)
                    assert len(raw) == 32 and raw[:6] == b'D2WC\x01' + str(args.case).encode()
                    def u16(at): return int.from_bytes(raw[at:at + 2], 'little')
                    report['result'] = {'pass': raw[6] == 1, 'target': raw[7], 'stage': raw[8],
                                        'errno': raw[9], 'oserror': raw[10], 'round': u16(12),
                                        'smartport_rounds': u16(14), 'disk2_rounds': u16(16),
                                        'mismatch_offset': u16(18), 'got': raw[20], 'expected': raw[21]}
                    assert report['result']['smartport_rounds'] == 8
                    for name, last_round in (('host', 7), ('disk2', 31)):
                        if name == 'disk2' and not report['result']['pass']:
                            continue
                        expected_bytes = bytes((at * 17 + last_round * 29 + 0x53) & 255 for at in range(512))
                        assert files_by_volume[name]['SCRATCH'] == expected_bytes
                        report[name]['persisted_pattern_verified'] = True
                transfer(collect)
            else:
                original = original_path.read_bytes()

                def restore(ftp):
                    put(ftp, '/appletini_cfg.txt', original)
                    for path in ('/D2CHECK/HOST.PO', '/D2CHECK/DISK.PO'):
                        if path.endswith('DISK.PO') and args.nib:
                            path = '/D2CHECK/DISK.NIB'
                        ftp.delete(path)
                    ftp.rmd('/D2CHECK')
                transfer(restore)
                report['settings_readback_sha256'] = digest(original)
                command('reboot', 15)
                time.sleep(12)
                report['runtime'] = command('status', 1) + command('vtw status', 1)
                report['media'] = command('sd status', 1) + command('disk2 status', 1)
                report['ftp'] = command('ftp status', 1)
                if 'SP1 open=1' not in report['media'] or 'ftp: off' not in report['ftp']:
                    raise RuntimeError('Original media did not reopen after restoration')
            report['completed'] = True
        except Exception as exc:
            report['error'] = str(exc)
            raise
        finally:
            (OUT / (label + '.json')).write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
            (OUT / (label + '.txt')).write_text(''.join(log), encoding='utf-8')
    print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
