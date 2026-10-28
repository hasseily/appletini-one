# Appletini changes to Mbed TLS 3.6.7

The import stays pinned to the archive hash in `APPLETINI_PROVENANCE.txt`.
`scripts/patch_mbedtls.py` changes one TLS 1.2 client condition. With
`APPLETINI_TLS_RSA_RESTART`, ECDHE-RSA uses the existing restartable ECDH
state machine that upstream enables for ECDHE-ECDSA. The normal ECDSA-only
probe keeps its original behavior.

The patch keeps required chain, hostname and time checks. RSA signature
verification stays synchronous. Native handshake tests must show crypto
yields for both suites, successful verified handshakes, and rejection of
bad certificates. Board tests measure the remaining RSA verification step.
It adds no cipher, trust exception, or insecure fallback.
