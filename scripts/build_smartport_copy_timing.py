#!/usr/bin/env python3
"""Build a stock-firmware SmartPort read benchmark and owned ProDOS volume."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/smartport-copy-board'
sys.path.insert(0, str(ROOT / 'software/net_transport_checkpoint'))
from prodos_disk import build_image, validate_image


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    sources = [ROOT / 'software/smartport_copy_timing' / name
               for name in ('main.c', 'transport.s')]
    container = OUT / 'SPCOPY.as'
    subprocess.run(['cl65', '-t', 'apple2enh', '-O', '--start-addr', '0x2000',
                    '-Wl', '-D,__FILETYPE__=0xFF', '-o', str(container),
                    '-m', str(OUT / 'client.map'), '-Ln', str(OUT / 'client.lbl')]
                   + [str(p) for p in sources], cwd=OUT, check=True)
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
    # Keep program, globals and heap below the first deliberately fixed buffer.
    labels = (OUT / 'client.lbl').read_text()
    end = int(next(line.split()[1] for line in labels.splitlines()
                   if line.endswith('.__BSS_RUN__')), 16)
    size = int(next(line.split()[1] for line in labels.splitlines()
                    if line.endswith('.__BSS_SIZE__')), 16)
    assert end + size < 0x4400, hex(end + size)
    (OUT / 'SPCOPY.SYSTEM').write_bytes(program)
    template = (ROOT / 'software/ProDOS_2_4_3.po').read_bytes()
    image = bytearray(build_image(template, program, 'SPCOPY.SYSTEM',
                                 total=1600, volume='SPCOPY'))
    image[1599*512:1600*512] = bytes([0xA5])*512
    validate_image(image)
    (OUT / 'TEST.PO').write_bytes(image)
    digest = lambda b: hashlib.sha256(b).hexdigest()
    report = {'program_sha256': digest(program), 'bss_end': end + size,
              'sources': {str(p.relative_to(ROOT)): digest(p.read_bytes()) for p in sources},
              'prodos_input_sha256': digest(template), 'volume_sha256': digest(image),
              'firmware_rebuilt': False, 'firmware_written': False}
    (OUT / 'client-build.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
