.setcpu "65C02"
.include "zeropage.inc"
.export _ti_clock_install, _ti_clock_remove, _ti_clock_start, _ti_clock_stop
.export _ti_audio_ticks, _ti_audio_irq, _ti_ay_write
.export _ti_clock_counter
.export _ti_clock_pause, _ti_clock_resume
.export _ti_ssi_take_ready
.export _ti_via_probe
.export _ti_phasor_probe
.import _ti_audio_native, _ti_audio_speech_enabled, _ti_ssi_ready, _ti_ssi_inf
.import _ti_ay_chip, _ti_ay_reg, _ti_ay_value
.import _ti_clock_latch

.segment "BSS"
_ti_clock_counter: .res 4
saved_p: .res 1
clock_running: .res 1

.segment "DATA"
allocate: .byte 2,0
          .word _ti_audio_irq
deallocate: .byte 1,0

.segment "RODATA"
latch: .byte $0F,$17
idle:  .byte $0C,$14
write: .byte $0E,$16

.segment "CODE"
_ti_via_probe:
    lda $C483
    pha
    ldx #0
    lda #$55
    sta $C483
    cmp $C483
    bne probe_done
    lda #$AA
    sta $C483
    cmp $C483
    bne probe_done
    inx
probe_done:
    pla
    sta $C483
    txa
    ldx #0
    rts
; After ports reset, distinguish a real second AY from Mockingboard aliasing.
; This intentionally changes AY0 period while the exclusive driver is silent.
_ti_phasor_probe:
    stz $C41F
    lda #$0F
    sta $C410
    lda #$0C
    sta $C410
    lda #$55
    sta $C41F
    lda #$0E
    sta $C410
    lda #$0C
    sta $C410
    stz $C41F
    lda #$17
    sta $C410
    lda #$14
    sta $C410
    lda #$AA
    sta $C41F
    lda #$16
    sta $C410
    lda #$14
    sta $C410
    stz $C41F
    lda #$0F
    sta $C410
    lda #$0C
    sta $C410
    stz $C413
    lda #$0D
    sta $C410
    ; Probe runs before speech starts, so normal ORA's CA1 acknowledge is
    ; harmless. Runtime AY writes still use no-handshake ORA.
    lda $C411
    pha
    lda #$0C
    sta $C410
    lda #$FF
    sta $C413
    pla
    cmp #$55
    bne not_native
    lda #1
    bra probe_result
not_native:
    lda #0
probe_result:
    ldx #0
    rts
_ti_clock_install:
    jsr $BF00
    .byte $40
    .word allocate
    bcs error
    lda allocate+1
    sta deallocate+1
    lda #0
error: ldx #0
    rts
_ti_clock_remove:
    jsr $BF00
    .byte $41
    .word deallocate
    ldx #0
    rts
_ti_clock_start:
    php
    pla
    sta saved_p
    sei
    lda #1
    sta clock_running
    stz _ti_clock_counter
    stz _ti_clock_counter+1
    stz _ti_clock_counter+2
    stz _ti_clock_counter+3
    lda $C48B
    and #$3F
    ora #$40
    sta $C48B
    ; T1 free-running period is caller-calibrated latch+2 device clocks.
    lda _ti_clock_latch
    sta $C484
    lda _ti_clock_latch+1
    sta $C485
    lda #$C0
    sta $C48E
    cli
    rts
_ti_clock_stop:
    sei
    stz clock_running
    lda #$40
    sta $C48E
    lda $C484
    lda saved_p
    pha
    plp
    rts
; Declared blocking gaps must not let VIA/SSI accesses deselect a slot's C8
; firmware from inside ProDOS. Preserve CPU interrupt policy, stop only ours.
_ti_clock_pause:
    php
    sei
    stz clock_running
    lda #$40
    sta $C48E
    lda $C484
    lda _ti_audio_native
    beq paused
    lda #$80
    sta $C443
    lda _ti_ssi_inf
    sta $C441
paused:
    plp
    rts
_ti_clock_resume:
    php
    sei
    lda #1
    sta clock_running
    lda _ti_clock_latch
    sta $C484
    lda _ti_clock_latch+1
    sta $C485
    lda #$C0
    sta $C48E
    plp
    rts
_ti_audio_ticks:
    php
    sei
    lda _ti_clock_counter+2
    sta sreg
    lda _ti_clock_counter+3
    sta sreg+1
    lda _ti_clock_counter
    ldx _ti_clock_counter+1
    plp
    rts
_ti_ssi_take_ready:
    php
    sei
    lda _ti_ssi_ready
    stz _ti_ssi_ready
    ldx #0
    plp
    rts

; ProDOS receiving routine: CLD first; never C, ZP, bank mapping or MLI.
; The dispatcher preserves registers. Carry clear claims, set declines.
_ti_audio_irq:
    cld
    lda clock_running
    beq unclaimed
    ldy #0
    lda $C48D
    and #$40
    beq speech
    lda $C484
    inc _ti_clock_counter
    bne counted
    inc _ti_clock_counter+1
    bne counted
    inc _ti_clock_counter+2
    bne counted
    inc _ti_clock_counter+3
counted: iny
speech:
    lda _ti_audio_native
    beq done
    lda _ti_audio_speech_enabled
    beq done
    lda $C440
    bpl done
    ; INF write acknowledges D7/direct IRQ without restarting the phoneme.
    lda _ti_ssi_inf
    sta $C441
    lda #1
    sta _ti_ssi_ready
    iny
done:
    cpy #1
    bcc unclaimed
    clc
    rts
unclaimed:
    sec
    rts

; Foreground only. No handshake ORA preserves Mockingboard CA1 speech flag.
_ti_ay_write:
    lda _ti_ay_chip
    and #1
    tax
    lda _ti_ay_chip
    cmp #2
    bcs via_b
    lda _ti_ay_reg
    sta $C41F
    lda latch,x
    sta $C410
    lda idle,x
    sta $C410
    lda _ti_ay_value
    sta $C41F
    lda write,x
    sta $C410
    lda idle,x
    sta $C410
    rts
via_b:
    lda _ti_ay_reg
    sta $C48F
    lda latch,x
    sta $C480
    lda idle,x
    sta $C480
    lda _ti_ay_value
    sta $C48F
    lda write,x
    sta $C480
    lda idle,x
    sta $C480
    rts
