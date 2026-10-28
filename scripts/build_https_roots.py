"""Build the bounded public root set from the pinned Mozilla PEM input."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import warnings
from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa

ROOT=Path(__file__).resolve().parents[1]
PREFIXES=('ISRG Root ','GTS Root ','Amazon Root CA ','DigiCert ','GlobalSign','USERTrust ','Sectigo Public Server ')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'third_party/mozilla-ca/cacert.pem')
    p.add_argument('--output',type=Path,default=ROOT/'software/https_setup')
    p.add_argument('--as-of',default='2026-10-28')
    a=p.parse_args();raw=a.source.read_bytes();at=datetime.fromisoformat(a.as_of).replace(tzinfo=timezone.utc)
    with warnings.catch_warnings():
        warnings.simplefilter('ignore');certs=x509.load_pem_x509_certificates(raw)
    chosen=[]
    for c in certs:
        cn=c.subject.get_attributes_for_oid(x509.NameOID.COMMON_NAME)
        if not cn or not cn[0].value.startswith(PREFIXES):continue
        if not c.not_valid_before_utc<=at<c.not_valid_after_utc:continue
        key=c.public_key()
        if isinstance(key,rsa.RSAPublicKey) and key.key_size<=4096:pass
        elif isinstance(key,ec.EllipticCurvePublicKey) and key.curve.name in ('secp256r1','secp384r1'):pass
        else:continue
        chosen.append(c)
    chosen.sort(key=lambda c:c.subject.rfc4514_string())
    digest=hashlib.sha256(raw).hexdigest()
    text=(f'# Appletini bounded public roots, selected {a.as_of}.\n'
          '# Source: https://curl.se/ca/cacert.pem (Mozilla, MPL 2.0).\n'
          f'# Full source SHA256: {digest}\n'
          '# This subset does not trust every public issuer. No test CA is included.\n\n').encode()
    records=[]
    for c in chosen:
        pem=c.public_bytes(serialization.Encoding.PEM);text+=pem+b'\n'
        records.append({'subject':c.subject.rfc4514_string(),'not_after':c.not_valid_after_utc.isoformat(),
                        'der_sha256':hashlib.sha256(c.public_bytes(serialization.Encoding.DER)).hexdigest()})
    if not chosen or len(text)>65567:raise RuntimeError('Public root policy exceeds the firmware file budget')
    a.output.mkdir(parents=True,exist_ok=True);(a.output/'ATNETCA.PEM').write_bytes(text)
    (a.output/'roots.json').write_text(json.dumps({'source':'https://curl.se/ca/cacert.pem','source_sha256':digest,
        'as_of':a.as_of,'pem_bytes':len(text),'sha256':hashlib.sha256(text).hexdigest(),'roots':records},indent=2)+'\n',newline='\n')
    print(f'Built {len(chosen)} public roots, {len(text)}/65567 PEM bytes; TLS pool use still requires measurement')

if __name__=='__main__':main()
