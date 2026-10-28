# HTTP SmartPort protocol, version 1

This is the production interface. The step-1 `ATNT` diagnostic does not
implement it. No FPGA or slot-ROM change is needed.

All numbers are little endian. Calls use slot 7, unit 0, a parameter count
of 3, and all nine parameter-list bytes. Private selectors are `$82` to `$86`.
The command FIFO holds at most 1024 bytes.

## CONTROL $82

The FIFO frame is command `$04`, nine list bytes, a 16-bit payload length,
then the payload. The payload is at most 1012 bytes, including this header:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Version, 1 |
| 1 | 1 | START=1, CANCEL=2, JSON=3 |
| 2 | 1 | START: STREAM=0, RETAIN=1; otherwise zero |
| 3 | 1 | START method length, 1..15; otherwise zero |
| 4 | 2 | START URL length or JSON Pointer length |
| 6 | 2 | START header-lines length; otherwise zero |
| 8 | 2 | START body length; otherwise zero |
| 10 | 2 | Reserved, zero |
| 12 | variable | START: method, URL, header lines, body; JSON: pointer |

Fields have no terminating NUL. Method, URL, and headers cannot contain NUL.
The byte counts must exactly match the payload length. CANCEL has exactly
12 bytes. A JSON Pointer has at most 1000 bytes. An empty pointer selects
the document root. A START replaces the earlier request and advances the
nonzero generation, wrapping 255 to 1. A successful CONTROL returns one
zero status byte. Invalid frames return SmartPort BADCTL (`$21`); a JSON
command while a lifecycle command awaits its poll returns BUSY (`$2F`).

The handler copies START and returns. The foreground poll cancels the old
backend and starts the new one. DNS, SD setup, and TLS never run in the
CONTROL handler. CANCEL drops readable data at once and queues cleanup.

## STATUS $82: 64-byte descriptor

The FIFO frame is command zero and nine list bytes. The response is status
zero, a 16-bit length, then the descriptor. Unsupported selectors return
BADCTL without a length or data.

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 4 | ASCII `ATNH` |
| 4 | 1 | Version 1 |
| 5 | 1 | Descriptor size, 64 |
| 6 | 1 | Capabilities: HTTPS=1, JSON=2, STREAM=4, RETAIN=8 |
| 7 | 1 | Generation; zero means no request |
| 8 | 1 | IDLE=0, QUEUED=1, ACTIVE=2, DONE=3, FAILED=4, CANCELLED=5 |
| 9 | 1 | RETAIN flag |
| 10 | 2 | Signed backend result; zero on success |
| 12 | 2 | HTTP status, zero until received |
| 14 | 1 | JSON result, below |
| 15 | 1 | JSON value type, below |
| 16 | 4 | Decoded HTTP body bytes received |
| 20 | 4 | Body bytes in the backend buffer |
| 24 | 4 | JSON value length |
| 28 | 2 | Final URL length |
| 30 | 2 | Content-Type length, at most 127 |
| 32 | 4 | Longest backend step, microseconds |
| 36 | 4 | Longest backend poll slice, microseconds |
| 40 | 4 | TLS verification flags |
| 44 | 4 | VIA Hz for the selected ONE//e profile; zero if unknown |
| 48 | 16 | Reserved, zero |

ONE//e's 133333333 Hz virtual bus gives 1017812 Hz for PAL (131 clocks)
and 1025641 Hz for NTSC (130 clocks), rounded to an integer. PAL was checked
against the ARM timer in the [clock study](README_STARFALL_IRQ_CLOCK.md).
The physical Apple clock is not inferred from this number: its value is zero.

Backend results include BUSY=-100, ARGUMENT=-101, IO=-102, TIME=-103,
SEED=-104, TRUST=-105, DNS=-106, TIMEOUT=-107, CANCELLED=-108,
REDIRECT=-109, STORAGE_BUSY=-110, NETWORK_NOT_READY=-111.
NETWORK_NOT_READY means DNS or TCP connection setup does not finish within
the 30-second request deadline. During DNS wait the engine reads
`Sn_IR[TIMEOUT]`, clears it, and sends the query again. It also retries DNS
after 3 seconds without a reply. TCP connect retries wait 200 ms before
opening the socket again. These waits yield to the foreground loop and
never extend the request deadline. This covers switches that delay forwarding
after link-up and lost DNS packets. It does not prove a local link fault:
TCP connect can also time out when a SYN receives no reply. A negative or
invalid DNS response still returns DNS. A timeout after TCP connect still
returns TIMEOUT. Connection retries send no HTTP data and never replay a POST.
Parser and Mbed TLS errors retain their
negative codes. HTTP error status codes remain successful HTTP transfers;
applications must inspect the HTTP status.

JSON results: NONE=0, PENDING=1, FOUND=2, NO_DOCUMENT=3,
BAD_DOCUMENT=4, BAD_PATH=5, MISSING=6. Value types: STRING=1,
NUMBER=2, BOOLEAN=3, NULL=4, OBJECT=5, ARRAY=6. Invalid JSON does not
turn an otherwise valid HTTP response into a transport error.

## STATUS $83..$86: offset reads

Selectors: BODY=`$83`, JSON VALUE=`$84`, FINAL URL=`$85`, CONTENT TYPE=`$86`.
List bytes 5..6 hold the requested 16-bit offset; 7..8 hold the largest
accepted reply, 9..512 bytes. The response has the usual SmartPort status
and 16-bit length, followed by this reply:

| Offset | Size | Value |
| --- | --- | --- |
| 0 | 1 | Request generation |
| 1 | 1 | MORE=1, END=2, FAILED=3, LOST=4 |
| 2 | 4 | Requested offset, zero extended |
| 6 | 2 | Number of following data bytes |
| 8 | variable | At most requested capacity minus 8 bytes |

END means this reply reaches the end of the source. MORE with no bytes
means poll again. Do not interpret an empty MORE reply as EOF. FAILED
means inspect the descriptor. LOST means the requested offset is outside
the retained source or the STREAM replay range. Read offsets cannot exceed
65535 in this version; applications stop before wrapping that field.

RETAIN holds at most 32768 decoded body bytes. Reads do not consume them.
STREAM has a 4096-byte ring. The service keeps the last reply (at most 504
bytes) for replay. Reading within it returns its suffix; reading just after
it releases it and consumes the next ring bytes. Older or skipped offsets
return LOST. The generation lets the client reject an old reply.

Strings contain no terminating NUL. The final URL and Content-Type become
final on DONE. JSON strings return decoded UTF-8, including possible NUL
bytes; scalar literals and object/array source bytes retain their JSON form.
JSON parsing accepts UTF-8, validates surrogate pairs and number grammar,
and limits nested containers to 16. Lookup uses JSON Pointer escapes `~0`
and `~1`, decimal array indexes without leading zero, and the first duplicate
object member. It runs in bounded foreground slices.

## Lifetime and discovery

Clients identify Appletini's slot-7 ROM before sending a private request,
then require the descriptor signature, size, version and capabilities.
libtini uses the known C700 CRC-16/CCITT-FALSE `$95D9`; older firmware
returns BADCTL and the application continues offline. Each request has one
owner. Reset discards data and queues socket cleanup. USB sharing blocks
new work and clears old data. FTP and HTTP take Ethernet one at a time.

IRQ masking covers only frame submission and reply copying. The RAM copy
loop avoids C8 ROM execution between data writes. Callers use their own
reply area; STARFALL uses 128 bytes. A timeout or oversized frame leaves
the client poisoned for the rest of the program: it must not replay a POST
or consume an unknown late response.

Host checks: `python scripts/test_net_service.py` compiles the production
adapter and JSON parser. SDK tests compile the actual cc65 client and run
it against a controller model. Hardware acceptance is recorded separately.
