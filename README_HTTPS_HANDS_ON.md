# HTTP/S, JSON and STARFALL hands-on build

This is a development candidate from 28 October 2026. It uses the unchanged
F1.3.3b3 FPGA payload and a new normal ARM application build. Golden stays
B2.0.6. The display still names the F1.3.3b3 base; use the package hashes to
identify the candidate.

The files are in `deliverables/https-starfall-test2-20261028/`. Install the
firmware through the normal SD updater. Select `STARFALL.po` as a SmartPort
disk and run at 33 MHz or TURBO with slot-4 audio. The same package includes
a standalone `A2LYNX.po` browser disk. Its README gives the first-use seed,
public-root, RTC-offset and DNS steps. The connected test board already has
its own seed, roots and configuration; do not run first-use setup again.

## Results

| Area | Result and evidence |
| --- | --- |
| Normal firmware | HTTP/S is part of the normal build. Test provisioning, trace and register commands are absent. Startup rotates/syncs the seed and parses the roots before Apple release. A request uses cached setup state. |
| Public TLS | The board fetched the public STARFALL ladder and RSA 2048/4096 sites with required certificate checks. Wrong hostnames and expired certificates failed. No controlled test root was installed. |
| JSON and streaming | The actual SDK passed at 33 MHz and TURBO: POST, 32 KiB retained string/object/escaped UTF-8 fixtures, a 32 KiB stream, invalid JSON, and certificate rejection. The worst measured JSON slice was 7 us. The stream test found and fixed a stale occupancy count that hid END after the last bytes. |
| ARM memory | The live public requests used at most 96128 of 524288 TLS-pool bytes. The measured foreground stack upper bound was 16536 of 65536 bytes, including a 512-byte unpainted guard. This does not measure the separate exception-mode stacks. |
| SDK failure handling | Actual 65C02 model tests cover 9–512-byte replies, dirty scratch RAM, bad replies, timeout, generation mismatch and network-not-ready. NET and optional AMEM share a resident poison flag; a failed reply from either blocks the other without sending a second command. Programs that do not use networking keep their offline paths. |
| Card leaderboard | The real STARFALL panel submitted an explicitly artificial test run, read its rank, retried the same ID and read the top ten. D1 held one matching row with platform `appletini`. The acceptance check removed only that owned test row afterward. |
| Browser leaderboard | A real browser game reached game over and submitted its final score to the local service as `web`. The deployed game disk and browser module match the tested files. Production API storage and responses carry `platform`; a repeated ID with different data returns 409. |
| Game fixes | Host tests cover firing a bomb pickup at the maximum while keeping the reserve full. A focused actual-65C02 render test at both speeds checks that Excalibur has no phase-zero sprites and enters at the top after its first step. |
| A2LYNX | On the board: public HTTPS HTML, HTTPS redirect, plain text, the 10 KiB cut notice, hostname/expiry rejection, non-text rejection, Esc cancellation and a successful later request passed. A body-read/status race now preserves the certificate error message. A separate run on exact stock F1.3.3b3 launched A2LEGACY.SYSTEM when the new service was absent; the candidate and STARFALL were then restored. |

The public service is <https://appletini.org/leaderboards/starfall>. The
browser game is <https://appletini.org/demo/starfall>. Each public name has
one best rank across platforms. Browser submissions use `web`; card
submissions use `appletini`. A run keeps its ID and frozen POST body across
manual retries. A new game permits a new run ID. Real-time co-op is deferred.

On game over, press N, enter a public name and press Return. Tab reads the
top ten and Esc returns. Q types Q in the name field and quits outside name
entry. On the web, use the name field and Submit run below the display.

## Release acceptance

The user reports successful full STARFALL gameplay and leaderboard testing.
The later [RSA integrity stress run](README_RSA_STRESS_CHECKPOINT.md) passes
at 33 MHz and TURBO. Under the agreed criterion, stutter is acceptable while
disk data, filesystem structure and the capture stream stay intact, reported
USB errors remain zero, and services recover after load. The synchronous
24-28 ms RSA work remains a known timing limit rather than an open blocker.
Firmware F1.3.3 then passes a separate final-image board check.

The selected bundle contains 26 public roots, rather than every public
issuer. The service fails closed for missing issuers, a wrong clock or a bad
certificate. Seed renewal can require a reboot after the DRBG call limit.
This is bounded integrity evidence; it does not promise jitter-free networking or cover USB0 throughput.

## Evidence and reproduction

`validation/https-application-20261028/` holds the selected reports. Full
UART logs stay under the matching `build/` directories. The native checks
and the board checks are separate evidence.

```powershell
python scripts/test_http_service.py
python scripts/test_http_network.py
python scripts/test_http_tls.py
python scripts/test_net_service.py
python scripts/build_https_checkpoint.py --service --application --package
```

The board harnesses use copied, hashed inputs in
`build/https-starfall-acceptance/inputs/`. They save a journal before changing
the SD card. `run_https_starfall_acceptance.py` tests the firmware and SDK;
`run_starfall_online_acceptance.py` drives the game panel;
`run_a2lynx_acceptance.py` drives the browser. The latter two use
`board_http_acceptance.py`. They restart the board. Do not run them during a
user test session. The test firmware/FPGA provenance is in `firmware-build.json`
inside the hands-on package.
