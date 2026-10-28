# HTTPS first-use setup

Run `python scripts/setup_https.py <SD-root> --utc-offset <minutes>` once
for each board. The tool needs only Python's standard library. Use the
SD-card root, not a directory that will be copied to several boards.

The tool creates two private random-state files from the PC's OS random
source. It never replaces existing seeds. Keep the initialized card with
that board. Do not clone it or restore an older seed from a backup.
An incomplete pair fails closed and needs deliberate recovery.

Set the RTC in the Appletini config menu. The offset is local RTC time
minus UTC: for UTC+3 use 180. The default, zero, expects the RTC set to UTC.
DNS normally comes from the Ethernet menu or DHCP. `--dns` sets an explicit
override; its default `0.0.0.0` leaves the menu/DHCP value in charge.

`ATNETCA.PEM` contains 26 public roots from the pinned Mozilla bundle,
covering ISRG, Google Trust Services, Amazon, DigiCert, GlobalSign,
USERTrust and Sectigo. This is a bounded subset, not every public issuer.
It has no test certificate. Missing issuers, bad hostnames, expired chains
and wrong time fail certificate validation; there is no insecure fallback.
`roots.json` records every root, expiry, source hash and output hash.

The original bundle and MPL 2.0 license live in `third_party/mozilla-ca`.
`scripts/build_https_roots.py` rebuilds this subset from that local input.
Its 33175-byte PEM fits the 65567-byte file limit. The native TLS test
measured 98488 bytes of the 524288-byte pool with all roots and SSL setup;
the live RSA stress run measures 96128 bytes with the chosen public roots
and peer chains, with no allocation failure.

The firmware rotates, syncs and reads back a new seed record before it
exposes random output to the network. This SD scheme has no hardware
entropy source or rollback protection. The current firmware prevents FTP
access to the private paths; old firmware may not provide that protection.

Firmware F1.3.3 includes this service in the normal application.
Use an SD-card reader or SD remote mounting for first-use seed setup;
the firmware blocks seed access through FTP. An initialized card keeps its
existing seed files across a firmware update.
