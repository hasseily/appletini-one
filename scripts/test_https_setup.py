"""Check setup record format, exclusive seed lifetime and config bounds."""
from pathlib import Path
import struct
import tempfile
import zlib
from setup_https import ROOT,setup,seed_record

def main():
    record=seed_record(bytes(range(48)))
    assert len(record)==72 and record[:16]==b'ATSEED01'+struct.pack('<Q',1)
    assert record[64:68]==struct.pack('<I',zlib.crc32(record[:64])) and record[68:]==b'DONE'
    out=ROOT/'build/https-setup-tests';out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as d:
        d=Path(d);roots=ROOT/'software/https_setup/ATNETCA.PEM'
        assert setup(d,roots,180,'0.0.0.0')
        a=(d/'ATNETA.SED').read_bytes();b=(d/'ATNETB.SED').read_bytes()
        assert a==b and len(a)==72 and a!=record
        assert not setup(d,roots,-60,'192.0.2.53')
        assert (d/'ATNETA.SED').read_bytes()==a and (d/'ATNETB.SED').read_bytes()==b
        assert (d/'ATNET.CFG').read_bytes()==b'N\x01'+bytes((192,0,2,53))+struct.pack('<h',-60)
        (d/'ATNETB.SED').unlink()
        try:setup(d,roots,0,'0.0.0.0')
        except ValueError:pass
        else:raise AssertionError('Partial seed state was accepted')
        assert (d/'ATNETA.SED').read_bytes()==a
    print('PASS HTTPS first-use setup: OS seed format, no overwrite/rollback, partial-state refusal, UTC and DNS')

if __name__=='__main__':main()
