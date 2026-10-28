#!/usr/bin/env python3
"""Import the pinned, official Mbed TLS release into this repository."""

import hashlib
from pathlib import Path
import tarfile
import urllib.request
from patch_mbedtls import apply_patch


VERSION = "3.6.7"
SHA256 = "a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6"
URL = ("https://github.com/Mbed-TLS/mbedtls/releases/download/"
       f"mbedtls-{VERSION}/mbedtls-{VERSION}.tar.bz2")
ROOT = Path(__file__).resolve().parents[1]


def main():
    cache = ROOT / "build/https-cpu0-checkpoint"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"mbedtls-{VERSION}.tar.bz2"
    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            archive.write_bytes(response.read())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SHA256:
        raise RuntimeError(f"Mbed TLS archive SHA256 mismatch: {digest}")
    target = ROOT / "third_party/mbedtls"
    with tarfile.open(archive) as contents:
        for member in contents.getmembers():
            parts = member.name.split("/", 1)
            if len(parts) != 2 or not member.isfile():
                continue
            relative = Path(parts[1])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Invalid archive member")
            if not (parts[1].startswith(("include/", "library/")) or
                    parts[1] in ("LICENSE", "README.md", "ChangeLog")):
                continue
            destination = target / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(contents.extractfile(member).read())
    apply_patch(target)
    (target / "APPLETINI_PROVENANCE.txt").write_text(
        f"Mbed TLS {VERSION}\nSource: {URL}\nSHA256: {SHA256}\n"
        "Imported include/, library/, LICENSE, README.md and ChangeLog.\n"
        "Local change: library/ssl_tls12_client.c enables existing restartable\n"
        "ECDH state for ECDHE-RSA when APPLETINI_TLS_RSA_RESTART is set.\n"
        "Reproduce with scripts/patch_mbedtls.py. Verification policy is unchanged.\n", encoding="utf-8")
    print(f"Verified and imported Mbed TLS {VERSION}: {digest}")


if __name__ == "__main__":
    main()
