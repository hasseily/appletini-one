; Appletini wall-clock timer override for IP65 on the Apple II.
;
; IP65's stock a2_timer advances its software clock by 33 ms after a ROM
; delay whose duration is measured in CPU cycles.  An accelerator therefore
; makes every protocol timeout expire too soon.  On a //e, and on every vTW
; session (including a II/II+ host), $C019 is a real-time VBL status bit.  Use
; its active-to-blank transition as a 60 Hz wall-clock source.  A stock
; II/II+, which has neither $C019 nor acceleration, retains IP65's compatible
; ROM-delay fallback.
;
; This file replaces the timer_init/timer_read/timer_seconds interface from
; IP65 drivers/a2_timer.s.  It is linked before ip65_web.lib, so ld65 does not
; extract the archive's CPU-speed-dependent a2_timer.o.
;
; The fallback is derived from IP65, whose Original Code was developed by
; Jonno Downes and is distributed under MPL 1.1.  See ip65/LICENSE.txt.

.setcpu "6502"

.export timer_init
.export timer_read
.export timer_seconds

RDVBLBAR       = $C019
LC_BANK2_READ  = $C080
LC_ROM_READ    = $C082
MACHINE_ID     = $FBB3
MON_WAIT       = $FCA8

IIE_ID         = $06
FRAME_MS       = 17

.bss

current_time_value: .res 2
last_vbl_state:     .res 1
use_vbl_timer:      .res 1

.code

; Reset the timer and select a wall-clock source.  $FBB3=$06 identifies the
; IIe family.  vTW presents its fixed Enhanced //e ROM and synthesizes $C019,
; so an accelerated II/II+ intentionally selects this path as well.
timer_init:
    lda #$00
    sta current_time_value
    sta current_time_value+1
    sta use_vbl_timer

    bit LC_ROM_READ
    lda MACHINE_ID
    tax
    bit LC_BANK2_READ
    cpx #IIE_ID
    bne @done

    inc use_vbl_timer
    lda RDVBLBAR
    and #$80
    sta last_vbl_state
@done:
    rts

; Return an approximate millisecond counter in AX, as required by IP65.
; RDVBLBAR is 1 during active display and 0 during vertical blanking.  Count
; only the active-to-blank edge so repeated calls within one frame do not make
; time depend on processor speed.  Missing an edge can only lengthen a timeout,
; never shorten it.
timer_read:
    lda use_vbl_timer
    beq @rom_delay

    lda RDVBLBAR
    and #$80
    cmp last_vbl_state
    beq @return_time
    sta last_vbl_state
    cmp #$00
    bne @return_time

    clc
    lda current_time_value
    adc #FRAME_MS
    sta current_time_value
    bcc @return_time
    inc current_time_value+1

@return_time:
    lda current_time_value
    ldx current_time_value+1
    rts

; A standard II/II+ has no VBL status register.  Preserve IP65's original
; behavior there; without vTW it is running at the expected 1 MHz.
@rom_delay:
    bit LC_ROM_READ
    lda #111
    jsr MON_WAIT
    clc
    lda current_time_value
    adc #33
    sta current_time_value
    bcc :+
    inc current_time_value+1
:
    lda current_time_value
    ldx current_time_value+1
    bit LC_BANK2_READ
    rts

timer_seconds:
    lda #$00
    rts
