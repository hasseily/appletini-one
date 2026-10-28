; Runtime-only STARFALL measurement. VIA-A T2 high reads have no IRQ ACK
; or Phasor extra-clock side effect. Do not access VIA-B T1 low here.
.setcpu "65C02"
.macpack longbranch
.include "game.inc"
.export probe_foreground, probe_irq, transport, state, command, ack, phase
.export cal_before, cal_after, stamp, latch, rc, length, clock_units
.export cap, selector
.export cal_first_after, cal_last_before, cal_wait_limit
FIFO=$CFF0
CTRL=$CFF1
POP=$CFF2
BUFFER=$AA00
SNAPSHOT=$AB00
.segment "DATA"
state:
command: .res 1
ack: .res 1
initialized: .res 1
phase: .res 1
clock_units: .res 4
previous_hi: .res 1
error: .res 1
last_irq: .res 4
irq_gap: .res 2
irqs: .res 2
foreground_gap: .res 2
updates: .res 2
last_foreground: .res 4
start_time: .res 4
end_time: .res 4
start_audio: .res 4
end_audio: .res 4
start_frames: .res 4
end_frames: .res 4
calls: .res 2
mask_max: .res 2
mask_total: .res 4
last_mask: .res 2
rc: .res 1
length: .res 2
phase_tag: .res 1
cal_before: .res 4
cal_after: .res 4
stamp: .res 32
latch: .res 2
cal_first_after: .res 4
cal_last_before: .res 4
cal_wait_limit: .res 2
        .res 12
; Scratch is outside the 128-byte frozen result.
saved_p: .res 1
saved_cx: .res 1
saved_ptr: .res 2
mask_start: .res 1
delta: .res 1
cap: .res 2
selector: .res 1
pages: .res 1
last_call_tick: .res 1
gap_temp: .res 2
params: .res 9

.segment "CODE"
; Accumulate elapsed 256-tick units, with less than 65536 ticks between
; samples. ARM timestamp calibration checks this condition over each span.
clock:
        php
        sei
        cld
        lda previous_hi
        sec
        sbc $C419
        sta delta
        sec
        lda previous_hi
        sbc delta
        sta previous_hi
        clc
        lda clock_units
        adc delta
        sta clock_units
        bcc clock_done
        inc clock_units+1
        bne clock_done
        inc clock_units+2
        bne clock_done
        inc clock_units+3
clock_done:
        plp
        rts

probe_irq:
        php
        pha
        phx
        phy
        cld
        lda initialized
        beq irq_done
        lda $C48D
        and #$40
        beq irq_done
        jsr clock
        lda phase
        beq irq_done
        sec
        lda clock_units
        sbc last_irq
        sta gap_temp
        lda clock_units+1
        sbc last_irq+1
        sta gap_temp+1
        cmp irq_gap+1
        bcc irq_small
        bne irq_bigger
        lda gap_temp
        cmp irq_gap
        bcc irq_small
irq_bigger:
        lda gap_temp+1
        sta irq_gap+1
        lda gap_temp
        sta irq_gap
irq_small:
        ldx #3
irq_save:
        lda clock_units,x
        sta last_irq,x
        dex
        bpl irq_save
        inc irqs
        bne irq_done
        inc irqs+1
irq_done:
        ply
        plx
        pla
        plp
        ; Replaced four bytes from the actual linked audio handler.
        cld
        lda GAME_CLOCK_RUNNING
        jmp GAME_IRQ+4

probe_foreground:
        php
        pha
        phx
        phy
        cld
        lda initialized
        bne foreground_ready
        lda #$20
        sta $C41E
        lda $C41B
        and #$C3
        sta $C41B
        lda #$FF
        sta $C418
        sta $C419
        sta previous_hi
        inc initialized
foreground_ready:
        jsr clock
        lda command
        beq foreground_run
        cmp #2
        jeq snapshot_command
        cmp #3
        jeq stamp_command
        cmp #4
        jeq snapshot_command
        cmp #5
        jeq tight_calibration
        cmp #$20
        jeq latch_command
        cmp #$10
        bcc command_done
        cmp #$16
        bcs command_done
        jmp start_command
command_done:
        stz command
        inc ack
        jmp foreground_done
foreground_run:
        lda phase
        jeq foreground_done
        php
        sei
        sec
        lda clock_units
        sbc last_foreground
        sta gap_temp
        lda clock_units+1
        sbc last_foreground+1
        sta gap_temp+1
        cmp foreground_gap+1
        bcc foreground_small
        bne foreground_bigger
        lda gap_temp
        cmp foreground_gap
        bcc foreground_small
foreground_bigger:
        lda gap_temp+1
        sta foreground_gap+1
        lda gap_temp
        sta foreground_gap
foreground_small:
        ldx #3
foreground_save:
        lda clock_units,x
        sta last_foreground,x
        dex
        bpl foreground_save
        inc updates
        bne update_counted
        inc updates+1
update_counted:
        plp
maybe_call:
        lda phase
        cmp #$10
        beq foreground_done
        lda GAME_AUDIO_COUNTER
        sec
        sbc last_call_tick
        cmp #5
        bcc foreground_done
        lda GAME_AUDIO_COUNTER
        sta last_call_tick
        jsr set_cap
        jsr transport
        lda rc
        beq call_ok
        sta error
call_ok:
        inc calls
        bne call_counted
        inc calls+1
call_counted:
        lda last_mask
        cmp mask_max
        bcc mask_small
        sta mask_max
mask_small:
        clc
        adc mask_total
        sta mask_total
        bcc foreground_done
        inc mask_total+1
        bne foreground_done
        inc mask_total+2
foreground_done:
        ply
        plx
        pla
        plp
        ; Replay the real C prologue before its active-state checks.
        jsr GAME_AUDIO_PROLOGUE
        jmp GAME_UPDATE+3

latch_command:
        php
        sei
        lda latch
        sta GAME_LATCH
        sta $C484
        lda latch+1
        sta GAME_LATCH+1
        sta $C485
        plp
        jmp command_done
snapshot_command:
        php
        sei
        lda phase
        sta phase_tag
        stz phase
        ldx #3
snapshot_fields:
        lda clock_units,x
        sta end_time,x
        lda GAME_AUDIO_COUNTER,x
        sta end_audio,x
        lda GAME_FRAMES,x
        sta end_frames,x
        dex
        bpl snapshot_fields
        lda command
        cmp #4
        beq snapshot_done
        ldx #127
snapshot_copy:
        lda state,x
        sta SNAPSHOT,x
        dex
        bpl snapshot_copy
snapshot_done:
        plp
        jmp command_done
start_command:
        php
        sei
        pha
        ldx #49
        lda #0
start_clear:
        sta irq_gap,x
        dex
        bpl start_clear
        pla
        sta phase
        ldx #3
start_fields:
        lda clock_units,x
        sta start_time,x
        sta last_foreground,x
        sta last_irq,x
        lda GAME_AUDIO_COUNTER,x
        sta start_audio,x
        lda GAME_FRAMES,x
        sta start_frames,x
        dex
        bpl start_fields
        lda GAME_AUDIO_COUNTER
        sta last_call_tick
        plp
        jmp command_done
stamp_command:
        jsr take_stamp
        jmp command_done
take_stamp:
        ldx #3
stamp_before:
        lda clock_units,x
        sta cal_before,x
        dex
        bpl stamp_before
        lda #32
        sta cap
        stz cap+1
        lda #$82
        sta selector
        jsr transport
        jsr clock
        ldx #3
stamp_after:
        lda clock_units,x
        sta cal_after,x
        dex
        bpl stamp_after
        ldx #31
stamp_copy:
        lda BUFFER,x
        sta stamp,x
        dex
        bpl stamp_copy
        rts
tight_calibration:
        stz phase
        jsr take_stamp
        ldx #3
tight_first:
        lda cal_after,x
        sta cal_first_after,x
        dex
        bpl tight_first
tight_wait:
        jsr clock
        sec
        lda clock_units
        sbc cal_first_after
        lda clock_units+1
        sbc cal_first_after+1
        cmp cal_wait_limit+1
        bcc tight_wait
        jsr clock
        ldx #3
tight_last:
        lda clock_units,x
        sta cal_last_before,x
        dex
        bpl tight_last
        jsr transport
        jsr clock
        ldx #3
tight_after:
        lda clock_units,x
        sta cal_after,x
        dex
        bpl tight_after
        ldx #15
tight_copy:
        lda BUFFER,x
        sta stamp+16,x
        dex
        bpl tight_copy
        jmp command_done
set_cap:
        lda #$83
        sta selector
        lda #128
        ldx #0
        ldy phase
        cpy #$11
        beq cap_set
        lda #$FF
        inx
        cpy #$12
        beq cap_set
        lda #0
        inx
cap_set:
        sta cap
        stx cap+1
        rts

transport:
        php
        pla
        sta saved_p
        lda #$CC
        sta BUFFER+128
        sta BUFFER+129
        sta BUFFER+511
        sta BUFFER+512
        sta BUFFER+513
        sei
        cld
        lda $C419
        sta mask_start
        stz rc
        lda phase
        cmp #$15
        jeq positive_control
        lda $C015
        sta saved_cx
        sta $C006
        bit $CFFF
        bit $C700
        lda $44
        sta saved_ptr
        lda $45
        sta saved_ptr+1
        lda #3
        sta params
        stz params+1
        stz params+2
        lda #>BUFFER
        sta params+3
        lda selector
        sta params+4
        stz params+5
        stz params+6
        cmp #$82
        bne data_request
        lda #$A7
        sta params+5
        lda #$CA
        sta params+6
data_request:
        lda cap
        sta params+7
        lda cap+1
        sta params+8
        lda phase
        cmp #$14
        bne raw
        jsr $C70D
        .byte 0
        .word params
        sta rc
        stx length
        sty length+1
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
        lda #2
        sta CTRL
        ldx #0
wait:   lda CTRL
        bmi ready
        lda mask_start
        sec
        sbc $C419
        cmp #190
        bcc wait
        lda #$67
        sta rc
        sta error
        jmp unlock
ready:  lda FIFO
        sta POP
        sta rc
        bne unlock
        lda FIFO
        sta POP
        sta length
        cmp cap
        bne malformed
        lda FIFO
        sta POP
        sta length+1
        cmp cap+1
        bne malformed
        stz $44
        lda #>BUFFER
        sta $45
        ldy #0
        ldx cap+1
        beq tail
full:   jsr page
        inc $45
        dex
        bne full
tail:   ldx cap
        beq unlock
rest:   lda FIFO
        sta POP
        sta ($44),y
        iny
        dex
        bne rest
        bra unlock
malformed:
        lda #$61
        sta rc
        sta error
unlock:
        bit $CFFF
        lda saved_cx
        bpl external
        sta $C007
external:
        lda saved_ptr
        sta $44
        lda saved_ptr+1
        sta $45
        bra measured
positive_control:
        lda mask_start
        sec
        sbc $C419
        cmp #100
        bcc positive_control
measured:
        lda mask_start
        sec
        sbc $C419
        sta last_mask
        stz last_mask+1
        lda saved_p
        pha
        plp
        lda phase
        cmp #$15
        beq guard_done
        lda cap+1
        cmp #2
        beq guard_512
        cmp #1
        beq guard_511
        lda BUFFER+128
        cmp #$CC
        bne guard_bad
        lda BUFFER+129
        bra guard_last
guard_511:
        lda BUFFER+511
        cmp #$CC
        bne guard_bad
        lda BUFFER+512
        bra guard_last
guard_512:
        lda BUFFER+512
        cmp #$CC
        bne guard_bad
        lda BUFFER+513
guard_last:
        cmp #$CC
        beq guard_done
guard_bad:
        lda #$71
        sta error
guard_done:
        rts
page:   lda FIFO
        sta POP
        sta ($44),y
        iny
        bne page
        rts
