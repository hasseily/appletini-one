.export _nt_selector, _nt_mode, _nt_offset, _nt_capacity, _nt_length
.export _nt_buffer, _nt_rc, _nt_poisoned, _nt_call_ticks, _nt_irq_ticks
.export _nt_reset_partial, _nt_reset_stage
.export _nt_timer_overflow
.exportzp ptr1
.segment "ZEROPAGE"
ptr1: .res 2
.segment "BSS"
_nt_selector: .res 1
_nt_mode: .res 1
_nt_offset: .res 2
_nt_capacity: .res 2
_nt_length: .res 2
_nt_rc: .res 1
_nt_poisoned: .res 1
_nt_call_ticks: .res 2
_nt_irq_ticks: .res 2
_nt_reset_partial: .res 1
_nt_reset_stage: .res 1
_nt_timer_overflow: .res 1
.segment "NETBUF"
_nt_buffer: .res 514
