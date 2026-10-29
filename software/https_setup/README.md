# HTTPS setup

The automatic-setup firmware embeds its public CA certificates and can
create its own seed files. A new writable SD card needs no Python setup,
`ATNETCA.PEM` or `ATNET.CFG`. Enable Ethernet, configure its address and DNS
in the Appletini menu or use DHCP, and set the RTC to UTC.

F1.3.4 includes this automatic setup. Its XADC source is experimental and
has not had its entropy rate measured
on physical boards. Certificate, hostname and date checks remain enabled.

## Automatic random-state setup

When both `ATNETA.SED` and `ATNETB.SED` are absent, the firmware collects
4096 raw internal XADC temperature samples before releasing the Apple CPU.
It disables averaging, checks for stuck, biased and short repeating sample
patterns, and conditions accepted samples with SHA-256. These checks do not
prove cryptographic entropy. Timers, serial numbers and MAC addresses do
not supply the seed.

The firmware exclusively creates, syncs and reads back both seed records.
It then advances and verifies the saved state before exposing random data
to the network. A failed sampler or failed SD write stops HTTPS setup.
Sampling never runs in a live HTTP request.

Valid existing seeds skip XADC sampling and keep the established rotation
scheme. Partial, corrupt or unreadable state fails instead of silently
replacing it. Keep the initialized card with its board; do not clone its
seed files or restore older copies. FTP cannot access the private paths.
The SD scheme does not provide rollback protection.

The UART command `http status` reports whether this boot created a seed,
the sampler result, sample/change counts and timing. It never prints a
seed or raw samples. No FPGA change or extra board component is required.

## Public certificates

The built-in bundle has 29 public roots from the pinned Mozilla input,
including ISRG, Google Trust Services, Amazon, DigiCert, GlobalSign,
USERTrust, Sectigo, the SSL.com 2022 ECC and RSA roots, and Go Daddy Root
Certificate Authority - G2. It has no test
CA and does not cover every public issuer.

An optional `ATNETCA.PEM` in the FAT SD-card root replaces the built-in
bundle. An old file still takes precedence; remove it to use the firmware
roots, or replace it with the current public bundle. A malformed or
unreadable override fails rather than falling back. Reboot after changing
it. Missing issuers, wrong hostnames, expired chains and wrong time fail
certificate verification.

`scripts/build_https_roots.py` rebuilds the bounded PEM and generated
`ps_sources/frontend/http_roots.c`/`.h` from project-owned inputs. The 29
roots occupy 26175 DER bytes. `roots.json` records the source and hashes.
The source bundle and MPL 2.0 notice live in `third_party/mozilla-ca/`.
The TLS allocator retains its 524288-byte pool.

The archive.org update adds only the direct, self-signed Go Daddy G2 root
from that pinned input. Its SHA-256 fingerprint is
`45140b3247eb9cc8c5b4f0d7b53091f73292089e6e5a63e2749dd3aca9198eda`.
This lets verification use G2 directly instead of the server's legacy
Class 2 path. SHA-1 certificate signatures remain disabled; the TLS
verification profile has not changed.

## Browser downloads

The download-enabled firmware and matching A2MONO browser save links marked
with HTML `download`, and responses marked `Content-Disposition: attachment`,
to `0:/downloads/` on the physical FAT SD card. The firmware creates the
folder when needed. Ordinary page and image responses keep their normal
browser behavior; a filename extension alone does not trigger a download.

The firmware streams the same HTTP response to SD, so downloads can exceed
the browser's 64 KiB page buffer. The browser shows bytes saved and, when the
server supplies a length, total bytes. Escape cancels a download and leaves
the displayed page in place.

An explicit link filename takes precedence over the response filename,
then the final redirected URL supplies a fallback. Names lose directory
components and unsafe FAT characters. Existing files remain intact: a
collision adds `-1`, `-2`, and so on before the extension. The firmware writes
a private `.part` file, syncs and closes it, then renames it after success.
Cancellation or failure removes that partial file; failed SD cleanup blocks
new downloads and remote SD handover until cleanup succeeds.

Downloads use the existing HTTPS certificate checks. They allow 30 seconds
without new network data and at most 24 hours in total. The browser needs
the matching download-capable firmware; it reports an update requirement
when an older firmware does not advertise this capability.

## Optional settings and older firmware

With no `ATNET.CFG`, DNS comes from the menu/DHCP and the RTC must use UTC.
An existing valid config keeps its explicit DNS and local-minus-UTC
offset. A malformed existing config fails instead of being ignored.

The older setup tool remains available for released firmware or for
PC-generated initial random state:

```text
python scripts/setup_https.py <SD-root> --utc-offset <minutes>
```

It creates missing seed pairs from the PC OS random source, never replaces
existing seeds, and writes the public CA file and settings. For UTC+3 the
offset is 180; zero expects UTC. Its default DNS `0.0.0.0` uses menu/DHCP.
Use the actual SD root for one board, not a folder copied to many boards.
