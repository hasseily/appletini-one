#!/usr/bin/env python3
"""Compare ROM/RAM copies and HGR/ordinary buffers on stock ONE//e firmware."""
import ftplib
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import time

import serial
from smartport_timing_board import serial_command, screen, get, put, stock_fingerprints, STOCK_HASH

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/smartport-copy-board'
sys.path.insert(0, str(ROOT / 'software/net_transport_checkpoint'))
from prodos_disk import read_root_files


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='backslashreplace')
    OUT.mkdir(parents=True, exist_ok=True)
    backup = OUT / 'original-settings.bin'
    if backup.exists():
        raise RuntimeError('Restore copy already exists; refusing to overwrite it')
    report = {'firmware_rebuilt': False, 'firmware_written': False,
              'fpga_written': False, 'scope': 'physical ONE//e, stock F1.3.3b3'}
    log = []
    original = None
    owned = False
    with serial.Serial('COM13', 921600, timeout=.05) as uart:
        def command(text, timeout=1):
            response = serial_command(uart, text, timeout=timeout)
            log.append(response)
            (OUT / 'uart.txt').write_text(''.join(log), encoding='utf-8')
            return response

        def transfer(operation):
            command('ftp on')
            try:
                with ftplib.FTP() as ftp:
                    ftp.connect('10.17.1.18', timeout=30)
                    ftp.login('anonymous', 'appletini-checkpoint')
                    return operation(ftp)
            finally:
                command('ftp off')

        def boot():
            command('reboot', 15)
            time.sleep(12)

        try:
            report['before'] = command('status') + command('vtw status')
            if 'Firmware F1.3.3b3' not in report['before'] or 'session=ONE//e' not in report['before']:
                raise RuntimeError('Wrong firmware or machine mode')
            for name, code in stock_fingerprints().items():
                words = command(f'mrd 0x{code["address"]:08X} 8')
                for index, word in enumerate(code['words']):
                    address = code['address'] + 4*index
                    match = re.search(rf'0x{address:08X}:\s*0x([0-9A-F]{{8}})', words, re.I)
                    assert match and int(match[1], 16) == word, name
            assert re.search(r'0x0015124C:\s*0xE24220EB', command('mrd 0x0015124C'), re.I)

            def stage(ftp):
                nonlocal original, owned
                assert digest(get(ftp, '/FIRMWARE.OK')) == STOCK_HASH
                original = get(ftp, '/appletini_cfg.txt')
                backup.write_bytes(original)
                report['original_settings_sha256'] = digest(original)
                ftp.mkd('/SPCOPY')  # Refuse existing, unowned directories.
                owned = True
                put(ftp, '/SPCOPY/TEST.PO', (OUT / 'TEST.PO').read_bytes())
                values = {'smartport.disk.1.path': '"0:/SPCOPY/TEST.PO"',
                          'smartport.disk.1.enabled': 'ON', 'vtw.speed.mode': '2'}
                profile = original.decode('utf-8')
                for key, value in values.items():
                    profile, count = re.subn(r'(?m)^'+re.escape(key)+r'=[^\r\n]*', key+'='+value, profile)
                    assert count == 1, key
                put(ftp, '/appletini_cfg.txt', profile.encode('utf-8'))
            transfer(stage)
            print('Staged owned benchmark volume; restarting stock firmware.', flush=True)
            boot()
            initial = screen(command('vtw dump 400 400', 2))
            assert 'STOCK SMARTPORT COPY TIMING' in initial, initial
            assert 'E10002' in command('mrd 0x4000017C')
            for speed, key, completed in (('1mhz', '1', 1), ('turbo', 'T', 2)):
                command('vtw speed '+speed)
                status = command('vtw status')
                expected = 'speed=1 MHz' if key == '1' else 'speed=TURBO'
                for value in (expected, 'c074=0', 'session=ONE//e', 'slowdown mask=$008 cycles=256'):
                    assert value in status, value
                report['profile_'+key] = status
                command(f'mwr 0x40000170 0x{ord(key):02X}')
                for _ in range(15):
                    time.sleep(2)
                    display = screen(command('vtw dump 400 400', 2))
                    if display.count('SAVED PASS') >= completed:
                        break
                    if 'SAVED FAIL' in display or 'SAVE FAILED' in display:
                        raise RuntimeError(display)
                else:
                    raise RuntimeError('Benchmark timeout: '+display)
                report['screen_'+key] = display
                print(display, flush=True)

            def collect(ftp):
                image = get(ftp, '/SPCOPY/TEST.PO')
                (OUT / 'RESULT.PO').write_bytes(image)
                files = read_root_files(image)
                report['results'] = {}
                for name in ('NATIVE.BIN', 'TURBO.BIN'):
                    data = files[name]
                    (OUT / name).write_bytes(data)
                    assert len(data) == 64 and data[:4] == b'SCB1'
                    assert data[5:8] == bytes(3), data[:16].hex()
                    rows = []
                    for at in range(16, 64, 12):
                        dest, raw, count, low, high, total = struct.unpack_from('<HBBHHI', data, at)
                        assert count == 32
                        rows.append({'destination': hex(dest), 'path': 'RAM' if raw else 'ROM',
                                     'count': count, 'min_ticks': low, 'max_ticks': high,
                                     'mean_ticks': total/count})
                    report['results'][name] = rows
            transfer(collect)
            report['passed'] = True
        except Exception as exc:
            report['error'] = str(exc)
            raise
        finally:
            if original is not None:
                def restore(ftp):
                    put(ftp, '/appletini_cfg.txt', original)
                    assert digest(get(ftp, '/FIRMWARE.OK')) == STOCK_HASH
                    if owned:
                        ftp.delete('/SPCOPY/TEST.PO')
                        ftp.rmd('/SPCOPY')
                transfer(restore)
                boot()
                report['restored'] = {'settings_sha256': digest(original),
                                      'runtime': command('status')+command('vtw status'),
                                      'media': command('sd status'), 'ftp': command('ftp status')}
                assert 'SP1 open=1' in report['restored']['media']
                assert 'ftp: off' in report['restored']['ftp']
            (OUT / 'board-results.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report.get('results', report), indent=2), flush=True)


if __name__ == '__main__':
    main()
