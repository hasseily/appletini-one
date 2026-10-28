#!/usr/bin/env python3
"""Build a ProDOS-only disk write check for the unchanged stock firmware."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/disk2-stock-checkpoint'
sys.path.insert(0, str(ROOT / 'software/net_transport_checkpoint'))
from prodos_disk import build_image, validate_image


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'software/disk2_write_checkpoint/main.c'
    container = OUT / 'D2WRITE.as'
    subprocess.run(['cl65', '-t', 'apple2enh', '-O', '--start-addr', '0x2000',
                    '-Wl', '-D,__FILETYPE__=0xFF', '-o', str(container),
                    '-m', str(OUT / 'client.map'), str(source)], cwd=OUT, check=True)
    raw = container.read_bytes()
    assert raw[:8] == bytes.fromhex('0005160000020000')
    entries = {}
    for at in range(26, 26 + 12 * int.from_bytes(raw[24:26], 'big'), 12):
        kind, offset, size = struct.unpack_from('>III', raw, at)
        assert offset + size <= len(raw)
        entries[kind] = raw[offset:offset + size]
    assert entries[11] == bytes.fromhex('00c300ff00002000')
    program = entries[1]
    assert program[:3] == bytes.fromhex('a2ff9a')
    (OUT / 'D2WRITE.SYSTEM').write_bytes(program)
    template = (ROOT / 'software/ProDOS_2_4_3.po').read_bytes()
    volumes = {}
    for volume, blocks in (('D2HOST', 1600), ('D2TEST', 280)):
        image = build_image(template, program, 'D2WRITE.SYSTEM', total=blocks,
                            volume=volume, files=[('SCRATCH', bytes(512), 0x06, 0)])
        validate_image(image)
        (OUT / (volume + '.po')).write_bytes(image)
        volumes[volume] = hashlib.sha256(image).hexdigest()
    report = {'program_sha256': hashlib.sha256(program).hexdigest(),
              'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
              'prodos_input_sha256': hashlib.sha256(template).hexdigest(),
              'volume_sha256': volumes, 'firmware_rebuilt': False}
    (OUT / 'client-build.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS standalone ProDOS write client and owned scratch volumes')


if __name__ == '__main__':
    main()
