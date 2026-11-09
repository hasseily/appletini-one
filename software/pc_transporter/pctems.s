# PCTEMS.SYS, an 8086 DOS device driver for the Appletini Transporter EMS board.
# as --32 -o pctems.o pctems.s
# objcopy -O binary -j .text pctems.o PCTEMS.SYS
#
# LIM EMS 3.2 uses INT 67h and a four-page frame. A handle owns logical
# pages, which this driver maps to physical backing pages with port writes.
# No conventional RAM is mapped through the frame.

.intel_syntax noprefix
.code16
.arch i8086
.text

.equ board_id_port, 0x260
.equ board_id, 0xe6
.equ board_select_port, 0x261
.equ frame_segment, 0xd000
.equ page_count, 256
.equ handle_count, 32
.equ map_size, 16
.equ int67_vector, 0x67 * 4

driver_start:
    .long 0xffffffff
    .word 0x8000
    .word strategy - driver_start
    .word device_request - driver_start
    .ascii "EMMXXXX0"

strategy:
    mov word ptr cs:[request_ptr], bx
    mov word ptr cs:[request_ptr + 2], es
    retf

device_request:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push ds
    push es
    push cs
    pop ds
    les bx, [request_ptr]
    cmp byte ptr es:[bx + 2], 0
    jne bad_device_request
    call install
    jmp device_done
bad_device_request:
    mov word ptr es:[bx + 3], 0x8103
device_done:
    pop es
    pop ds
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    retf

int67:
    push ds
    push es
    push bp
    push si
    push di
    push cx
    push bx
    push dx
    mov bp, sp
    push cs
    pop ds
    cld
    mov si, ax
    mov cl, 8
    shr si, cl
    sub si, 0x40
    cmp si, 0x0e
    ja unsupported
    shl si, 1
    jmp word ptr [functions + si]

.equ functions, . - driver_start
    .word ems_success - driver_start
    .word get_frame - driver_start
    .word get_pages - driver_start
    .word allocate - driver_start
    .word map_page - driver_start
    .word release - driver_start
    .word get_version - driver_start
    .word save_map - driver_start
    .word restore_map - driver_start
    .word unsupported - driver_start
    .word unsupported - driver_start
    .word get_handles - driver_start
    .word get_handle_pages - driver_start
    .word list_handles - driver_start
    .word page_map_call - driver_start

unsupported:
    mov ah, 0x84
    jmp ems_return
invalid_handle:
    mov ah, 0x83
    jmp ems_return
ems_success:
    xor ah, ah
ems_return:
    pop dx
    pop bx
    pop cx
    pop di
    pop si
    pop bp
    pop es
    pop ds
    iret

get_frame:
    mov word ptr [bp + 2], frame_segment
    jmp ems_success

get_pages:
    mov bx, word ptr [free_pages]
    mov word ptr [bp + 2], bx
    mov word ptr [bp], page_count
    jmp ems_success

get_version:
    mov al, 0x32
    jmp ems_success

allocate:
    mov ah, 0x89
    test bx, bx
    jz ems_return
    mov ah, 0x87
    cmp bx, page_count
    ja ems_return
    mov ah, 0x88
    cmp bx, word ptr [free_pages]
    ja ems_return

    mov dx, 1
    mov di, 2
find_handle:
    cmp word ptr [handle_pages + di], 0
    je have_handle
    add di, 2
    inc dx
    cmp dx, handle_count
    jbe find_handle
    mov ah, 0x85
    jmp ems_return

have_handle:
    mov word ptr [handle_pages + di], bx
    sub word ptr [free_pages], bx
    mov word ptr [bp], dx
    xor si, si
assign_page:
    cmp byte ptr [page_owner + si], 0
    jne next_free_page
    mov byte ptr [page_owner + si], dl
    dec bx
    jz ems_success
next_free_page:
    inc si
    jmp assign_page

map_page:
    mov ah, 0x8b
    cmp al, 4
    jae ems_return
    xor ah, ah
    mov cx, ax
    cmp bx, 0xffff
    je mapped_page
    call find_open_handle
    jc invalid_handle
    mov ah, 0x8a
    cmp bx, word ptr [handle_pages + di]
    jae ems_return
    xor si, si
find_page:
    cmp byte ptr [page_owner + si], dl
    jne next_owned_page
    test bx, bx
    jz found_page
    dec bx
next_owned_page:
    inc si
    jmp find_page
found_page:
    mov bx, si
mapped_page:
    mov si, cx
    call program_window
    jmp ems_success

release:
    call find_open_handle
    jc invalid_handle
    mov si, dx
    mov ah, 0x86
    cmp byte ptr [map_saved + si], 0
    jne ems_return
    mov ax, word ptr [handle_pages + di]
    add word ptr [free_pages], ax
    mov word ptr [handle_pages + di], 0
    test ax, ax
    jz ems_success

    xor si, si
release_window:
    mov di, si
    shl di, 1
    mov bx, word ptr [current_map + di]
    cmp bx, page_count
    jae next_release_window
    cmp byte ptr [page_owner + bx], dl
    jne next_release_window
    mov bx, 0xffff
    call program_window
next_release_window:
    inc si
    cmp si, 4
    jb release_window

    xor bx, bx
    xor si, si
release_page:
    cmp byte ptr [page_owner + bx], dl
    jne next_release_page
    mov byte ptr [page_owner + bx], 0
    # A saved map must not alias a page that another handle gets later.
    inc word ptr [page_epoch + si]
next_release_page:
    inc bx
    add si, 2
    cmp bx, page_count
    jb release_page
    jmp ems_success

save_map:
    call find_open_handle
    jc invalid_handle
    mov di, dx
    mov ah, 0x8d
    cmp byte ptr [map_saved + di], 0
    jne ems_return
    mov byte ptr [map_saved + di], 1
    mov cl, 4
    shl di, cl
    lea di, [di + saved_maps]
    push cs
    pop es
    call capture_map
    jmp ems_success

restore_map:
    call find_open_handle
    jc invalid_handle
    mov si, dx
    mov ah, 0x8e
    cmp byte ptr [map_saved + si], 0
    je ems_return
    mov byte ptr [map_saved + si], 0
    mov cl, 4
    shl si, cl
    lea si, [si + saved_maps]
    call apply_map
    jmp ems_success

get_handles:
    xor di, di
    jmp count_handles
list_handles:
    mov di, word ptr [bp + 6]
count_handles:
    mov al, ah
    xor bx, bx
    xor dx, dx
    xor si, si
next_handle:
    mov cx, word ptr [handle_pages + si]
    test dx, dx
    jz include_handle
    jcxz skip_handle
include_handle:
    inc bx
    cmp al, 0x4d
    jne skip_handle
    mov word ptr es:[di], dx
    mov word ptr es:[di + 2], cx
    add di, 4
skip_handle:
    add si, 2
    inc dx
    cmp dx, handle_count
    jbe next_handle
    mov word ptr [bp + 2], bx
    jmp ems_success

get_handle_pages:
    call find_open_handle
    jc invalid_handle
    mov ax, word ptr [handle_pages + di]
    mov word ptr [bp + 2], ax
    jmp ems_success

page_map_call:
    cmp al, 0
    je get_page_map
    cmp al, 3
    je get_page_map_size
    cmp al, 2
    jbe set_page_map
    mov ah, 0x8f
    jmp ems_return
get_page_map_size:
    mov al, map_size
    jmp ems_success
get_page_map:
    call capture_map
    jmp ems_success
set_page_map:
    # Copy the input before a Get & Set writes the caller's output array.
    push ax
    push es
    push di
    push ds
    mov ds, word ptr [bp + 14]
    mov si, word ptr [bp + 8]
    push cs
    pop es
    lea di, [staged_map]
    mov cx, map_size / 2
    rep movsw
    pop ds
    pop di
    pop es
    pop ax

    lea si, [staged_map]
    mov cx, map_size / 4
validate_map:
    mov bx, word ptr [si]
    call valid_saved_page
    jc invalid_map
    add si, 4
    loop validate_map
    cmp al, 2
    jne apply_page_map
    mov di, word ptr [bp + 6]
    call capture_map
apply_page_map:
    lea si, [staged_map]
    call apply_map
    jmp ems_success
invalid_map:
    mov ah, 0xa3
    jmp ems_return

# DX identifies the handle. On success DI indexes the handle's word count.
find_open_handle:
    cmp dx, handle_count
    ja closed_handle
    mov di, dx
    shl di, 1
    test dx, dx
    jz open_handle
    cmp word ptr [handle_pages + di], 0
    je closed_handle
open_handle:
    clc
    ret
closed_handle:
    stc
    ret

# Map BX at window SI. Preserve the caller's loop and handle registers.
program_window:
    push dx
    push di
    mov di, si
    shl di, 1
    mov word ptr [current_map + di], bx
    mov dx, board_select_port
    mov ax, si
    out dx, al
    inc dx
    mov al, bl
    out dx, al
    inc dx
    mov al, bh
    out dx, al
    pop di
    pop dx
    ret

# Store four (bank, generation) pairs at ES:DI. The generation detects a
# saved mapping that refers to a bank which has since been deallocated.
capture_map:
    xor si, si
capture_window:
    mov ax, word ptr [current_map + si]
    stosw
    xor bx, bx
    cmp ax, 0xffff
    je capture_epoch
    mov bx, ax
    shl bx, 1
    mov bx, word ptr [page_epoch + bx]
capture_epoch:
    mov ax, bx
    stosw
    add si, 2
    cmp si, 8
    jb capture_window
    ret

# Check BX and the stored generation at DS:SI+2. CF marks an invalid bank.
valid_saved_page:
    cmp bx, 0xffff
    je saved_page_ok
    cmp bx, page_count
    jae bad_saved_page
    cmp byte ptr [page_owner + bx], 0
    je bad_saved_page
    mov di, bx
    shl di, 1
    mov di, word ptr [page_epoch + di]
    cmp di, word ptr [si + 2]
    jne bad_saved_page
saved_page_ok:
    clc
    ret
bad_saved_page:
    stc
    ret

# Restore four pairs from CS:SI. A freed bank in a handle's saved map
# becomes unmapped instead of exposing memory now owned by another handle.
apply_map:
    xor cx, cx
apply_window:
    mov bx, word ptr [si]
    call valid_saved_page
    jnc apply_saved_window
    mov bx, 0xffff
apply_saved_window:
    xchg si, cx
    call program_window
    xchg si, cx
    add si, 4
    inc cx
    cmp cx, 4
    jb apply_window
    ret

.equ request_ptr, . - driver_start
    .long 0
.equ free_pages, . - driver_start
    .word page_count
.equ current_map, . - driver_start
    .word 0xffff, 0xffff, 0xffff, 0xffff
.equ handle_pages, . - driver_start
    .zero (handle_count + 1) * 2
.equ page_owner, . - driver_start
    .zero page_count
.equ page_epoch, . - driver_start
    .zero page_count * 2
.equ map_saved, . - driver_start
    .zero handle_count + 1
.equ saved_maps, . - driver_start
    .zero (handle_count + 1) * map_size
.equ staged_map, . - driver_start
    .zero map_size
.equ resident_end, . - driver_start

install:
    mov byte ptr es:[bx + 13], 0
    mov word ptr es:[bx + 14], 0
    mov word ptr es:[bx + 16], cs
    mov word ptr es:[bx + 3], 0x810c
    mov dx, board_id_port
    in al, dx
    cmp al, board_id
    jne install_return

    push es
    xor ax, ax
    mov es, ax
    mov di, word ptr es:[int67_vector + 2]
    push es
    mov es, di
    cmp word ptr es:[0x0a], 0x4d45
    jne hook_vector
    cmp word ptr es:[0x0c], 0x584d
    jne hook_vector
    cmp word ptr es:[0x0e], 0x5858
    jne hook_vector
    cmp word ptr es:[0x10], 0x3058
    je install_duplicate
hook_vector:
    pop es
    # DOS can reload the driver after a warm boot without resetting the board.
    push bx
    mov bx, 0xffff
    xor si, si
clear_window:
    call program_window
    inc si
    cmp si, 4
    jb clear_window
    pop bx
    pushf
    cli
    mov word ptr es:[int67_vector], int67 - driver_start
    mov word ptr es:[int67_vector + 2], cs
    popf
    pop es
    mov word ptr es:[bx + 14], resident_end
    mov word ptr es:[bx + 3], 0x0100
install_return:
    ret
install_duplicate:
    pop es
    pop es
    ret
