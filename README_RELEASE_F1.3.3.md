# Firmware F1.3.3

F1.3.3 adds HTTP/S and JSON over the onboard Ethernet connection. The ARM
handles DNS, TLS, HTTP framing and JSON Pointer lookups. Apple software uses
the slot-7 SmartPort private interface, with start, poll, read and cancel
calls. The optional libtini API gives games a small client and leaves their
offline paths available on older firmware.

Software 1.1.0 supplies STARFALL with the public leaderboard and A2LYNX with
direct HTTPS text browsing. The browser player submits platform `web`; the
card submits `appletini`. Both keep one run ID across retries. Rankings use
server UTC and each public name's best score. A2LYNX falls back to IP65 on
older firmware. Its page limit is 10 KiB; it does not run website JavaScript.

The firmware also fixes TURBO Disk II address-field decoding, preserves
stored sectors when a write capture does not contain a valid replacement,
and keeps private transport replies outside video RAM. It retains the
64 KiB frontend stack, 64 MiB heap, ONE//e standby screen and fixed keyboard
menu controls from the F1.3.3 prereleases.

## Install and set up HTTPS

Use the normal SD updater with `FIRMWARE.BIN`. Golden B2.0.6 stays in place.
This is an ARM update: it keeps the exact tested F1.3.3b3 FPGA/XSA/FSBL
payload, whose FPGA SHA-256 is
`94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34`.
No FPGA build or Golden update runs for this release.

For first use on each board, unzip `HTTPS-SETUP.zip`, mount that board's SD
card on the PC and run:

```powershell
python setup_https.py E:\ --roots ATNETCA.PEM --utc-offset 0
```

Use the actual SD root and the RTC's local-time-minus-UTC offset in minutes.
For an RTC set to UTC+3, use 180. Set the correct RTC date/time in the menu.
Set DNS in the Ethernet menu or use DHCP. An optional setup `--dns` value
overrides that choice. Use an SD reader or SD remote mounting for initial
seed setup; FTP blocks the private seed files.

The tool uses the PC's OS random source and keeps existing seed records.
Do not clone them to another board or restore an older seed. The release
contains 26 selected public roots and no controlled test root. Missing
issuers, wrong hostnames, expired chains and a wrong clock fail closed.
The service supports TLS 1.2. A board can need a reboot for seed renewal
after the DRBG call limit.

HTTP takes the Ethernet chip while a request runs. FTP/SD remote maintenance
and simultaneous Apple IP65 connections are outside that ownership model.
Existing Uthernet II applications keep their ordinary interface.

## Timing and validation

Heavy RSA work can stutter video or input. The user accepts this when data
stays intact. Two complete stress runs at 33 MHz and TURBO pass that
criterion: 130 verified RSA handshakes, 180 SmartPort write/readback cycles,
180 Disk II write/readback cycles, complete saved-file and filesystem checks,
and zero capture-corruption or reported USB-error counters. The longest
RSA work step is 27.858 ms; the longest USB1 service gap under combined
load is 70.056 ms. Recovery phases pass. These are bounded tests, not a
jitter-free timing guarantee.

The user reports a full successful STARFALL gameplay and leaderboard test.
On the board, A2LYNX passes public HTTPS HTML/text, redirects, certificate
rejection, page cutting, non-text rejection and cancellation/recovery. Its
older-firmware fallback also passes on exact stock F1.3.3b3. The final normal
F1.3.3 image passes boot/version, updater-receipt and public HTTPS checks.

The image audit checks the target manifest and CRC, the selected FPGA/XSA,
all ARM file-backed load bytes, embedded CPU1, firmware label and memory
reservations. Source and native tests remain separate from board evidence.
The [RSA report](README_RSA_STRESS_CHECKPOINT.md) and
`validation/F1.3.3-release/` retain the selected evidence.
