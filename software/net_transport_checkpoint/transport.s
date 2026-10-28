.setcpu "65C02"
.export _nt_call, _nt_lock, _nt_unlock, _nt_check_data
.import _nt_selector, _nt_mode, _nt_offset, _nt_capacity, _nt_length
.import _nt_buffer, _nt_rc, _nt_poisoned, _nt_call_ticks, _nt_irq_ticks
.import _nt_reset_partial, _nt_reset_stage
.import _nt_timer_overflow

FIFO = $CFF0
CTRL = $CFF1
POP = $CFF2
T1LO = $C484
T1HI = $C485

.importzp ptr1
dest = ptr1
.segment "BSS"
saved_p: .res 1
saved_cx: .res 1
call_start: .res 2
irq_start: .res 2
timer_end: .res 2
vbl: .res 1
changes: .res 1
id_p: .res 1
id_cx: .res 1
check_base: .res 1
check_xor: .res 1
check_low: .res 1
check_temp: .res 1
check_left: .res 2
.segment "DATA"
params: .byte 3,0
        .word _nt_buffer
        .byte $82
        .word 0,512
.segment "CODE"

; Check the independent pattern and untouched tail in bounded byte loops.
; The C caller has already checked all frame fields and the data length.
_nt_check_data:
        lda _nt_offset
        sta check_low
        asl a
        asl a
        asl a
        sta check_temp
        asl a
        asl a
        asl a
        clc
        adc check_temp
        clc
        adc check_low
        sta check_base
        lda _nt_offset+1
        asl a
        sta check_temp
        asl a
        asl a
        asl a
        clc
        adc check_temp
        clc
        adc _nt_offset+1
        clc
        adc check_base
        clc
        adc #$5A
        sta check_base
        lda _nt_offset+1
        asl a
        asl a
        asl a
        asl a
        sta check_temp
        lda _nt_offset
        lsr a
        lsr a
        lsr a
        lsr a
        ora check_temp
        sta check_xor
        lda #<(_nt_buffer+8)
        sta dest
        lda #>(_nt_buffer+8)
        sta dest+1
        lda _nt_buffer+6
        sta check_left
        lda _nt_buffer+7
        sta check_left+1
        ldy #0
        ora check_left
        beq check_tail
check_byte:
        lda check_base
        eor check_xor
        cmp (dest),y
        bne check_bad
        clc
        lda check_base
        adc #73
        sta check_base
        inc check_low
        lda check_low
        and #15
        bne check_advance
        inc check_xor
        lda check_low
        bne check_advance
        clc
        lda check_base
        adc #19
        sta check_base
check_advance:
        iny
        bne check_decrement
        inc dest+1
check_decrement:
        lda check_left
        bne check_dec_low
        dec check_left+1
check_dec_low:
        dec check_left
        lda check_left
        ora check_left+1
        bne check_byte
check_tail:
        ; The cursor now points just past the returned data.
        sec
        lda #<514
        sbc _nt_length
        sta check_left
        lda #>514
        sbc _nt_length+1
        sta check_left+1
tail_byte:
        lda (dest),y
        cmp #$CC
        bne check_bad
        iny
        bne tail_decrement
        inc dest+1
tail_decrement:
        lda check_left
        bne tail_dec_low
        dec check_left+1
tail_dec_low:
        dec check_left
        lda check_left
        ora check_left+1
        bne tail_byte
        lda #1
        ldx #0
        rts
check_bad:
        lda #0
        tax
        rts

_nt_lock:
        php
        pla
        sta id_p
        sei
        cld
        lda $C015
        sta id_cx
        sta $C006
        bit $CFFF
        bit $C700
        rts
_nt_unlock:
        bit $CFFF
        lda id_cx
        bpl id_external
        sta $C007
id_external:
        lda id_p
        pha
        plp
        rts

timer:
        ldx T1HI
        lda $C48D
        and #$40
        ora _nt_timer_overflow
        sta _nt_timer_overflow
        lda T1LO
        cpx T1HI
        beq timer_done
        ; A reload between the IFR and low-byte reads must not hide a wrap.
        cpx #0
        bne timer
        lda T1HI
        cmp #$FF
        bne timer
        lda #$40
        sta _nt_timer_overflow
        jmp timer
timer_done:
        rts

_nt_call:
        lda #0
        sta _nt_length
        sta _nt_length+1
        lda _nt_poisoned
        beq begin
        lda #$67
        sta _nt_rc
        rts
begin:
        lda #0
        sta _nt_timer_overflow
        php
        pla
        sta saved_p
        jsr timer
        sta call_start
        stx call_start+1
        sei
        cld
        jsr timer
        sta irq_start
        stx irq_start+1
        lda $C015
        sta saved_cx
        sta $C006
        bit $CFFF
        bit $C700
        lda _nt_selector
        sta params+4
        lda _nt_offset
        sta params+5
        lda _nt_offset+1
        sta params+6
        lda _nt_capacity
        sta params+7
        lda _nt_capacity+1
        sta params+8
        lda _nt_mode
        bne raw
        jsr $C70D
        .byte 0
        .word params
        sta _nt_rc
        stx _nt_length
        sty _nt_length+1
        jmp unlock
raw:
        lda #0
        sta FIFO
        ldy #0
send:   lda params,y
        sta FIFO
        iny
        cpy #9
        bne send
        lda $C019
        and #$80
        sta vbl
        lda #0
        sta changes
        lda #2
        sta CTRL
wait:   lda CTRL
        bmi ready
        lda $C019
        and #$80
        cmp vbl
        beq wait
        sta vbl
        inc changes
        lda changes
        cmp #240
        bne wait
        lda #1
        sta _nt_poisoned
        lda #$67
        sta _nt_rc
        jmp unlock
ready:  lda FIFO
        sta POP
        sta _nt_rc
        bne unlock
        lda FIFO
        sta POP
        sta _nt_length
        lda FIFO
        sta POP
        sta _nt_length+1
        cmp _nt_capacity+1
        bcc copy
        bne malformed
        lda _nt_length
        cmp _nt_capacity
        bcc copy
        beq copy
malformed:
        lda #1
        sta _nt_poisoned
        lda #$61
        sta _nt_rc
        jmp unlock
copy:   lda #<_nt_buffer
        sta dest
        lda #>_nt_buffer
        sta dest+1
        ldy #0
        ldx _nt_length+1
        beq tail
page:   lda FIFO
        sta POP
        sta (dest),y
        iny
        bne page
        inc dest+1
        dex
        beq tail
        lda _nt_reset_partial
        beq page
        lda #1
        sta _nt_reset_stage
        ldy #0
reset_marker:
        lda reset_text,y
        beq reset_wait
        ora #$80
        sta $0400,y
        iny
        bne reset_marker
reset_wait:
        jmp reset_wait
tail:   ldx _nt_length
        beq unlock
rest:   lda FIFO
        sta POP
        sta (dest),y
        iny
        dex
        bne rest
unlock:
        bit $CFFF
        lda saved_cx
        bpl external
        sta $C007
external:
        jsr timer
        sta timer_end
        stx timer_end+1
        sec
        lda irq_start
        sbc timer_end
        sta _nt_irq_ticks
        lda irq_start+1
        sbc timer_end+1
        sta _nt_irq_ticks+1
        lda saved_p
        pha
        plp
        jsr timer
        sta timer_end
        stx timer_end+1
        ; PHP/PLP preserve the caller's flags, including decimal state.
        php
        cld
        sec
        lda call_start
        sbc timer_end
        sta _nt_call_ticks
        lda call_start+1
        sbc timer_end+1
        sta _nt_call_ticks+1
        plp
        rts
.segment "RODATA"
reset_text: .asciiz "RESET NOW: PARTIAL FIFO READ PAUSED"
