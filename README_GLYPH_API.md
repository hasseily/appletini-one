# Appletini character glyph API 1.0

Apple II programs can replace the character ROM used by Appletini's rendered
video through SmartPort selector `$81`. This firmware service accepts a full
4096-byte ROM in chunks, checks its CRC, and applies it between frames. It
works with the motherboard CPU and vTW; it does not require vTW DMA or a CPU
hold. The existing `$80` copy/fill API keeps its format and behavior.

The API changes Appletini video output. It does not change the motherboard's
physical character ROM. The implementation uses ARM firmware and the existing
SmartPort transport; it adds no HDL or slot-ROM changes.

## Calling sequence

1. Identify the Appletini SmartPort controller in slot 7.
2. Send **STATUS (`$00`), controller unit 0, selector `$81`** to a 32-byte
   buffer. Check `AGLY`, major version 1, header size 16, ROM size 4096,
   maximum chunk size at least the chunk you will use, and AVAILABLE.
3. Wait until PENDING clears. Save the applied generation from STATUS.
4. Send **CONTROL (`$04`), unit 0, selector `$81`** with BEGIN and a nonzero
   transaction ID. Use that ID for all WRITE, COMMIT, and ABORT calls.
5. WRITE every byte of the ROM. Eight 512-byte chunks cover the image.
6. COMMIT with the CRC32 of all 4096 bytes, in address order. Success queues
   the complete ROM. Partial uploads never change the active ROM.
7. Poll STATUS until PENDING clears, the applied generation changes, and
   CUSTOM_ACTIVE is set. The renderer has now applied a software ROM.
8. To leave the demo, ABORT any unfinished upload, then RESTORE with
   transaction ID zero. Wait for PENDING to clear and CUSTOM_ACTIVE to clear.

Use the normal SmartPort entry with a `JSR`, followed by the command byte and
two-byte parameter-list pointer. The slot-ROM SmartPort entry offset is
`$CnFF + 3`, within page `$Cn00`. On the current slot-7 ROM it is `$C70D`.
Success returns `A=$00, C=0`; failure returns `A=error, C=1`. Treat X and Y as
clobbered. Older firmware returns `BADCTL=$21` for this selector. Do not send
private selectors to an unrelated SmartPort controller.

Both calls use the following parameter list. All multibyte fields in this
document use little endian.

| Offset | Bytes | Value |
|---|---:|---|
| 0 | 1 | Parameter count: 3 |
| 1 | 1 | Controller unit: 0 |
| 2 | 2 | CPU pointer to STATUS output or CONTROL input |
| 4 | 1 | Selector: `$81` |
| 5 | 4 | Zero padding; the current slot ROM streams nine parameter bytes |

The buffer pointer follows the caller's memory mapping. Keep the code, stack,
parameter list, and buffers in memory that survives each call. The normal
slot-ROM entry uses `$07F8` as workspace under that mapping; preserve it if
your program needs its prior value. The API does not change Apple memory
softswitches or the primary/alternate character-set selection.

## STATUS block

STATUS always returns 32 bytes. It does not clear the last CONTROL result.
Flags, generation, and upload state belong to the whole controller.

| Offset | Bytes | Meaning |
|---|---:|---|
| 0 | 4 | ASCII `AGLY` |
| 4 | 1 | Major version: 1 |
| 5 | 1 | Minor version: 0 |
| 6 | 1 | CONTROL header size: 16 |
| 7 | 1 | Reserved: zero |
| 8 | 2 | Flags below |
| 10 | 2 | ROM size: 4096 |
| 12 | 2 | Maximum WRITE data size: 512 |
| 14 | 1 | Last CONTROL result |
| 15 | 1 | Last CONTROL operation; zero after reset or when the fixed header fails validation |
| 16 | 4 | Current upload transaction ID; zero when no upload exists |
| 20 | 2 | Number of distinct ROM bytes written in that upload |
| 22 | 2 | Reserved: zero |
| 24 | 4 | Last generation applied by the renderer |
| 28 | 4 | Reserved: zero |

| Flag | Value | Meaning |
|---|---:|---|
| AVAILABLE | `$0001` | Firmware initialized the ROM service |
| CUSTOM_ACTIVE | `$0002` | The renderer currently uses a software-uploaded ROM |
| PENDING | `$0004` | A complete ROM change awaits the renderer |
| UPLOADING | `$0008` | A BEGIN transaction exists |

CUSTOM_ACTIVE describes the applied ROM. An SD ROM selected in the menu does
not set this flag. COMMIT success can therefore return with PENDING set and
CUSTOM_ACTIVE still clear. The applied generation changes after the renderer
copies the whole image and rebuilds its text tables. Compare generations for
inequality using unsigned values; do not assume a specific starting value.
Menu changes also advance the generation.

## CONTROL buffer

The caller buffer starts with a two-byte payload length which **excludes the
length word**. The header below begins after that word. A full WRITE occupies
530 bytes including the length word: `2 + 16 + 512`. BEGIN, ABORT, and RESTORE
use payload length 16; COMMIT uses 20.

| Payload offset | Bytes | Value |
|---|---:|---|
| 0 | 4 | ASCII `AGLY` |
| 4 | 1 | Major version: 1 |
| 5 | 1 | Operation below |
| 6 | 1 | Flags: zero |
| 7 | 1 | Reserved: zero |
| 8 | 4 | Transaction ID |
| 12 | 2 | WRITE byte offset, otherwise zero |
| 14 | 2 | WRITE byte count, otherwise zero |
| 16 | Variable | WRITE data or COMMIT CRC32 |

| Operation | Value | Rules |
|---|---:|---|
| BEGIN | 1 | Nonzero ID; no appended data. A repeated BEGIN with the same active ID preserves its bytes and coverage. A different ID cannot replace an active upload. |
| WRITE | 2 | Matching active ID; count 1–512; offset plus count at most 4096; append exactly count bytes. Chunks may arrive in any order and may overlap. Later bytes replace earlier bytes. |
| COMMIT | 3 | Matching active ID; append exactly four CRC32 bytes; every byte must have been written. Success queues the ROM and ends the upload. |
| ABORT | 4 | Matching active ID; no appended data. Discard the upload while keeping the active and pending ROMs. A repeated ABORT returns TRANSACTION. |
| RESTORE | 5 | ID zero, no active upload, no appended data. Queue the menu-selected ROM, or the built-in ROM when the menu has no valid override. |

BEGIN, WRITE, COMMIT, and RESTORE return BUSY while a ROM change is pending.
ABORT is local cleanup and works even when the renderer service is
unavailable. Failed calls preserve upload bytes and coverage. After an
INCOMPLETE, CHECKSUM, BUSY, or UNAVAILABLE result, correct the problem and
retry using the same ID. After a successful COMMIT, that ID is no longer an
active upload; a second COMMIT returns TRANSACTION.

Compute CRC32 with the IEEE polynomial `$EDB88320`, initial value
`$FFFFFFFF`, reflected input bits, and final XOR `$FFFFFFFF`. The standard
test string `123456789` produces `$CBF43926`. COMMIT carries that value as
four little-endian bytes. Blank or solid ROM images are valid API uploads;
the SD menu loader's all-zero/all-FF rejection does not apply to this API.

The API has one upload buffer. It keeps that buffer until COMMIT, ABORT,
Apple reset, SmartPort service initialization, or a menu ROM change. Reset discards
the upload and clears its result fields. Reset alone does not change the
active ROM. A menu ROM change ends an upload and queues the latest menu ROM
after any image already published to the renderer has been acknowledged.
Apps should use a bounded wait and report a menu
change or unavailable renderer if their requested ROM never becomes active.

## Errors

| A | Name | Meaning |
|---|---|---|
| `$00` | OK | Command accepted |
| `$60` | UNAVAILABLE | Renderer ROM service unavailable |
| `$61` | BAD_HEADER | Bad magic/version/flags/reserved bytes or payload length |
| `$62` | BAD_COMMAND | Unknown operation |
| `$63` | RANGE | Invalid offset/count, or nonzero offset/count outside WRITE |
| `$64` | TRANSACTION | Missing, zero, conflicting, or wrong transaction ID; RESTORE during an upload |
| `$65` | INCOMPLETE | At least one ROM byte has never been written |
| `$66` | BUSY | A previous activation awaits the renderer, or a call reentered the core |
| `$67` | CHECKSUM | CRC32 does not match the staged ROM |

SmartPort still returns `$21` for a private call directed to a device unit
instead of controller unit zero, or for an unsupported selector.

## ROM layout

Upload the full enhanced Apple //e 4096-byte video-ROM layout, such as
342-0265-A. Each character has eight rows. ROM bytes contain inverted pixel
bits, as in a physical ROM dump; this is not an ASCII bitmap file. Preserve
all 4096 bytes because the PAL Accurate renderer also reads ROM addresses
used during graphics scanout.

The standard renderer uses these addresses. `$C00E` selects primary and
`$C00F` selects alternate/MouseText; these switches choose how the renderer
reads the same uploaded ROM.

| Character bytes in screen memory | Primary ROM bytes | Alternate ROM bytes |
|---|---|---|
| `$00–$3F` | `$000–$1FF`, inverse | `$000–$1FF` |
| `$40–$7F` | `$000–$1FF`, flash copy | `$200–$3FF` |
| `$80–$BF` | `$400–$5FF`, normal | `$400–$5FF` |
| `$C0–$FF` | `$600–$7FF`, normal | `$600–$7FF` |

## Firmware backend and tests

The portable core in `ps_sources/frontend/glyph_api.c` owns the staging ROM
and coverage map. SmartPort passes its captured payload to
`glyph_api_execute()`. The backend contract is:

```c
glyph_api_backend_t backend = {
    .ctx = context,
    .apply = apply_complete_rom, /* Copy 4096 bytes; NULL restores menu ROM. */
    .status = read_renderer_status
};
glyph_api_status(status_buffer, &backend);
result = glyph_api_execute(payload_after_length_word, payload_length, &backend);
```

The apply callback copies all 4096 bytes before it returns OK. It must check
readiness itself and leave both active and pending images unchanged on
failure. The status callback supplies AVAILABLE, CUSTOM_ACTIVE, PENDING,
and the applied generation. The core adds UPLOADING. Calls run on the
frontend service CPU; the core does not support concurrent threads.

Run `python scripts/test_glyph_api.py`. It compiles the actual production
core and runs 12 native test groups: malformed headers and commands, bounds
and exact lengths, transaction conflicts, eight-chunk transfer, arbitrary
order and overlap, coverage holes, CRC rejection and repair, busy/backend
retries, abort/reset, restore, unavailable/reentry, and byte coverage/parser
stress. The mock checks that no incomplete or corrupt upload reaches the
backend, and that activation leaves the old ROM intact until a frame boundary.

The test defaults to address and undefined-behavior sanitizers. Set `CC` to
your native compiler; use `GLYPH_API_SANITIZE=0` only when that compiler lacks
the sanitizer runtime. On this Windows host, all 12 groups passed with
Visual Studio Clang 22 and both sanitizers, and with UCRT64 GCC and strict
warnings. This is host test evidence. Physical Apple II video, live HDMI,
and firmware flashing have not been tested for this feature.

`python scripts/test_glyph_transport.py` executes the shipped C700/C800
SmartPort ROM against the production C parser. Native and vTW transport
tests send eight 512-byte chunks, verify the exact FIFO framing and caller
state, then commit, query, and restore the image. The backend is a host model.

`python scripts/test_glyph_reset.py` runs the actual SmartPort reset and
submit guards with the real parser and fake register reads. Its 79 checks
cover reset edges during background polling, delayed UI reset hooks, reset
sequence wrap, and refusal to submit after the command's session ends. They
also confirm that native CPU submission needs no vTW or DMA state. The test
passed with Clang 22 address and undefined-behavior sanitizers.

On Windows, `python scripts/test_video_rom_service.py` connects the parser to
the production ROM service and renderer. Its 104,547 checks cover pixel
changes, frame-boundary activation, pending/BUSY handling, stable shared
bytes until acknowledgement, reset persistence, generation wrap, and exact
menu-ROM restoration. The renderer/compositor regression also passed 29,088
frame checks. The SmartPort, memory API, config, browser, ONE//e, video-output,
and VidHD regressions passed.

F1.3.0 on `custom-glyphs` includes the F1.3.0-dev10 multiboot base. Both ARM
frontend cores build with Vitis 2025.2, and the frontend embeds the rebuilt
renderer core. No HDL or slot-ROM bytes changed. The multiboot builder's
`APP_MULTIBOOT_REBUILD_CORE1=1` option rebuilds that core before CPU0. Its
explicit hardware archive inputs preserve the audited FPGA sources while
the bitstream and FSBL remain unchanged. The test image uses the current
Appletini product manifest and 6 MB firmware-slot limit.

The user reported that the earlier glyph test worked; that image used the
F1.2.5 base. The final F1.3.0 build still needs a device
test. Host checks do not establish live HDMI behavior.
