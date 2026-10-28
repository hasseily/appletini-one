.setcpu "65C02"
.export _sc_call
.import _sc_dest, _sc_ticks, _sc_raw, _sc_rc, _sc_overflow
.importzp ptr1
FIFO=$CFF0
CTRL=$CFF1
POP=$CFF2
.segment "BSS"
saved_p: .res 1
saved_cx: .res 1
start: .res 2
finish: .res 2
vbl: .res 1
changes: .res 1
.segment "DATA"
; Standard SmartPort READ_BLOCK, unit 1, block 1599, nine parameter bytes.
params: .byte 3,1
        .word 0
        .byte $3F,$06,0,0,0
.segment "CODE"
timer:
        ldx $C485
        lda $C48D
        and #$40
        ora _sc_overflow
        sta _sc_overflow
        lda $C484
        cpx $C485
        beq timer_done
        cpx #0
        bne timer
        lda $C485
        cmp #$FF
        bne timer
        lda #$40
        sta _sc_overflow
        jmp timer
timer_done:
        rts
_sc_call:
        php
        pla
        sta saved_p
        sei
        cld
        lda #0
        sta _sc_overflow
        jsr timer
        sta start
        stx start+1
        lda $C015
        sta saved_cx
        sta $C006
        bit $CFFF
        bit $C700
        lda _sc_dest
        sta params+2
        sta ptr1
        lda _sc_dest+1
        sta params+3
        sta ptr1+1
        lda _sc_raw
        bne raw
        jsr $C70D
        .byte 1
        .word params
        sta _sc_rc
        jmp unlock
raw:
        lda #1
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
        lda #$67
        sta _sc_rc
        jmp unlock
ready:  lda FIFO
        sta POP
        sta _sc_rc
        bne unlock
        ldy #0
        jsr page
        inc ptr1+1
        jsr page
unlock:
        bit $CFFF
        lda saved_cx
        bpl external
        sta $C007
external:
        jsr timer
        sta finish
        stx finish+1
        sec
        lda start
        sbc finish
        sta _sc_ticks
        lda start+1
        sbc finish+1
        sta _sc_ticks+1
        lda saved_p
        pha
        plp
        rts
; Same instruction sequence as production ROM RDPAGE, executing in main RAM.
page:   lda FIFO
        sta POP
        sta (ptr1),y
        iny
        bne page
        rts
