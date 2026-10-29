# Controlled TLS peer certificate

`http_tls_controlled_ca.pem` is the public certificate for the local TLS test
peer used by `scripts/test_http_tls_handshake.py`. It contains no private key.
The test copies it into a generated test-only bundle alongside public roots.
Firmware trust generation and release packages must never include this issuer.

- Subject and issuer: `CN=Appletini trusted test CA`.
- Certificate SHA-256: `2426f2bc7e885c5c410bea54d1e2e12f8716a1e1d208bd8e59361bce933ca7a4`.
- PEM file SHA-256: `3a095db57b792ab9177495add6b941e79a3a44986eb28eed74f2f8645a5aa4ef`.
- Valid from 2026-10-26 14:10:52 UTC to 2027-10-27 14:10:52 UTC.
- The certificate was copied unchanged from the controlled peer checkpoint.

The live test needs the matching peer on `10.17.1.17`, ports 8443–8446.
It checks valid ECDSA/RSA chains and rejects wrong names, untrusted issuers,
and expired leaf certificates. `--build-only` needs no running peer.
When the peer CA changes or expires, update this fixture and its pinned hash
explicitly. No test reads a CA from an old build-output folder.
