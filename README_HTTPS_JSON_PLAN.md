# HTTPS and JSON plan

Plan agreed on 27 October 2026 and changed on 28 October 2026 after an
independent review. It replaces the CoGS Slot 1 plan. Appletini does not
implement the CoGS card or its protocol.

The required work is complete for F1.3.3 and Software 1.1.0. The normal
release image passes the board updater, version and public HTTPS checks.
The user accepts STARFALL gameplay and its public leaderboard. A2LYNX
passes direct HTTPS and stock-firmware fallback tests. The combined
RSA/video/USB/storage test passes at 33 MHz and TURBO under the agreed
integrity criterion. See the [release notes](README_RELEASE_F1.3.3.md)
for setup and the measured stutter limits. Shared IP65/HTTP ownership
in optional step 9 stays outside this release.

## Goals

1. **A2LYNX fetches HTTPS pages directly.** It stays a text browser with its
   current renderer. FrogFind search and page simplification stay available;
   A2LYNX no longer needs FrogFind for TLS.
2. **libtini has an HTTP/S and JSON API.** Games and tools can start a request,
   poll it, read the body, and extract JSON values.
3. **STARFALL has online rankings.** A finished run can submit its score, and
   the game can show the ranking table.

None of this needs an FPGA change. The ARM firmware does all the network work,
and the Apple reaches it through the slot 7 SmartPort controller that Appletini
already has. The [CPU0 HTTPS checkpoint](README_HTTPS_CHECKPOINT.md) holds the
hardware evidence for the backend.

## Design

### Transport: a private SmartPort selector

libtini already talks to the ARM through private SmartPort calls on slot 7,
unit 0. STATUS and CONTROL code `$80` is the AMEM memory service and `$81` is
the glyph service; `smartport_service.c` sends each one to its handler. The
network service uses the same path:

| Call | Purpose |
| --- | --- |
| CONTROL `$82` START | Method, URL, header lines, body, and the mode (RETAIN or STREAM). Replaces any earlier request, then returns. |
| CONTROL `$82` CANCEL | Stops the request, closes its sockets, and drops its bytes. |
| CONTROL `$82` JSON | Starts a lookup of a JSON Pointer in the retained body. |
| STATUS `$82` | Descriptor of at most 64 bytes: signature, version, capability flags, generation, request state, result code, HTTP status, body counts, JSON state, and the lengths of the final URL, the Content-Type value, and the JSON value. |
| STATUS `$83` | Body bytes. |
| STATUS `$84` | JSON value bytes. |
| STATUS `$85` | The final URL after redirects. |
| STATUS `$86` | The Content-Type value, up to 127 bytes. |

The [version 1 protocol](README_HTTP_SMARTPORT_PROTOCOL.md) freezes the byte
layout, with fixtures and host tests.

STREAM passes the body through the backend's 4096-byte ring; A2LYNX uses it.
RETAIN keeps one decoded body of up to 32768 bytes; JSON and STARFALL use it.

**Reads take an offset and a size.** For every call, the slot 7 ROM sends nine
parameter bytes from the caller's list, and the libtini frame also sends nine
(today its last four are zero). A read from `$83` to `$86` uses those last four
bytes: the low 16 bits of the wanted offset, and the largest reply the caller
accepts, from 9 to 512 bytes. A reply starts with an 8-byte header
(generation, source state, 32-bit offset, data length), so it carries at most
504 data bytes. A reply is never larger than the caller's size, so STARFALL can
read into its 128-byte transfer area.

**Reads keep no cursor on the ARM.** A read at any offset works for the
retained body, the JSON value, and the two strings. In STREAM mode the ARM keeps
the bytes of the last reply. The next read can start at any point in those
bytes or just after them. A read past them releases them. A read at an
earlier offset gives the LOST state. Thus a caller can read a lost reply again,
and no SEEK call is necessary.

**The source state in each reply tells the caller what to do next:** MORE
(more bytes can come; zero data bytes means none yet), END (the source has no
more bytes), FAILED (the descriptor holds the result code), or LOST.

**Every call returns at once and the 6502 polls.** A loaded HTTPS handshake
takes 1.7 to 2.4 seconds. The slot 7 ROM waits without a limit, and libtini
gives up on a reply after 2 seconds, so no call may wait for the network. After
a libtini timeout the network driver stops for the rest of the program, as AMEM
does, because the late reply can still arrive. The program then continues
offline.

**Calls hold the 6502 with IRQs masked.** libtini masks IRQs from the request
until it has copied the reply. The ROM copy loop takes 19 cycles per byte, so a
512-byte reply takes about 9.5 ms at 1 MHz. AMEM reads its short replies one
byte at a time in C, which is slower; the network transport copies replies in
assembly. Programs that play audio use a small reply size.

The [VIA calibration and STARFALL IRQ check](README_STARFALL_IRQ_CLOCK.md)
measures about 0.9825 microseconds per tick on the tested PAL ONE//e profile.
Use the raw RAM FIFO path and STARFALL's 128-byte transfer area. A 512-byte
ROM reply crossed the 100 Hz audio period and lost ticks at the 26 MHz
preset. Raw 128–512-byte replies preserved ticks at that preset and TURBO,
but the probe affected frame rate near the lower speed. Step 7 still needs
an API integration test with little probe overhead and an audible check.
Supply the board profile's calibrated `via_hz` when starting audio; do not
assume a physical 1 MHz timer or use full-game 1 MHz performance as a gate.

### Request lifecycle

- One request runs at a time. START cancels any earlier request, active or
  finished, drops its bytes and JSON value, and takes the next generation
  number. Generation 0 means no request. Thus a program that stops during a
  request cannot block the next program.
- The descriptor and every read reply carry the generation, so a caller can
  find a reply that belongs to an earlier request.
- A request ends as DONE, FAILED, or CANCELLED. Its descriptor stays until the
  next START. After DONE, its body, JSON value, and strings also stay. FAILED
  and CANCEL drop the bytes; the backend already drops a retained body when a
  request fails.
- An Apple reset, a slot 7 reset, and the start of USB storage sharing cancel
  an active request and drop all its bytes. USB storage sharing also blocks new
  requests while it is active. The service reads the same reset generation that
  AMEM uses.
- A JSON call replaces the earlier JSON value. It does not change body reads,
  because body reads have no cursor.

### Backend

The CPU0 service in `http_service.c` already does the network work. It has
start, poll, read, status, and cancel calls, DNS, redirects, chunked bodies,
JSON POST bodies, and retained bodies. Its status record keeps the final URL
after redirects; its parser also keeps the Content-Type value. CPU1
stays on capture and video. CPU0 runs the network service cooperatively: the
scheduler checks a 750 us budget between calls, with at most 32 TLS steps,
eight socket callbacks, and 512 bytes per callback in each poll. A single
crypto or SD call can take longer than 750 us.

TLS is Mbed TLS 3.6.7, TLS 1.2, ECDHE-ECDSA or RSA with AES-128-GCM, on P-256
or P-384. Its allocator has a fixed 512 KiB pool with 8-byte alignment. Chain,
hostname, and time checks are always on; there is no insecure fallback.

The network menu has a saved static DNS field. DHCP keeps option 6. The
service uses that DNS server unless `ATNET.CFG` supplies a nonzero override.
DNS and TCP setup retry within the request deadline so that link-up can
precede switch forwarding. See the retry limits below.

### Request and response limits

These limits come from the existing backend and the command frame. Step 2
records them in the contract and tests each one.

- **A START payload has at most 1012 bytes, all fields included.** The command
  frame holds 1024 bytes and its header takes 12. The method, URL, header
  lines, body, and request fields share the 1012 bytes. This is the public
  limit, and libtini refuses a larger request before it sends anything. The
  larger backend limits (a 1023-byte URL, and 2048 bytes each for header lines
  and body) therefore never apply. A STARFALL submission needs only a few
  hundred bytes.
- Method: up to 15 bytes.
- Bodies may use Content-Length, chunked transfer (with trailers), or end at
  connection close.
- Up to 4095 bytes per response line, 16384 header bytes per header or
  trailer section, four interim responses, and three redirects.
- A 30-second deadline per request, setup included. Read and clear W5100S
  `Sn_IR[TIMEOUT]` during DNS wait, then resend the query. Also resend after
  3 seconds without a DNS reply. Retry TCP connection setup after a 200 ms
  foreground wait. Keep the original deadline through all attempts. Return
  `NETWORK_NOT_READY` (-111) if DNS or TCP setup reaches that deadline.
  Never retry HTTP data after a connection starts sending it.
- Redirects resolve relative paths, drop credentials when the origin changes,
  and refuse a change from HTTPS to HTTP. 301, 302, and 303 turn POST into
  GET; 307 and 308 keep the method and body.
- No IPv6, URL credentials, compression, WebSockets, HTTP/2, or connection
  reuse.

### JSON

Add a strict JSON parser on the ARM, with host tests:

- UTF-8 only, and a depth limit of 16.
- The service checks the retained body as it arrives, in the poll loop, within
  the byte limit for each callback. So the service knows whether a complete
  body is valid before any lookup.
- Paths use [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901), for
  example `/scores/0/name`. In a member name, `~1` stands for `/` and `~0` for
  `~`. The empty path selects the root. Array indexes are decimal, with no
  leading zero. Member names match byte for byte, and the first of two equal
  names wins.
- The JSON call returns at once. The lookup runs in the poll loop in limited
  slices, as HTTP work does, and the descriptor shows its state.
- A string gives its decoded UTF-8 text. A number, boolean, or null gives its
  literal text. An object or array gives its source JSON. The descriptor gives
  the value type and length, and STATUS `$84` reads the bytes.
- Results: FOUND; NO_DOCUMENT (no complete retained body); BAD_DOCUMENT (the
  body is not valid JSON); BAD_PATH (the pointer syntax is wrong); MISSING (a
  member or index does not exist, or the path goes through a value that is not
  an object or array).

### Ethernet ownership

Today a request takes the whole W5100S: `claim()` in `http_service.c` turns
slot 1 off, closes all four sockets, and uses sockets 0 and 1. None of the
three goals needs an Apple program to drive Uthernet II while a request runs,
because A2LYNX stops starting IP65 when the ARM service is present. So the
three goals can keep this rule.

The W5100 Shared Access convention (see the links below) lets both run at
once. Step 9 adopts it:

- The ARM uses only sockets 2 and 3 (1 KiB each), leaves slot 1 on, and never
  closes sockets 0 or 1.
- The ARM owns the settings for the whole chip: MAC, IP address, gateway,
  subnet, and memory sizes. Apple programs read them and do not write them
  while the chip is shared.
- `appletini_net.c` closes only sockets 0 and 1, and keeps the chip's MAC and
  memory sizes. IP65 already skips the chip reset when it finds RMSR = `$06`.
- `uthernet2_control.c` tries again, up to a fixed count, when an Apple access
  stops an ARM access. A new try is safe: the card stops an access only before
  the data cycle, and it marks the address pointer stale, so the next try
  loads it again. After the last try the request fails with an I/O result. The
  ARM never waits for the Apple in a loop.
- FTP moves from sockets 0 and 1 to 2 and 3, and runs one at a time with HTTP,
  as it does now.

## Board state

- `ATNET.CFG` holds the signed RTC UTC offset and an optional DNS override.
  After step 3, the normal DNS server comes from the network settings.
- `ATNETCA.PEM` holds the trusted roots, up to 65567 bytes. The parsed roots
  stay in the 512 KiB TLS pool for every later handshake.
- Two private seed records hold the random-number state. A new seed is written,
  synced, and read back before any random output reaches the network. Broken or
  missing records fail closed. FTP refuses the seed paths and their aliases.
- Never clone a seed record to another board, or restore an old one from a
  backup. The scheme has no hardware entropy, no physical protection on the SD
  card, and no rollback check.

## First-use and service choices

1. `scripts/setup_https.py` writes two private seed records from 48 bytes
   from the PC's OS random source. Run it once per board with that board's
   SD card. It keeps existing records. Do not clone or roll back a seed.
2. `software/https_setup/ATNETCA.PEM` contains a pinned set of 26 public
   roots, 33175 PEM bytes. It excludes the controlled test CA. A chain whose
   issuer is outside this set fails verification. The live board used up
   to 96128 bytes of the fixed 524288-byte TLS pool in the acceptance run.
3. Set the RTC in the menu and give setup its local-time-minus-UTC offset.
   No SNTP service is part of this checkpoint. DNS uses the Ethernet menu
   or DHCP unless `ATNET.CFG` gives an explicit override.
4. The public service is `https://appletini.org/leaderboards/starfall`, on
   Cloudflare Workers and D1. It validates plausible scores, limits requests,
   uses server UTC and ranks each name by its best run. A game client has no
   secret score-signing key. Every POST requires `platform`; the card sends
   `appletini` and the browser sends `web`. A repeated run ID must carry the
   exact same data.

Seed rotation, file sync and root parsing now run before Apple release.
The normal request path uses cached state and does not touch SD for setup.
The application build contains the service and omits test provisioning,
trace and register commands. See [current acceptance](README_HTTPS_HANDS_ON.md).
RSA verification still has synchronous steps of about 24 to 28 ms on the
board. The scheduler's 750 us budget does not bound those calls. The user
accepts stutter if data stays intact. The two-speed RSA, moving-video, USB
and verified SmartPort/Disk II write stress run passes that release criterion.
See [the results](README_RSA_STRESS_CHECKPOINT.md).

## Steps

Steps 1, 3, and 4 prove the transport first, then one HTTPS GET, then JSON.

| Step | Work | Exit check |
| --- | --- | --- |
| 0. Remove CoGS | Done on 28 October 2026. The CoGS card and its hooks are gone (list below). The two timing fixes from 27 October in `onee_mode_safety_guard.sv` and `apple_top.sv` stay for the next FPGA release. | 135 of 177 regressions pass, the Uthernet II, SSC, SmartPort, UART help, HTTP/S, ONE//e guard, and vTW tests among them. The other 42 fail the same way on the committed tree, or need a tool that this PC does not have on its path (a native C or C++ compiler, Verilator, or the 65C02 test vectors). The network work runs on the current F1.3.3b3 FPGA payload; no FPGA build is needed. |
| 1. Transport checkpoint | Done on ONE//e on 28 October 2026, using the unchanged F1.3.3b3 FPGA. Diagnostic STATUS `$82` and `$83`, Apple client, host tests, build tools, and [results](README_NET_TRANSPORT_CHECKPOINT.md) are in place. | Both ROM and raw FIFO paths passed 10,000 reads each at 1 MHz and TURBO, covering 9 to 512 bytes, repeated offsets, and interleaved SmartPort/Disk II writes and readbacks. Per-size call and IRQ interval bounds are recorded. A warm Apple reset with 256 bytes unread cleared the FIFO and service; a fresh client passed recovery. TURBO uses built-in slot-6 slowdown, mask `$028`, 256 ticks, with Disk II acceleration off. Unthrottled TURBO Disk II writes also failed on stock F1.3.3b3. The cause was the ARM sector decoder, which commit `ca8cf1f` fixes ([details](README_DISK2_ADDRESS_EPILOGUE_FIX.md)). External Apple bus pins remain untested. |
| 2. Freeze the contract | Write the byte layout of every call, the results, the lifecycle, and the limits, with fixtures. Add a 65C02 and controller model test, as the libtini AMEM check has. Define how libtini finds the service. | Fixtures and host tests exist for every call, result, and lifecycle rule. |
| 3. ARM service and HTTPS GET | Add the handler over `http_service.c`: START, CANCEL, the descriptor, and the body, final URL, and Content-Type reads. Keep the Content-Type value in the parser. Take DNS from the network settings: keep the DNS server from DHCP, and add a DNS field to the static settings, their saved file, and the menu. | Host tests pass. On the board, the Apple test program fetches a public HTTPS page in STREAM and RETAIN modes, and does a POST. |
| 4. JSON | Add the parser, the check during receipt, the JSON Pointer lookup in slices, and STATUS `$84`. | Host tests pass for every result. On the board, the test program extracts values from a retained reply. No JSON slice uses more than the 750 us scheduler budget. |
| 5. Backend release gates | Settle the four decisions. Bound the slow steps: an RSA TLS step took 19.606 ms, and a cold setup step took 25.451 ms with the seed sync. Budget the root set and the TLS pool together. | After the first-use setup, public HTTPS works with no PC connected. Stutter is allowed under heavy TLS. The combined RSA/video/USB/write run must preserve file contents, filesystem structure and capture integrity, with no reported USB errors and successful recovery. The pool high-water mark with the chosen roots is recorded and stays below the pool size. |
| 6. libtini API | Add `ti_net.h` with start, poll, read, cancel, and JSON calls. Callers pass their own buffers of 9 to 512 bytes and get explicit errors. Update the SDK reference and add an example. | The example posts JSON, reads the reply, and extracts values on hardware at 33 MHz and in TURBO. Builds without networking still work. |
| 7. STARFALL rankings | Add the service and the game client: player name, game version, score, sector, and kills, with a server timestamp and a run ID that makes retries safe. Show the ranking table. Recheck STARFALL's memory layout first: reads use the 128-byte transfer area, and nothing large goes on the 256-byte C stack. | A real board submits a run, gets its rank, and shows the table. Retries do not duplicate, and network failures never stop offline play. With audio on, the time with IRQs masked is recorded for each call, and audio and game timing stay correct. |
| 8. A2LYNX HTTPS | Fetch every page through the ARM service. The ARM follows redirects. A2LYNX uses the final URL as the base for relative links, and shows an error when that URL does not fit its 128-byte buffer. Replace the code that splits headers from the body with a path that gets only the body. Read the Content-Type value and refuse types that are not text or HTML. A2LYNX already shows one `?` for each non-ASCII UTF-8 character, so it needs no charset work. FrogFind search and simplification stay on all firmware; with the service, A2LYNX reaches FrogFind through the ARM. IP65 stays only for firmware without the service. | The board shows verified HTTPS text pages and follows relative links after a redirect, without a proxy. It fails clearly on bad certificates and on types that are not text. A page longer than the 10 KiB buffer shows its first 10 KiB and a note that the page was cut. |
| 9. Shared W5100S access (optional) | Apply the convention above. | A bench test on the W5100S shows that IP65 in MACRAW mode and an HTTPS request run together, and that neither side resets the TCP connections of the other. |
| 10. Validate and package | Run the release tests below and build the exact release packages. | The libtini example, STARFALL rankings, and A2LYNX HTTPS pass on the board with recorded provenance. Normal builds contain no test CA or probe commands. |

### CoGS files removed in step 0

- Deleted files: `hdl/apple/cogs_slot1_card.sv`,
  `hdl/sim/tb_cogs_slot1_card.sv`, `ps_sources/frontend/cogs_service.c`,
  `ps_sources/frontend/cogs_service.h`, `scripts/build_cogs_arm.py`,
  `scripts/build_cogs_checkpoint.tcl`, `scripts/cogs_slot1_smoke.py`,
  `scripts/test_cogs_service.py`, `scripts/test_cogs_slot1.py`,
  `scripts/fixtures/cogs_http_subset.json`,
  `scripts/fixtures/cogs_service_host.c`, and
  `validation/cogs-slot1-checkpoint-20261027/`.
- CoGS lines removed from: `hdl/apple/apple_top.sv`, `hdl/hdl_sources.txt`,
  `ps_sources/frontend/card_control_regs.h`, `ps_sources/frontend/main.c`,
  `ps_sources/frontend/uart_control.c`, `scripts/create_vitis_workspace.py`,
  and `scripts/test_uthernet2_card.py`. The other work in these files stays.
  Card control registers `0xB0` to `0xBA` are free again.
- The HTTP/S validation records from 27 October still name CoGS files in
  their source lists. They stay as written, because they record what was
  tested then.

## Release tests

- Wrong hostname, unknown issuer, expired certificate, missing or wrong time,
  missing seed, DNS failure, cable removal, server timeout, cut-off replies,
  bad HTTP or JSON, and full buffers.
- Reads at every reply size, second reads at the same offset, LOST reads, and
  replies from an earlier generation.
- Repeated requests, redirects, retained-body replacement, cancellation, slot
  7 reset, Apple reset, power-on, and FTP or USB storage activity during a
  request. Check cleanup, stack high-water, and memory use over a long run.
- Repeat the video, mouse, Xbox, and SD measurements from the checkpoint, and
  add Apple-bus SmartPort reads and writes, Disk II traffic, and USB0 transfers.
  Record the SmartPort call time and the time with IRQs masked.
- Uthernet II programs and the slot 1 printer still work when no request runs.
- Release only the exact package tested on the board, through the normal
  updater, with a checked restore image. This plan does not change Golden.

## Not in scope

The CoGS card and protocol, Wi-Fi, raw TCP or TLS sockets for Apple programs,
uploads larger than one START payload (until an application needs them),
network disks and network boot, audio streaming, JavaScript or modern page
layout, and real-time multiplayer.

## References

- [W5100 Shared Access](https://github.com/a2retrosystems/uthernet2/wiki/W5100-Shared-Access)
- [IP65 W5100 driver at the pinned commit](https://github.com/cc65/ip65/blob/25a9c5aa9480ddab0e506108ec7f1ce538165d28/drivers/w5100.s)
- [JSON Pointer, RFC 6901](https://www.rfc-editor.org/rfc/rfc6901)
- libtini AMEM private transport: `libtini/design/AMEM.md` in the
  appletini-software repository.
