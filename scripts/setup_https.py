"""Set up an SD card once per Appletini, with OS entropy and public roots.

This tool never overwrites an existing seed, prints secret bytes, or reads
seeds back to the PC. Keep each initialized card with its original board.
"""
import argparse
import os
from pathlib import Path
import ipaddress
import struct
import zlib

ROOT=Path(__file__).resolve().parents[1]

def seed_record(entropy):
    if len(entropy)!=48 or not any(entropy):raise ValueError('48 random bytes required')
    head=b'ATSEED01'+struct.pack('<Q',1)+entropy
    return head+struct.pack('<I',zlib.crc32(head))+b'DONE'

def setup(directory,roots,offset,dns):
    directory=Path(directory).resolve()
    if not directory.is_dir():raise ValueError('Select an existing SD-card root directory')
    if not -840<=offset<=840:raise ValueError('UTC offset must be -840..840 minutes')
    address=ipaddress.IPv4Address(dns).packed
    public=Path(roots).read_bytes()
    if not public or len(public)>65567 or b'-----BEGIN CERTIFICATE-----' not in public:
        raise ValueError('Invalid or oversized trusted-root file')
    paths=[directory/'ATNETA.SED',directory/'ATNETB.SED']
    exists=[p.exists() for p in paths]
    if any(exists) and not all(exists):raise ValueError('Incomplete seed state; recover it deliberately, do not overwrite')
    if not any(exists):
        record=seed_record(os.urandom(48))
        for path in paths:
            # Exclusive creation prevents a repeated setup from rolling a seed back.
            with path.open('xb') as f:f.write(record);f.flush();os.fsync(f.fileno())
        del record
    (directory/'ATNETCA.PEM').write_bytes(public)
    (directory/'ATNET.CFG').write_bytes(b'N\x01'+address+struct.pack('<h',offset))
    return not any(exists)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sd_root',type=Path)
    p.add_argument('--utc-offset',type=int,default=0,help='RTC local time minus UTC, in minutes; default expects the RTC set to UTC')
    p.add_argument('--dns',default='0.0.0.0',help='optional override; zero uses the menu/DHCP DNS setting')
    p.add_argument('--roots',type=Path,default=ROOT/'software/https_setup/ATNETCA.PEM')
    a=p.parse_args();created=setup(a.sd_root,a.roots,a.utc_offset,a.dns)
    print('Created per-board random state.' if created else 'Kept existing random state unchanged.')
    print('Wrote public roots and UTC/DNS settings. Set the RTC in the config menu before HTTPS use.')

if __name__=='__main__':main()
