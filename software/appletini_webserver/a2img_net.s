; a2img_net.s -- cc65 glue for A2IMG: raw ip65 TCP streaming plus
; main-to-aux memory helpers.
;
; The C side sets the exported globals, calls the entry points, and
; receives inbound TCP segments through _img_feed() (a void C function
; that reads _net_data_ptr/_net_data_len). ip65 is polled, so the
; callback runs inside net_poll() -- no interrupt context.
;
; The aux helpers toggle RAMWRT around indexed stores. Only private
; zero-page scratch and the target aux bytes are written while RAMWRT
; is on; zero page is unaffected by RAMWRT.

        .export _net_resolve, _net_connect, _net_send, _net_close
        .export _net_poll
        .export _net_data_ptr, _net_data_len, _net_closed
        .export _net_out_len
        .export _aux_dst, _aux_len, _aux_val
        .export _aux_copy, _aux_fill

        .import dns_set_hostname, dns_resolve, dns_ip
        .import tcp_connect, tcp_connect_ip, tcp_callback
        .import tcp_send, tcp_send_data_len, tcp_close
        .import tcp_inbound_data_ptr, tcp_inbound_data_length
        .import ip65_process
        .import _img_feed

RAMWRTOFF := $C004
RAMWRTON  := $C005

; Private zero-page pointers outside the cc65/ip65 ZP segment (which
; is completely full -- everything in it is owned). $06-$09 is free
; under a ProDOS SYS program: no Applesoft, no MLI use, and neither
; cc65 nor ip65 allocates there. The aux helpers run inside the ip65
; callback chain, where borrowed scratch may be live in the caller.
auxsrc := $06
auxdst := $08
auxlen := $0A

.bss

_net_data_ptr:  .res 2
_net_data_len:  .res 2
_net_closed:    .res 1
_net_out_len:   .res 2
_aux_dst:       .res 2
_aux_len:       .res 2
_aux_val:       .res 1
port_tmp:       .res 2

.code

; uint8_t __fastcall__ net_resolve(const char* host);  0 = ok
_net_resolve:
        jsr dns_set_hostname
        bcs @fail
        jsr dns_resolve
        bcs @fail
        ldy #3
@copy:  lda dns_ip,y
        sta tcp_connect_ip,y
        dey
        bpl @copy
        lda #0
        tax
        rts
@fail:  lda #1
        ldx #0
        rts

; uint8_t __fastcall__ net_connect(uint16_t port);  0 = ok
_net_connect:
        sta port_tmp
        stx port_tmp+1
        lda #0
        sta _net_closed
        lda #<callback
        sta tcp_callback
        lda #>callback
        sta tcp_callback+1
        lda port_tmp
        ldx port_tmp+1
        jsr tcp_connect
        bcs @fail
        lda #0
        tax
        rts
@fail:  lda #1
        ldx #0
        rts

; uint8_t __fastcall__ net_send(const uint8_t* buf);  len in _net_out_len
_net_send:
        pha
        lda _net_out_len
        sta tcp_send_data_len
        lda _net_out_len+1
        sta tcp_send_data_len+1
        pla
        jsr tcp_send
        bcs @fail
        lda #0
        tax
        rts
@fail:  lda #1
        ldx #0
        rts

_net_close:
        jsr tcp_close
        rts

_net_poll:
        jsr ip65_process
        rts

; ip65 tcp callback: forward the segment to C, or flag the close.
callback:
        lda tcp_inbound_data_length
        and tcp_inbound_data_length+1
        cmp #$FF                        ; $FFFF = connection closed
        bne @data
        lda #1
        sta _net_closed
        rts
@data:  lda tcp_inbound_data_ptr
        sta _net_data_ptr
        lda tcp_inbound_data_ptr+1
        sta _net_data_ptr+1
        lda tcp_inbound_data_length
        sta _net_data_len
        lda tcp_inbound_data_length+1
        sta _net_data_len+1
        jmp _img_feed

; void __fastcall__ aux_copy(const uint8_t* src);
; copies _aux_len bytes from main src to aux _aux_dst.
; The countdown MUST live in zero page: while RAMWRT is on, absolute
; writes ($0200-$BFFF) divert to aux, so a BSS counter would never
; decrement when read back from main -- the copy marches forever,
; eventually writing through the $C0xx soft switches. (That was the
; original A2IMG crash AND the streaming freeze.)
_aux_copy:
        sta auxsrc
        stx auxsrc+1
        lda _aux_dst
        sta auxdst
        lda _aux_dst+1
        sta auxdst+1
        lda _aux_len
        sta auxlen
        lda _aux_len+1
        sta auxlen+1
        sta RAMWRTON
        ldy #0
@loop:  lda auxlen
        ora auxlen+1
        beq @done
        lda (auxsrc),y
        sta (auxdst),y
        iny
        bne @nowrap
        inc auxsrc+1
        inc auxdst+1
@nowrap:
        lda auxlen
        bne @noborrow
        dec auxlen+1
@noborrow:
        dec auxlen
        jmp @loop
@done:  sta RAMWRTOFF
        rts

; void aux_fill(void); fills _aux_len bytes of aux _aux_dst with
; _aux_val. Same zero-page countdown rule as aux_copy.
_aux_fill:
        lda _aux_dst
        sta auxdst
        lda _aux_dst+1
        sta auxdst+1
        lda _aux_len
        sta auxlen
        lda _aux_len+1
        sta auxlen+1
        sta RAMWRTON
        ldy #0
@loop:  lda auxlen
        ora auxlen+1
        beq @done
        lda _aux_val
        sta (auxdst),y
        iny
        bne @nowrap
        inc auxdst+1
@nowrap:
        lda auxlen
        bne @noborrow
        dec auxlen+1
@noborrow:
        dec auxlen
        jmp @loop
@done:  sta RAMWRTOFF
        rts
