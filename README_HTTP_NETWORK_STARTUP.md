# HTTP network startup retries

The switch caused the 30-second frame blackout after boot. The operator
has set GigabitEthernet1/0/3 as an edge port. The delay came from spanning
tree, not the W5100S or the HTTP engine. The firmware keeps bounded retries
for other networks where users cannot change the switch.

The HTTP engine now reads `Sn_IR` during DNS wait. It acknowledges TIMEOUT
and queues the same DNS query again. It also retries a query after 3 seconds
without a reply. During TCP connection setup it acknowledges TIMEOUT,
closes the socket, and retries after a 200 ms foreground wait. A closed
socket during connection setup takes the same path.

All attempts share the original 30-second request deadline. DNS or TCP
setup that reaches it returns `HTTP_E_NETWORK_NOT_READY` (-111). The
SmartPort descriptor carries that signed result, and libtini names it
`TI_NET_RESULT_NETWORK_NOT_READY`. A2LYNX shows "Network not ready; try again".
The foreground waits leave the CPU free for other services.

A missing DNS setting or a negative or invalid DNS response still returns
DNS (-106). A failed register access still returns IO (-102). Timeouts
after TCP connects keep TIMEOUT (-107). Retries only set up the connection;
they do not replay HTTP bytes, POST data or a failed TLS handshake.

## Validation on 28 October 2026

The same development ARM image passed these tests on the W5100S board:

| Case | Result |
| --- | --- |
| First controlled DNS and HTTPS GET after reboot | Eight ARP retries; verified HTTP 200 after 14.518 s |
| Repeat without reboot | Verified HTTP 200 after 0.639 s |
| Public HTTPS through DNS 8.8.8.8 | Verified HTTP 200 after 0.611 s |
| Controlled server drops all DNS replies | Repeated queries; -111 at 30.005 s |
| Actual Apple libtini POST, then JSON Pointer lookup | HTTP 200, 43-byte body, 18-byte decoded value; TLS verification flags zero |

The JSON slices reached 2 microseconds in the Apple test. These runs use
UART trace and do not establish video, audio, USB or storage performance.
The first request's longest step was 26.436 ms, which includes first-use
setup. The full HTTPS plan retains its workload and release gates.

## Edge-port test on 28 October 2026

After the operator set GigabitEthernet1/0/3 as an edge port, the same firmware
passed the first DNS and HTTPS request after a normal reboot. The test first
creates its seed and trust files, then reboots with those files on SD. This
keeps first-use provisioning out of the normal boot measurement.

The runner uses `--settle 0`. It now waits for a working frontend UART
status, with no fixed 15-second reboot read or hidden 10-second pause.
It starts the request 218 ms after that status response.

| Case | Result |
| --- | --- |
| First DNS and verified HTTPS request after reboot | HTTP 200 in 279.056 ms; zero ARP retries |
| Repeat without reboot | HTTP 200 in 165.187 ms |
| Public HTTPS through DNS 8.8.8.8 | HTTP 200 in 1.217 s |
| Controlled server drops all DNS replies | Nine retries; -111 at 30.005 s |
| Apple libtini POST and JSON lookup | HTTP 200; 18-byte decoded value |

The HTTP engine source and test firmware are unchanged from the original
retry test. DNS and TCP retries keep their 30-second limit for networks
whose switches still delay forwarding. This early boot run measured a
99.169 ms longest setup step; video, audio, USB and storage workload tests
remain part of the full plan.

The stock firmware receipt and original settings passed readback checks.
The runner removed its test files and seed, disabled FTP and rebooted to
the original media. The [edge-port record](validation/http-edge-port-20261028/summary.json)
holds the times and artifact hashes. The earlier forwarding-delay record
stays in `validation/http-network-retry-20261028/`.

Native tests compile the production engine with a W5100S register model.
They cover repeated ARP timeouts, a lost DNS reply, a reply at 29 seconds,
TCP connection retries and their backoff, cancellation, register I/O
failure, deadline cleanup and no POST replay. SDK CPU tests cover signed
-111 propagation and all reply sizes from 9 to 512 bytes.

The board test also found a libtini placement defect: cc65 assigned two
persistent flags to the later `TINETBSS` segment despite their earlier C
declarations. That scratch segment does not receive startup clearing.
Assembly now places the flags in `NEARBSS`, which startup clears. The CPU
tests check their addresses and start with dirty near and scratch RAM.
STARFALL and the self-contained A2LYNX transport use this correction.

The test keeps the F1.3.3b3 FPGA payload and Golden B2.0.6. The collector
saves the completed run only after it verifies the stock firmware receipt
and exact original settings. Evidence goes in
`validation/http-network-retry-20261028/`.

Commands:

```text
python scripts/test_http_network.py
python scripts/test_net_service.py
python scripts/test_http_service.py
python scripts/build_https_checkpoint.py --service --application --package
python scripts/run_http_api_checkpoint.py --require-dns --check-network-deadline
python scripts/collect_http_retry_checkpoint.py

# Test normal startup after first-use setup, with no extra boot wait:
python scripts/run_http_api_checkpoint.py --settle 0 --reboot-provisioned --require-dns --check-network-deadline
python scripts/collect_http_retry_checkpoint.py --output validation/http-edge-port-20261028 --expect-early-dns
```
