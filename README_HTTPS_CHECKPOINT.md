# CPU0 HTTPS checkpoint — 27 October 2026

This file records the earlier checkpoints. The completed F1.3.3 release,
application tests and accepted RSA integrity gate are in the
[release notes](README_RELEASE_F1.3.3.md).

CPU0 completed a verified HTTPS GET while Apple video, a USB mouse, an Xbox
controller, and read-only SD traffic stayed active. It does not yet serve Apple
requests or parse JSON.

The reusable CPU0 HTTP/S backend now passes the next functional checkpoint.
See [its results and limits](#standalone-http-service-checkpoint). The
[HTTPS and JSON plan](README_HTTPS_JSON_PLAN.md) defines the remaining work:
an Apple-side API over the slot 7 SmartPort controller, JSON, libtini,
STARFALL rankings, and direct HTTPS in A2LYNX. No Apple-side networking client
is enabled by this checkpoint.

The loaded request returned HTTP 200 and all 65,536 expected bytes. Both the byte
pattern and SHA-256 matched. Mbed TLS reported no certificate errors. Hardware
tests also rejected a wrong hostname, an untrusted issuer, and an expired chain,
each before returning any HTTP body.

## Scheduling retest

The tuned build reduced the loaded handshake from **7.732 seconds to 1.740 and
1.764 seconds**, about **4.4 times faster** in two runs with the same game/video,
physical mouse and Xbox input, and read-only SD workload. Both runs returned the
full verified 64 KiB response, with zero capture gaps, oversized drains, resyncs,
USB input errors, SD read errors, or CRC errors in all three windows. The three
certificate rejection cases also passed again. The at-rest handshake took 201
ms; capture was idle in that run, so it does not count as video acceptance.

| Measure | Tuned run 1: before / HTTPS / after | Tuned run 2: before / HTTPS / after |
| --- | ---: | ---: |
| Window (seconds) | 3.012 / 7.044 / 3.016 | 3.015 / 7.006 / 3.006 |
| Apple capture frames/second | 47.81 / 47.98 / 48.41 | 47.77 / 47.96 / 48.57 |
| Compositor publications/second | 42.83 / 42.31 / 43.10 | 43.12 / 42.25 / 43.25 |
| Longest USB1 poll gap (ms) | 24.444 / 25.252 / 24.696 | 24.645 / 25.533 / 24.587 |
| HID / Xbox reports during HTTPS | 372 / 253 | 381 / 246 |
| SD reads / cache misses during HTTPS | 299 / 299 | 296 / 296 |
| Mean SD read time (microseconds) | 374 / 375 / 374 | 374 / 375 / 374 |
| Longest SD read (microseconds) | 390 / 392 / 390 | 389 / 393 / 389 |

The longest complete TLS slices took **1.693 and 1.905 ms**. The longest single
TLS steps took 1.578 and 1.582 ms. The new schedule checks a **750 microsecond**
budget between calls, with at most 32 TLS steps and eight socket callbacks per
slice. A call can extend the slice past that budget. Socket chunks grew from 128
to 512 bytes; the ECC restart limit stays at 32 operations. A blocked socket ends
the slice. The main loop still calls the probe at the same point, and the
synthetic SD load runs once per outer poll as before.

Handshake calls consumed about 86 ms of CPU0 time in each loaded run; the rest
of the handshake elapsed time includes service scheduling and network waits.
This is not a whole-system CPU utilization measure. TLS allocator use stayed at
36,660 bytes, with the same reserved pool and trust/entropy/time rules.

The retest shows a small increase in service delay: the worst USB1 poll gap rose
from 24.868 ms in the first HTTPS run to 25.533 ms, and compositor publication
rate dipped by about 1–2% against each run's own baseline. Capture frame rate
stayed close to 48 Hz. Two runs do not establish a worst-case bound; the same
storage and HDMI measurement limits below still apply.

[The scheduling report](validation/https-cpu0-scheduling-20261027/checkpoint-report.json)
holds all six cases, the original comparison, source hashes, raw UART logs, and
restore checks. Loaded runs are `20261027T150926Z-valid` and
`20261027T150940Z-valid`. The tuned package is 4,651,948 bytes:

| Tuned artifact | SHA-256 |
| --- | --- |
| FIRMWARE.BIN | `a3b2b14ada8ed7fe7d36c603dc4d5400260ba64067a2b786c2807f9f030d42d6` |
| CPU0 ELF | `4f23ae4088e7125a1c6f9d28f9d203a89cc5f4856160b7f44b3d6ee5dc9d2d54` |

CPU1, FSBL, and FPGA payload hashes match the first checkpoint below. The tuning
retest also passed AMP startup, USB HID, Xbox, SmartPort, firmware memory, and
framebuffer handoff regressions. Both the probe and restored normal ARM builds
passed their linked-memory checks. The build helper now finds the compiler path
even when the generated command puts include flags before defines. The host
runner's `--require-workload` option checks active capture, input, real SD misses,
and zero errors before accepting a loaded run.

`python scripts/test_https_probe_host.py` also passes six host tests, including
rejection of idle video, missing HID input, a storage CRC error, and a wrong
target clock when replaying saved UART transcripts. F1.3.3b3 is restored on the
board, USB1 is ready, the probe command is absent, and the test server has stopped.

## First checkpoint measured impact

The final run used the ONE//e game workload with the user moving the mouse and
Xbox controller. Each row below comes from the same run: three seconds before
TLS, the request, then three seconds of recovery. The request phase includes TCP
setup, TLS, and the response. The peer sends 256-byte chunks with a 20 ms delay;
this is a service-response test, not a throughput benchmark.

| Measure | Before | During HTTPS | Recovery |
| --- | ---: | ---: | ---: |
| Window (seconds) | 3.006 | 19.555 | 3.014 |
| Apple capture frames/second | 48.24 | 48.58 | 50.10 |
| Compositor publications/second | 43.25 | 43.06 | 43.47 |
| Capture gaps / oversized drains / resyncs | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 |
| HID / Xbox reports | 45 / 103 | 785 / 674 | 128 / 88 |
| USB input errors | 0 | 0 | 0 |
| Longest USB1 poll gap (ms) | 24.448 | 24.868 | 24.651 |
| Longest main-loop gap (ms) | 24.438 | 24.851 | 24.628 |
| SD reads / cache misses | 130 / 130 | 845 / 845 | 131 / 131 |
| Mean SD read time (microseconds) | 373 | 375 | 374 |
| Longest SD read (microseconds) | 395 | 394 | 392 |
| SD read / CRC errors | 0 / 0 | 0 / 0 | 0 / 0 |

Capture frames and compositor publications are software counters, not a physical
HDMI frame-drop measurement. CPU1 publishes a consistent snapshot through
strongly ordered OCM. The display was configured for 50 Hz. No UART reports print
inside the measured windows. The target timer agrees with host elapsed time.

The loaded TLS handshake took **7.732 seconds**; a separate run without USB input
took **0.278 seconds**. The longest TLS poll step in the loaded run took **1.377
ms**. The small restart budget and one 128-byte socket operation per main-loop
pass kept each step short but slowed progress when the compositor was busy. The
scheduling retest above addresses this limit. These measurements do not give a
CPU utilization percentage or a worst-case latency bound.

TLS allocator use peaked at **36,660 bytes**. The probe reserves a **512 KiB**
allocator pool, plus static contexts and buffers; the peak is not the full RAM
cost. The linked frontend still has its checked 64 KiB stack and 64 MiB heap.
Runtime stack high-water was not measured.

The SD workload cycles through 64 blocks in SmartPort image 1, exceeding its
32-block cache, and checks each block against its first CRC. These were real
SD-backed image reads, but the Apple-bus SmartPort read/write counters stayed
zero. This run does not cover Apple-bus storage timing, writes, Disk II traffic,
or USB0 bulk throughput.

## TLS and ownership

The opt-in build uses Mbed TLS 3.6.7 with TLS 1.2,
ECDHE-ECDSA/P-256/AES-128-GCM/SHA-256, required certificate verification, and SNI
plus hostname checking. It trusts only the generated test CA. Each run consumes
a fresh 48-byte Windows CSPRNG seed sent over the owned UART and uses host UTC
for certificate dates. The firmware clears the seed after use. This verifies the
TLS path; production entropy, trusted time, public roots, and DNS remain work.

The probe uses W5100S socket 0 and the existing 4+2+1+1 KiB socket layout. It
temporarily disables slot 1, restores its enable bit at the end, and excludes FTP
while active. Run it with Apple networking idle. This is a diagnostic ownership
rule; the [plan](README_HTTPS_JSON_PLAN.md#ethernet-ownership) describes the
shared-access rule that can replace it.

## First checkpoint build and package provenance

Baseline source: `f602c53731b57556e57b050e0a0102f2aa21c3e7` on
`codex/usb-input-parity`, with the checkpoint source changes. The probe rebuilt
CPU0 and CPU1 ARM firmware. CPU1 only adds capture-health snapshots. It reused
the exact F1.3.3b3 FSBL and FPGA bitstream; no FPGA build took place.

| Artifact | SHA-256 |
| --- | --- |
| Tested probe FIRMWARE.BIN (4,651,308 bytes) | `709177ad8e090c59409335c225ca84db63bea0dc4a870ab30a2dc0a268a5ab55` |
| CPU0 ELF | `4eb591426290cbbb8c5b1dcbb00bbd3e48376268845101ea5fbd6ba665cc6c6a` |
| CPU1 ELF | `520ee01e316e2de48b99eeb368dbb191ad42fadf04a56654b85ec13360225f75` |
| Embedded CPU1 binary | `2746ba87a43a443dd19a586aee35fafdc26839d37c7eacb2c68b742387518fa6` |
| Reused FSBL | `7e47637e290b70701381d8ac3c91c267129dde7e4b12e172e76b4634786f7079` |
| Reused FPGA bitstream | `94c578698466ac057d5a58768e7b1c98fd680dca38a7840cd82f1d226f6dfd34` |
| Restored F1.3.3b3 FIRMWARE.BIN | `cbbdd97ff97d43f7609f3da73ff0368164621350b18ff6ec7398c480d66672c2` |

Delivery used the existing FTP service to stage the package on SD, with a full
SHA-256 readback before reboot into the normal firmware updater. JTAG was absent
and the Golden UART receiver could not be caught. Golden B2.0.6 was not written.
The board is now restored to F1.3.3b3: UART reports that version and rejects the
probe's `https` command. The local HTTPS server has stopped. The generated Vitis
projects were rebuilt with the probe disabled; saved probe artifacts remain in
`build/https-cpu0-checkpoint/`.

## Evidence and checks

[The saved report](validation/https-cpu0-20261027/checkpoint-report.json) contains
the five accepted cases, phase counters, build input hashes, and scope. Matching
raw UART transcripts, deployment records, and restore checks live beside it.
The loaded case is `20261027T145126Z-valid`; the idle case is
`20261027T144913Z-valid`. Earlier exploratory runs are excluded, including one
with an incorrect target timer conversion.

Both the probe and normal ARM builds passed their linked-memory audit. Relevant
checks passed: AMP startup, Uthernet II source/simulation, framebuffer handoff
simulation/synthesis, Xbox transport fixtures, SmartPort service, USB HID service,
and the firmware memory gate. These checks supplement the board run; they do not
stand in for it. The build still emits warnings in existing unrelated code.

## Repeat the checkpoint

Use an existing generated Vitis workspace with the F1.3.3b3 platform. Python needs
`cryptography` and `pyserial`. The repository contains the pinned Mbed TLS sources
and license; `scripts/vendor_mbedtls.py` can reimport the verified upstream
archive. Keep host private test keys under the ignored build directory.

For the measured setup, the PC was `10.17.1.17` and the board `10.17.1.18`, with
the console on COM13 at 921600 baud. Adjust addresses and the serial port for
another setup. Prepare the CA before building; a new CA requires a matching
firmware rebuild.

```powershell
python scripts/https_probe_host.py prepare
python scripts/build_https_checkpoint.py --package
python scripts/https_probe_deploy.py build/https-cpu0-checkpoint/FIRMWARE.BIN --board 10.17.1.18
```

Wait for the frontend to boot, then confirm `https net` is available. The deploy
script confirms SD readback, not a successful frontend boot. Start the peer in a
separate terminal:

```powershell
python scripts/https_probe_host.py serve --bind 10.17.1.17
```

With SP1 mounted, run the valid case once at rest and once with video and USB
input active. Each run includes three-second baseline and recovery windows; a
tuned busy request takes about 13 seconds in total; the first build took about
26 seconds.

```powershell
python scripts/https_probe_host.py run --peer 10.17.1.17 --case valid --label idle
python scripts/https_probe_host.py run --peer 10.17.1.17 --case valid --label loaded --require-workload
python scripts/https_probe_host.py run --peer 10.17.1.17 --case hostname
python scripts/https_probe_host.py run --peer 10.17.1.17 --case untrusted
python scripts/https_probe_host.py run --peer 10.17.1.17 --case expired
```

The runner supplies fresh entropy and time, checks the response or rejection, and
writes JSON plus redacted UART logs. Check `health=1`, active capture/input, real
cache misses, timing validity, and error counters before accepting a loaded run.
The expired case advances supplied UTC by 400 days to reject the certificate
chain. To query the console, omit the leading colon from `--command`:

```powershell
python scripts/https_probe_host.py run --timeout 3 --command status
```

Finish by stopping the peer with Ctrl-C, restoring the selected normal firmware,
and restoring the local default build:

```powershell
python scripts/https_probe_deploy.py firmwares/firmware-F1.3.3b3/FIRMWARE.BIN --board 10.17.1.18
python scripts/build_https_checkpoint.py --restore-default-build
```

After the board boots, check its status and confirm `https net` is unknown.

## Standalone HTTP service checkpoint

The [saved report](validation/http-service-checkpoint-20261027/checkpoint-report.json)
records the exact ARM package and board cases. The package is an opt-in service
build on the unchanged F1.3.3b3 FPGA payload, SHA-256
`f5daec556274805970c5918caa74110292f055ae925b91a84d46776a9b864657`.
No FPGA build or Golden change was needed.

The service provides start/poll/read/status/cancel, DNS, required certificate
and hostname checks, RTC-based UTC, bounded body buffering, JSON POST bodies,
de-chunking, redirects, and retained replies. It does not yet parse JSON or
serve Apple requests. The [plan](README_HTTPS_JSON_PLAN.md) defines that work.

The board passed 17 recorded positive/expected-failure cases on this image,
including two loaded runs, HTTP, HTTPS, JSON POST, chunked replies, relative and
HTTP-to-HTTPS redirects, retained-body overflow, truncated replies, certificate
failures, and a 30-second response deadline. Separate logs cover cancellation,
missing DNS/seed state, and FTP denial of private seed paths.

Direct public HTTPS also passed with DNS 8.8.8.8 (the PC Ethernet adapter's
configured resolver), not the controlled PC DNS peer. The board retrieved
`https://www.cloudflare.com/favicon.ico`, status 200, 908 bytes, matching the
PC's verified download SHA-256. After reboot, seed generation advanced from
6 to 7 and the same request passed without sending entropy or time from the PC.

| Loaded measurement | Baseline / HTTPS / recovery |
| --- | --- |
| Run 1 capture frames | 144 / 341 / 145 over 3.002 / 7.132 / 3.020 seconds |
| Run 2 capture frames | 146 / 376 / 148 over 3.019 / 7.780 / 3.007 seconds |
| Run 1 longest USB1 service gap | 24.676 / 27.007 / 24.725 ms |
| Run 2 longest USB1 service gap | 24.750 / 26.290 / 24.745 ms |
| Run 1 mean SD read | 361 / 376 / 373 us |
| Run 2 mean SD read | 373 / 374 / 373 us |

Loaded ECDSA handshakes took 1.714 and 2.365 seconds; longest service steps were
4.016 and 3.920 ms. Both runs had mouse and Xbox reports in all phases and zero
capture gaps/oversized records/resyncs, USB errors, SD errors, or CRC mismatches.
This is the same 64-block read-only SmartPort-backend workload as before;
Apple-bus reads/writes, Disk II, USB0 transfer throughput, and physical HDMI
frame loss are not covered. The allocator's observed high-water was 43952
bytes within its fixed 512 KiB reservation; stack high-water is still unmeasured.

At that checkpoint, two latency gates remained before broad release. A successful cold public request
spent up to 25.451 ms in one setup step, including seed-file sync. An RSA TLS
request passed, but a step took 19.606 ms: upstream Mbed TLS enables restartable
TLS ECDH for ECDHE-ECDSA only. Bound or restrict the RSA path and move/split cold
setup before calling this a general latency-safe service. The 750 us scheduler
budget is checked between calls and is not a maximum call duration.

The first diagnostic image stalled inside TLS setup. Enabling SHA-384 introduced
64-bit context fields into a heap whose default alignment was four bytes.
Eight-byte allocation alignment fixed the board fault; a native odd-allocation
test now checks that requirement, alongside repeated TLS setup/free and public
CA parsing. HTTP/seed native tests and AMP, FTP, SmartPort, USB HID/Xbox, UART,
and firmware-memory checks passed. Both normal ARM builds were then restored
with diagnostic symbols absent. The board restore and SD cleanup evidence are
in the report; the test seed, trust, and configuration files were removed.

To reproduce, install the host-only dependencies from
`scripts/requirements-http-checkpoint.txt`, then use:

```powershell
python scripts/http_service_host.py prepare
python scripts/test_http_service.py
python scripts/test_http_tls.py
python scripts/build_https_checkpoint.py --service --package
python scripts/https_probe_deploy.py build/http-service-checkpoint/FIRMWARE.BIN --board 10.17.1.18 --output-dir build/http-service-checkpoint
# Wait for :http status to answer before provisioning.
python scripts/http_service_host.py serve
# In another terminal:
python scripts/http_service_host.py provision
python scripts/http_service_host.py run --case valid --bench --require-workload
python scripts/http_service_host.py run --case rsa
```

Provisioning uses one Windows CSPRNG seed per board, a selected CA bundle, and
the board RTC with an explicit UTC offset. It fails if the RTC is out of step.
The SD seed journal is private state; it has no hardware entropy, physical
protection, or rollback guarantee. Do not clone or restore it from backups.
Keep the controlled test CA out of a production trust bundle. See the
[plan](README_HTTPS_JSON_PLAN.md) for file layouts, limits, and the remaining
release gates. Before restoring
an older firmware, use `:http erase-test-provisioning`, remove test CA/config
files, and restore the exact stock image through the updater. Stop the owned
peer and run `build_https_checkpoint.py --service --restore-default-build`.

## Next work

The [28 October hands-on build](README_HTTPS_HANDS_ON.md) supersedes the
diagnostic-only workflow above for user testing. It includes the normal
firmware, libtini client, public STARFALL service, browser submission and
A2LYNX direct HTTPS. The later [RSA stress run](README_RSA_STRESS_CHECKPOINT.md) closes the agreed integrity gate while retaining the measured latency limits.

The [HTTPS and JSON plan](README_HTTPS_JSON_PLAN.md) replaces the earlier
CoGS Slot 1 plan. Appletini does not implement the CoGS card. The checkpoint
results above remain the evidence for the CPU0 TLS backend.

The [SmartPort transport checkpoint](README_NET_TRANSPORT_CHECKPOINT.md)
now passes on ONE//e with the unchanged F1.3.3b3 FPGA. It proves the diagnostic
byte transport and warm-reset recovery, with the stated TURBO compatibility
setting. The network byte contract is the next step.
