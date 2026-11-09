# Build with as --32 -o pctbench.o pctbench.s, then
# objcopy -O binary -j .text pctbench.o PCTBENCH.COM.

.intel_syntax noprefix
.code16
.arch i8086
.text

.equ cpu_batches, 2048
.equ cpu_iterations, 512
.equ ram_batches, 128
.equ copy_words, 8192
.equ source_buffer, 0x1000
.equ dest_buffer, source_buffer + 2 * copy_words
.equ buffer_end, dest_buffer + 2 * copy_words
.equ cpu_rate_scale, cpu_batches * cpu_iterations * 1000
.equ ram_rate_scale, ram_batches * copy_words * 2 / 1024 * 1000
.equ clock_id_port, 0x7b00
.equ clock_low_port, 0x7b02
.equ clock_id, 0x4250

entry:
    push cs
    pop ds
    push cs
    pop es
    cld
    mov ax, word ptr [2]
    mov bx, ds
    sub ax, bx
    # Keep 4 KiB between the copy buffers and the DOS-supplied stack.
    cmp ax, (buffer_end + 0x1000) / 16
    jb no_memory

    mov dx, offset intro_msg
    call print
    mov si, 0x81
skip_spaces:
    lodsb
    cmp al, ' '
    je skip_spaces
    cmp al, '/'
    jne probe_clock
    lodsb
    and al, 0xdf
    cmp al, 'M'
    je manual_mode

probe_clock:
    mov dx, clock_id_port
    in ax, dx
    cmp ax, clock_id
    jne probe_bios_clock
    mov byte ptr [wall_clock_flag], 1
    mov dx, offset wall_clock_msg
    call print
    jmp benchmark

probe_bios_clock:
    # Only trust the BIOS clock after observing a tick. Escape also works here.
    call read_bios_clock
    mov bx, ax
    mov bp, 192
wait_tick:
    mov cx, 4096
tick_delay:
    loop tick_delay
    call check_escape
    call read_bios_clock
    cmp ax, bx
    jne bios_timed
    dec bp
    jnz wait_tick
    mov dx, offset no_clock_msg
    call print
manual_mode:
    mov byte ptr [manual_flag], 1
    mov dx, offset manual_msg
    call print
    jmp benchmark

bios_timed:
    mov dx, offset bios_clock_msg
    call print

benchmark:
    mov di, source_buffer
    mov ax, 0x5aa5
    mov cx, copy_words
    rep stosw

    mov dx, offset cpu_msg
    mov bx, cpu_batches
    mov si, offset cpu_fn
    call run_phase
    mov bx, cpu_rate_scale & 0xffff
    mov dx, cpu_rate_scale >> 16
    mov si, offset cpu_unit_msg
    call show_rate

    mov dx, offset ram_msg
    mov bx, ram_batches
    mov si, offset ram_fn
    call run_phase
    cmp word ptr [dest_buffer], 0x5aa5
    jne bad_copy
    cmp word ptr [buffer_end - 2], 0x5aa5
    jne bad_copy
    mov bx, ram_rate_scale & 0xffff
    mov dx, ram_rate_scale >> 16
    mov si, offset ram_unit_msg
    call show_rate

    mov dx, offset complete_msg
    call print
    mov ax, 0x4c00
    int 0x21

no_memory:
    mov dx, offset memory_msg
    jmp fail
bad_copy:
    mov dx, offset copy_msg
fail:
    call print
    mov ax, 0x4c01
    int 0x21
cancel:
    mov dx, offset cancel_msg
    call print
    mov ax, 0x4c02
    int 0x21

# DX = phase title, BX = batch count, SI = work function offset.
# Stores elapsed milliseconds for show_rate when a clock is available.
run_phase:
    mov word ptr [batch_count], bx
    mov word ptr [work_fn], si
    call print
    cmp byte ptr [manual_flag], 0
    je phase_start
    mov dx, offset ready_msg
    call print
wait_enter:
    xor ah, ah
    int 0x16
    cmp al, 27
    je cancel
    cmp al, 13
    jne wait_enter
phase_start:
    mov dx, offset start_msg
    call print
    cmp byte ptr [manual_flag], 0
    jne run_phase_work
    call clock_read
    mov word ptr [start_low], ax
    mov word ptr [start_high], dx
run_phase_work:
    mov bp, word ptr [batch_count]
run_batch:
    call word ptr [work_fn]
    call check_escape
    dec bp
    jnz run_batch

    cmp byte ptr [manual_flag], 0
    jne phase_done
    call clock_read
    sub ax, word ptr [start_low]
    sbb dx, word ptr [start_high]
    jnc elapsed
    cmp byte ptr [wall_clock_flag], 0
    jne elapsed
    # BIOS resets its time-of-day counter at midnight.
    add ax, 0x00b0
    adc dx, 0x0018
elapsed:
    mov cx, 1000
    cmp byte ptr [wall_clock_flag], 0
    jne divide_elapsed
    test dx, dx
    jnz unusable_ticks
    cmp ax, 4
    jb unusable_ticks
    cmp ax, 3600
    ja unusable_ticks
    # 91 BIOS ticks are about five seconds.
    mov cx, 5000
    mul cx
    mov cx, 91
divide_elapsed:
    call div_u32_word
    jmp save_elapsed
unusable_ticks:
    xor ax, ax
    xor dx, dx
save_elapsed:
    mov word ptr [elapsed_low], ax
    mov word ptr [elapsed_high], dx
phase_done:
    mov dx, offset done_msg
    call print
    ret

cpu_work:
    mov cx, cpu_iterations
    mov ax, 0x1234
    xor dx, dx
cpu_loop:
    add ax, 0x1357
    rol ax, 1
    xor dx, ax
    loop cpu_loop
    ret

ram_work:
    mov si, source_buffer
    mov di, dest_buffer
    mov cx, copy_words
    rep movsw
    ret

# The low port latches the full 32-bit wall time in microseconds.
clock_read:
    cmp byte ptr [wall_clock_flag], 0
    je read_bios_clock
    push bx
    mov dx, clock_low_port
    in ax, dx
    mov bx, ax
    add dx, 2
    in ax, dx
    mov dx, ax
    mov ax, bx
    pop bx
    ret

# Keep the BIOS clock's midnight flag intact. Read both halves consistently.
read_bios_clock:
    push es
    mov ax, 0x0040
    mov es, ax
clock_retry:
    mov dx, word ptr es:[0x006e]
    mov ax, word ptr es:[0x006c]
    cmp dx, word ptr es:[0x006e]
    jne clock_retry
    pop es
    ret

check_escape:
    push bx
    push bp
    mov ah, 1
    int 0x16
    jz no_key
    xor ah, ah
    int 0x16
    cmp al, 27
    je cancel
no_key:
    pop bp
    pop bx
    ret

# DX:BX = workload units * 1000, SI = units label.
show_rate:
    cmp byte ptr [manual_flag], 0
    jne rate_return
    mov ax, word ptr [elapsed_low]
    or ax, word ptr [elapsed_high]
    jz missing_rate
    push dx
    push bx
    mov ax, word ptr [elapsed_low]
    mov dx, word ptr [elapsed_high]
    call print_u32
    mov dx, offset milliseconds_msg
    call print
    pop ax
    pop dx
    call div_by_elapsed
    call print_u32
    mov dx, si
    jmp print
missing_rate:
    mov dx, offset rate_msg
    jmp print
rate_return:
    ret

# Divide DX:AX by CX, returning a full 32-bit quotient in DX:AX.
div_u32_word:
    mov bx, ax
    mov ax, dx
    xor dx, dx
    div cx
    xchg ax, bx
    div cx
    mov dx, bx
    ret

# Divide DX:AX by the nonzero 32-bit elapsed milliseconds. Shift the dividend
# into CX:BX and collect quotient bits in DX:AX.
div_by_elapsed:
    mov bp, 32
    xor bx, bx
    xor cx, cx
rate_bit:
    shl ax, 1
    rcl dx, 1
    rcl bx, 1
    rcl cx, 1
    cmp cx, word ptr [elapsed_high]
    jb next_rate_bit
    ja subtract_elapsed
    cmp bx, word ptr [elapsed_low]
    jb next_rate_bit
subtract_elapsed:
    sub bx, word ptr [elapsed_low]
    sbb cx, word ptr [elapsed_high]
    or ax, 1
next_rate_bit:
    dec bp
    jnz rate_bit
    ret

print_u32:
    mov bx, 10
    xor cx, cx
next_digit:
    mov di, ax
    mov ax, dx
    xor dx, dx
    div bx
    xchg ax, di
    div bx
    push dx
    inc cx
    mov dx, di
    or di, ax
    jnz next_digit
print_digit:
    pop dx
    add dl, '0'
    mov ah, 2
    int 0x21
    loop print_digit
    ret

print:
    mov ah, 9
    int 0x21
    ret

intro:      .ascii "\r\nPC Transporter speed test\r\nEsc cancels. Higher automatic scores are faster.\r\n$"
wall_clock: .ascii "Timing: Appletini wall clock.\r\n$"
bios_clock: .ascii "Timing: DOS BIOS ticks (approximate).\r\n$"
no_clock:   .ascii "BIOS clock is not advancing.\r\n$"
manual:     .ascii "Stopwatch mode. Time each START to DONE.\r\n$"
cpu_title:  .ascii "\r\nCPU: 1,048,576 register iterations\r\n$"
ram_title:  .ascii "\r\nRAM: 2 MiB conventional-memory copy\r\n$"
ready:      .ascii "Press Enter to start, or Esc to cancel.\r\n$"
start:      .ascii "START\r\n$"
done:       .ascii "DONE\r\n$"
millis:     .ascii " ms, $"
cpu_unit:   .ascii " iterations/s\r\n$"
ram_unit:   .ascii " KiB/s\r\n$"
rate:       .ascii "No usable elapsed time. Try PCTBENCH /M.\r\n$"
complete:   .ascii "\r\nSpeed test complete.\r\n$"
cancelled:  .ascii "\r\nSpeed test cancelled.\r\n$"
memory_err: .ascii "PCTBENCH needs 40 KiB of DOS RAM.\r\n$"
copy_err:   .ascii "RAM copy check failed.\r\n$"
manual_byte: .byte 0
wall_clock_byte: .byte 0
batches: .word 0
work: .word 0
time_low: .word 0
time_high: .word 0
duration_low: .word 0
duration_high: .word 0

.equ intro_msg, 0x100 + intro - entry
.equ wall_clock_msg, 0x100 + wall_clock - entry
.equ bios_clock_msg, 0x100 + bios_clock - entry
.equ no_clock_msg, 0x100 + no_clock - entry
.equ manual_msg, 0x100 + manual - entry
.equ cpu_msg, 0x100 + cpu_title - entry
.equ ram_msg, 0x100 + ram_title - entry
.equ ready_msg, 0x100 + ready - entry
.equ start_msg, 0x100 + start - entry
.equ done_msg, 0x100 + done - entry
.equ milliseconds_msg, 0x100 + millis - entry
.equ cpu_unit_msg, 0x100 + cpu_unit - entry
.equ ram_unit_msg, 0x100 + ram_unit - entry
.equ rate_msg, 0x100 + rate - entry
.equ complete_msg, 0x100 + complete - entry
.equ cancel_msg, 0x100 + cancelled - entry
.equ memory_msg, 0x100 + memory_err - entry
.equ copy_msg, 0x100 + copy_err - entry
.equ manual_flag, 0x100 + manual_byte - entry
.equ wall_clock_flag, 0x100 + wall_clock_byte - entry
.equ batch_count, 0x100 + batches - entry
.equ work_fn, 0x100 + work - entry
.equ start_low, 0x100 + time_low - entry
.equ start_high, 0x100 + time_high - entry
.equ elapsed_low, 0x100 + duration_low - entry
.equ elapsed_high, 0x100 + duration_high - entry
.equ cpu_fn, 0x100 + cpu_work - entry
.equ ram_fn, 0x100 + ram_work - entry
