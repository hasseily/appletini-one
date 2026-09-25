; Assembly-only usage example. It modifies MAIN $9000-$9FFF and AUX125
; $6000-$7FFF. Run only when those regions are yours to overwrite.
;
; Preconditions: active vTW; Appletini SmartPort owns slot7; MAIN ZP/stack,
; RAMRD/RAMWRT off, peripheral slot ROM visible, decimal mode clear.
; Code and return stack must survive both operations. This example uses the
; normal F1.1.1 slot7 SmartPort ROM entry, which clobbers MAIN $07F8; the
; example saves/restores it because neither operation includes that address.
; General snapshots must instead use a transport without that workspace write.

.include "memory_api.inc"
SP_ENTRY = $C70D               ; F1.1.1; discover CnFF+3 in a general caller

.segment "CODE"
.export example
example:
        php
        cld
        lda $07F8
        pha
        jsr SP_ENTRY
        .byte $00             ; STATUS, controller capability
        .word probe_params
        bcs done
        ldx #3
check_magic:
        lda capabilities,x
        cmp signature,x
        bne unsupported
        dex
        bpl check_magic
        lda capabilities+4
        cmp #AMEM_VERSION
        bne unsupported
        lda capabilities+15
        and #1
        beq unavailable
        jsr SP_ENTRY
        .byte $04             ; CONTROL, execute the ordered list
        .word copy_params
        bra done
unsupported:
        lda #AMEM_UNSUPPORTED
        bra done
unavailable:
        lda #AMEM_UNAVAILABLE
done:
        sta result
        pla
        sta $07F8
        plp
        lda result
        cmp #1                ; C clear on success, set on nonzero error
        rts

.segment "RODATA"
signature: .byte "AMEM"
probe_params:
        .byte 3, 0
        .word capabilities
        .byte AMEM_STATUS_CODE
        .byte 0, 0, 0, 0      ; F1.1.1 ROM streams nine parameter bytes
copy_params:
        .byte 3, 0
        .word operations
        .byte AMEM_CONTROL_CODE
        .byte 0, 0, 0, 0
operations:
        AMEM_BEGIN 2
        AMEM_COPY_RECORD AMEM_MAIN, 0, $6000, AMEM_AUX, 125, $6000, $2000, 0
        AMEM_FILL_RECORD AMEM_MAIN, 0, $9000, $1000, $00, AMEM_PRIVATE

.segment "BSS"
capabilities: .res AMEM_STATUS_SIZE
result: .res 1
