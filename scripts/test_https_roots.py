"""Check pinned roots, reproducibility and exact SSL.com/Go Daddy additions."""
from datetime import datetime, timezone
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys
import warnings

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

ROOT=Path(__file__).resolve().parents[1]
BUILD=ROOT/'build/https-root-tests'
AS_OF='2026-10-29'
EXPECTED={
    'SSL.com TLS ECC Root CA 2022':'c32ffd9f46f936d16c3673990959434b9ad60aafbb9e7cf33654f144cc1ba143',
    'SSL.com TLS RSA Root CA 2022':'8faf7d2e2cb4709bb8e0b33666bf75a5dd45b5de480f8ea8d4bfe6bebc17f2ed',
    'Go Daddy Root Certificate Authority - G2':'45140b3247eb9cc8c5b4f0d7b53091f73292089e6e5a63e2749dd3aca9198eda',
}
# Fingerprint-set digest from the previous 28-root policy. This proves that
# the archive.org repair adds only the direct G2 root and preserves every
# previous trust anchor.
PREVIOUS_28='b52c3df6bff1fbf898bf529e218a93ffdce0e593696f061b4890f9607021f42f'

def certificates(raw):
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        return x509.load_pem_x509_certificates(raw)

def fingerprint(certificate):
    return certificate.fingerprint(hashes.SHA256()).hex()

def build(source, output, as_of=AS_OF):
    subprocess.run([sys.executable,str(ROOT/'scripts/build_https_roots.py'),
                    '--source',str(source),'--output',str(output),'--as-of',as_of],check=True)
    return (output/'ATNETCA.PEM').read_bytes(),json.loads((output/'roots.json').read_text())

def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    source=ROOT/'third_party/mozilla-ca/cacert.pem'
    original=source.read_bytes()
    pem,metadata=build(source,BUILD/'reproduced')
    assert pem==(ROOT/'software/https_setup/ATNETCA.PEM').read_bytes()
    assert metadata==json.loads((ROOT/'software/https_setup/roots.json').read_text())
    selected=certificates(pem)
    assert len(selected)==len(metadata['roots'])==29
    assert len(pem)==metadata['pem_bytes']<=65567
    assert hashlib.sha256(pem).hexdigest()==metadata['sha256']
    assert hashlib.sha256(original).hexdigest()==metadata['source_sha256']
    der=b''.join(c.public_bytes(serialization.Encoding.DER) for c in selected)
    assert len(der)==metadata['embedded_der_bytes']==26175
    assert hashlib.sha256(der).hexdigest()==metadata['embedded_der_sha256']
    for name in ('http_roots.c','http_roots.h'):
        assert (BUILD/'reproduced'/name).read_bytes()==(ROOT/'ps_sources/frontend'/name).read_bytes()
    generated=(BUILD/'reproduced/http_roots.c').read_text()
    data=re.search(r'root_der\[[^]]+\]\s*=\s*\{(.*?)\};',generated,re.S)[1]
    assert bytes(int(n,16) for n in re.findall(r'0x([0-9a-f]{2})',data))==der
    source_fingerprints={fingerprint(c) for c in certificates(original)}
    at=datetime.fromisoformat(AS_OF).replace(tzinfo=timezone.utc)
    exact={}
    for c in selected:
        assert fingerprint(c) in source_fingerprints, 'Root did not come from the pinned source'
        assert c.not_valid_before_utc<=at<c.not_valid_after_utc
        cn=c.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)[0].value
        if cn.startswith(('SSL.com','Go Daddy')):
            exact[cn]=fingerprint(c)
        if cn=='Go Daddy Root Certificate Authority - G2':
            assert c.subject==c.issuer
            assert c.signature_hash_algorithm.name=='sha256'
    assert exact==EXPECTED, exact
    previous={fingerprint(c) for c in selected}-{EXPECTED['Go Daddy Root Certificate Authority - G2']}
    assert len(previous)==28
    assert hashlib.sha256(('\n'.join(sorted(previous))+'\n').encode()).hexdigest()==PREVIOUS_28
    # A valid similarly named public certificate must not broaden the exact
    # selector. This catches adding SSL.com or the new names to PREFIXES.
    key=ec.generate_private_key(ec.SECP384R1())
    extras=[]
    for serial,common_name in enumerate(('SSL.com TLS ECC Root CA 2022 Extra',
                                        'Go Daddy Root Certificate Authority - G2 Extra'),1):
        name=x509.Name([x509.NameAttribute(x509.NameOID.COMMON_NAME,common_name)])
        extras.append(x509.CertificateBuilder().subject_name(name).issuer_name(name)
                      .public_key(key.public_key()).serial_number(serial)
                      .not_valid_before(datetime(2025,1,1,tzinfo=timezone.utc))
                      .not_valid_after(datetime(2040,1,1,tzinfo=timezone.utc))
                      .add_extension(x509.BasicConstraints(ca=True,path_length=None),critical=True)
                      .sign(key,hashes.SHA384()))
    augmented=BUILD/'augmented-source.pem'
    augmented.write_bytes(original+b'\n'+b''.join(c.public_bytes(serialization.Encoding.PEM) for c in extras))
    narrowed,narrow_metadata=build(augmented,BUILD/'narrowed')
    assert len(certificates(narrowed))==29
    assert not {fingerprint(c) for c in extras}&{fingerprint(c) for c in certificates(narrowed)}
    assert {row['der_sha256'] for row in narrow_metadata['roots']}=={fingerprint(c) for c in selected}
    # Selection dates cannot trust anchors before their validity begins.
    earlier,_=build(source,BUILD/'earlier',as_of='2020-10-29')
    future={value for name,value in EXPECTED.items() if name.startswith('SSL.com')}
    assert not future&{fingerprint(c) for c in certificates(earlier)}
    print('PASS pinned roots: 29 anchors, previous 28 unchanged, exact SSL.com/Go Daddy fingerprints, reproducible PEM/embedded DER, file budget and validity dates')

if __name__=='__main__':main()
