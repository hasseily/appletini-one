#!/usr/bin/env python3
"""Run the actual ARM decoder on a board-captured TURBO address splice."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from test_onee_input_service import find_native_c_compiler
from test_disk2_standard import nibblize_track, make_sector_track, find_prologues

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/disk2-address-splice-test'


def function(source, name):
    match = re.search(r'static [^;{}]+\b' + name + r'\([^;]*?\)\s*\{', source)
    assert match, name
    end = match.end(); depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    cc = find_native_c_compiler()
    if cc is None:
        raise RuntimeError('Native C compiler required')
    if os.name == 'nt':
        dll_path = os.add_dll_directory(str(cc.parent))
    source = (ROOT / 'ps_sources/frontend/disk2_service.c').read_text()
    table = re.search(r'static const uint8_t gcr_6and2\[64\] = \{.*?\};', source, re.S)[0]
    shared = '#include <stdint.h>\n#include <string.h>\n#define DISK2_DSK_SECTORS_PER_TRACK 16U\n#define DISK2_SECTOR_BYTES 256U\n'
    shared += table + '\n'
    for name in ('decode44_pair', 'sector_image_file_sector', 'gcr_decode_6and2',
                 'decode_6and2_sector', 'denibblize_sector_track'):
        shared += function(source, name) + '\n'
    shared += 'int decode(const uint8_t *n, uint32_t len, uint8_t *out, uint8_t po, uint8_t t) { return denibblize_sector_track(n,len,out,po,t); }\n'

    def compile_decoder(name, text):
        path = OUT / (name + '.c'); path.write_text(text, encoding='utf-8')
        lib = OUT / (name + ('.dll' if os.name == 'nt' else '.so'))
        subprocess.run([str(cc), '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-shared', str(path), '-o', str(lib)], check=True)
        dll = ctypes.CDLL(str(lib))
        dll.decode.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p,
                               ctypes.c_uint8, ctypes.c_uint8]
        def decode(raw, po, track):
            output = (ctypes.c_ubyte * 4096)()
            assert dll.decode(raw, len(raw), output, po, track) == 0
            return bytes(output)
        return decode

    fixed = compile_decoder('fixed', shared)
    old = shared.replace('encoded[9] != 0xAAU)',
                         'encoded[9] != 0xAAU || encoded[10] != 0xEBU)')
    assert old != shared
    stock = compile_decoder('stock_rule', old)
    fixture = ROOT / 'scripts/fixtures/disk2_turbo_address_splice.nib'
    meta = json.loads(fixture.with_suffix('.json').read_text())
    raw = fixture.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == meta['raw_sha256']
    good = fixed(raw, 1, 7); bad = stock(raw, 1, 7)
    assert hashlib.sha256(good).hexdigest() == meta['decoded_sha256']
    assert bad != good and bad[:512] == bytes(512)
    # Preserve full address/checksum/data checks while treating only the
    # third address-epilogue byte as part of the overwriteable sync gap.
    for po in (False, True):
        expected = make_sector_track(7)
        base = nibblize_track(expected, 7, po)
        addresses = find_prologues(base, b'\xD5\xAA\x96')
        data_fields = find_prologues(base, b'\xD5\xAA\xAD')
        for third in range(256):
            changed = bytearray(base)
            for pos in addresses:
                changed[pos + 13] = third
            assert fixed(bytes(changed), po, 7) == expected
        for pos, xor in ((addresses[0] + 11, 1), (addresses[0] + 12, 1),
                         (addresses[0] + 9, 1), (data_fields[0] + 3, 1)):
            changed = bytearray(base); changed[pos] ^= xor
            assert fixed(bytes(changed), po, 7) != expected
    report = {'pass': True, 'actual_c_decoder': True,
              'board_fixture_stock_rule_loses_data': True,
              'board_fixture_fixed_hash_matches_native_write': True,
              'third_address_epilogue_values_tested_per_order': 256,
              'sector_orders': ['DOS', 'ProDOS'], 'negative_validation_cases': 8}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS actual C decoder: captured TURBO splice, all third-byte values, and invalid-field rejection')


if __name__ == '__main__':
    main()
