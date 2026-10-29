# Firmware F1.3.4

F1.3.4 adds the services used by the graphical A2MONO browser: direct HTTPS,
mobile User-Agent overrides, automatic HTTPS setup and streaming downloads
to the FAT SD card. It accepts HTTP header lines up to 8192 bytes and embeds
29 public trust roots, including the Go Daddy G2 root needed by the tested
archive.org chain. Certificate, hostname and date checks stay enabled;
SHA-1 certificate signatures remain disabled.

## Install and set up HTTPS

Use the normal SD updater with `FIRMWARE.BIN`. Golden B2.0.6 stays in place.
This ARM release reuses the qualified F1.3.3 FSBL, FPGA/XSA and CPU1. The
FPGA SHA-256 is
`94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34`.
No FPGA, CPU1 or Golden rebuild runs for this release.

A new writable FAT SD card needs no Python tool, PEM file or `ATNET.CFG`.
Configure Ethernet/DNS in the Appletini menu or use DHCP and set the RTC to
UTC. With both private seed files absent, the firmware collects internal
XADC samples, checks them and creates verified seed records. The source
remains experimental: its health checks do not establish a cryptographic
entropy rate. Sampling runs at setup, not during HTTP requests.

Existing valid seeds remain in use and rotate normally. Keep them with
their board; do not clone or roll them back. Partial or corrupt seed state
fails without regeneration. Without `ATNET.CFG`, DNS uses menu/DHCP settings
and the RTC offset is zero. Existing valid settings keep their explicit
DNS and local-minus-UTC offset.

An optional FAT SD-root `ATNETCA.PEM` **replaces** the embedded roots. An old
override still takes precedence. Remove it to use the firmware roots, or
replace it with the supplied 29-root bundle, then reboot. Malformed overrides
fail closed. `HTTPS-SETUP.zip` contains the public roots, source receipts,
licenses and the optional older PC setup tool; it contains no private seeds.

## Browsing and downloads

The matching A2MONO browser uses firmware HTTPS while rendering text and
inline GIF, PNG and baseline JPEG images on the Apple II. Its source and
disk image are supplied by the separate Appletini software project.

Downloads save to `0:/downloads` on the FAT SD card, outside the ProDOS
image. The ARM streams the same response directly to storage; it does not
repeat the GET or limit files to the Apple page buffer. A response marked
`Content-Disposition: attachment`, or an explicit browser download link,
can trigger saving. MIME type or extension alone does not.

The service confines and sanitizes filenames, chooses a numbered suffix
without overwriting existing files, and uses an exclusive `.part` file.
HTTP success, length checks, sync and close precede the final rename.
Cancellation and errors remove the owned partial file. Failed cleanup
remains pending and blocks SD handover until retry succeeds. Completed
files survive cancellation and reset. Transfers allow 30 seconds without
received data and at most 24 hours in total.

HTTP ownership keeps other clients from consuming or replacing a download.
FTP, USB SD sharing, menu/update operations and media refresh wait for file
cleanup. Screenshot and printer writes reuse the registered filesystem;
necessary media refresh waits until it is safe. Older NET and JSON clients
retain their existing request behavior.

## Validation and provenance

The user reported that a ProDOS-file download from appletini.org worked
before the archive.org fix. After the archive firmware and matching browser
were supplied, the user reported that it worked and requested this release.
The installed flash hash was not independently read. F1.3.4 changes only
the version header from that working candidate; the final versioned image
has not had a separate physical-board run.

Native tests cover download streaming beyond 64 KiB, ownership, cancellation,
file faults and races, storage handover, automatic seed setup, root selection,
TLS verification and mobile headers. Actual 6502 and ProDOS tests cover the
browser separately. Native archive.org TLS checks pass with the new root;
the preserved old override and a wrong hostname still fail. These test
categories do not establish board timing or a measured entropy rate.

The release audit checks the firmware manifest and CRC, every packaged ARM
load byte, the exact qualified FPGA/XSA and embedded CPU1. CPU0 keeps a
64 KiB stack and 64 MiB heap. The reused timing manifest reports setup
+0.047 ns and hold +0.034 ns; no new timing run occurred. Unrelated local
HDL edits were excluded.

`ARM_BUILD.json`, `FIRMWARE_AUDIT.json`, `ARM-SOURCES.zip`, source hashes and
`validation/` retain the source, build and test evidence. Source changes
remain uncommitted for the user's publication step. This preparation does
not publish, tag, push or flash anything.
