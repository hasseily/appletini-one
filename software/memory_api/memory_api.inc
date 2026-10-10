; Appletini memory service 1.2, ca65 command builders.
; All multibyte fields are little endian. See README_MEMORY_API.md.
.ifndef APPLETINI_MEMORY_API_INC
APPLETINI_MEMORY_API_INC = 1

AMEM_STATUS_CODE = $80
AMEM_CONTROL_CODE = $80
AMEM_VERSION = 1
AMEM_STATUS_SIZE = 32
AMEM_DESCRIPTOR_SIZE = 16
AMEM_MAX_DESCRIPTORS = 16
AMEM_MAIN = 0
AMEM_AUX = 1
AMEM_PRIVATE = 1
AMEM_PUBLISH_SHR = 2        ; requires feature8; cannot combine with PRIVATE
AMEM_SHR_MIN_ADDR = $2000
AMEM_SHR_LIMIT_ADDR = $9D00  ; exclusive pixel-plane limit
AMEM_COPY = 1
AMEM_FILL = 2
AMEM_COPY_ROWS = 3
AMEM_FEATURE_COPY = 1
AMEM_FEATURE_FILL = 2
AMEM_FEATURE_PRIVATE = 4
AMEM_FEATURE_PUBLISH_SHR = 8
AMEM_FEATURE_COPY_ROWS = 16

AMEM_OK = $00
AMEM_UNSUPPORTED = $21       ; old SmartPort firmware: BADCTL
AMEM_UNAVAILABLE = $60
AMEM_BAD_HEADER = $61
AMEM_BAD_DESCRIPTOR = $62
AMEM_RANGE = $63
AMEM_OVERLAP = $64
AMEM_PRIVATE_REQUIRED = $65
AMEM_BUSY = $66
AMEM_IO = $67
AMEM_SESSION_LOST = $68
AMEM_UNSAFE = $69

; Includes the two-byte SmartPort length prefix. The length excludes itself.
.macro AMEM_BEGIN count
        .assert count >= 1 .and count <= AMEM_MAX_DESCRIPTORS, error, "invalid AMEM count"
        .word 8 + AMEM_DESCRIPTOR_SIZE * count
        .byte "AMEM", AMEM_VERSION, count, 0, 0
.endmacro

.macro AMEM_COPY_RECORD source_space, source_bank, source_addr, destination_space, destination_bank, destination_addr, length, flags
        .byte AMEM_COPY, flags, source_space, source_bank
        .word source_addr
        .byte destination_space, destination_bank
        .word destination_addr, length
        .byte 0, 0, 0, 0
.endmacro

.macro AMEM_FILL_RECORD destination_space, destination_bank, destination_addr, length, value, flags
        .byte AMEM_FILL, flags, 0, 0
        .word 0
        .byte destination_space, destination_bank
        .word destination_addr, length
        .byte value, 0, 0, 0
.endmacro
; Width is copied bytes per row. Gaps are skipped after each non-final row.
; Bounds and conservative full-span overlap are checked before any batch write.
.macro AMEM_COPY_ROWS_RECORD source_space, source_bank, source_addr, destination_space, destination_bank, destination_addr, width, rows, source_gap, destination_gap, flags
        .assert rows >= 1 .and rows <= 256, error, "invalid AMEM rows"
        .assert width >= 1 .and width * rows <= 65535, error, "invalid AMEM row width"
        .assert source_gap >= 0 .and source_gap <= 255, error, "invalid AMEM source gap"
        .assert destination_gap >= 0 .and destination_gap <= 255, error, "invalid AMEM destination gap"
        .byte AMEM_COPY_ROWS, flags, source_space, source_bank
        .word source_addr
        .byte destination_space, destination_bank
        .word destination_addr, width
        .byte 0, rows-1, source_gap, destination_gap
.endmacro
.endif
