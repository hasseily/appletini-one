# CGAMOVE.COM: 8088 DOS animation in 320x200 CGA mode 4. Any key exits.
# Build with as --32 -o cgamove.o cgamove.s, then
# objcopy -O binary -j .text cgamove.o CGAMOVE.COM.
# All code references are relative, so the COM file needs no link step.

.intel_syntax noprefix
.code16
.arch i8086
.text

.equ stride, 80
.equ odd_bank, 0x2000
.equ x_min, 1
.equ x_max, 71
.equ y_min, 16 * stride
.equ y_max, 90 * stride
.equ cga_color_select, 0x03d9
.equ bios_palette, 0x0066

    mov ah, 0x0f
    int 0x10
    and al, 0x7f
    push ax
    mov ax, 0x0004
    int 0x10
    cld
    mov ax, 0xb800
    mov es, ax
    mov ax, 0x0040
    mov ds, ax
    mov al, 0x30
    call set_palette

    # Fixed sides and top/bottom lines expose bank and stride errors.
    xor di, di
    mov cx, 100
draw_sides:
    mov byte ptr es:[di], 0x55
    mov byte ptr es:[di + odd_bank], 0x55
    mov byte ptr es:[di + stride - 1], 0xaa
    mov byte ptr es:[di + odd_bank + stride - 1], 0xaa
    add di, stride
    loop draw_sides
    xor di, di
    mov ax, 0xffff
    mov cx, stride / 2
    rep stosw
    mov di, odd_bank + 99 * stride
    mov cx, stride / 2
    rep stosw

    # Four color swatches inside a frame, left to right.
    mov di, 4 * stride + 30
    mov cx, 10
    mov bx, 8
    call paint_pairs
    mov si, 5 * stride + 32
    xor ax, ax
draw_swatch:
    mov di, si
    mov cx, 2
    mov bx, 6
    call paint_pairs
    add si, 4
    add ax, 0x5555
    jnc draw_swatch

    # X uses bytes, and Y is the address of the even scanline in a pair.
    mov bp, x_min
    mov dx, y_min
    mov bx, 1
    mov si, stride
frame:
    mov ax, 0x55aa
    call paint_sprite

    # Wait for the BIOS tick to change. Bound the wait for machines without
    # a working IRQ0 so the animation and keyboard check still advance.
    mov ax, word ptr [0x006c]
    mov cx, 0x4000
wait_tick:
    cmp ax, word ptr [0x006c]
    loopz wait_tick

    mov ah, 1
    int 0x16
    jnz exit
    xor ax, ax
    call paint_sprite

    xor cx, cx
    add bp, bx
    cmp bp, x_min
    je bounce_x
    cmp bp, x_max
    jne move_y
bounce_x:
    neg bx
    inc cx
move_y:
    add dx, si
    cmp dx, y_min
    je bounce_y
    cmp dx, y_max
    jne check_bounce
bounce_y:
    neg si
    inc cx
check_bounce:
    # A corner hit counts as one bounce even if both axes reverse.
    jcxz frame
    mov al, byte ptr [bios_palette]
    xor al, 0x20
    call set_palette
    jmp frame

exit:
    xor ah, ah
    int 0x16
    pop ax
    xor ah, ah
    int 0x10
    mov ax, 0x4c00
    int 0x21

set_palette:
    # Keep the BIOS copy in sync with the write-only CGA register.
    mov byte ptr [bios_palette], al
    push dx
    mov dx, cga_color_select
    out dx, al
    pop dx
    ret

paint_sprite:
    push bx
    mov di, bp
    add di, dx
    mov cx, 4
    mov bx, 8
    call paint_pairs
    pop bx
    ret

# DI = even bank offset, BX = pair count, CX = words per line, AX = fill.
# Exchange the fill bytes in the odd bank to make the sprite a checkerboard.
paint_pairs:
    push di
    push cx
    rep stosw
    pop cx
    pop di
    add di, odd_bank
    xchg al, ah
    push di
    push cx
    rep stosw
    pop cx
    pop di
    xchg al, ah
    sub di, odd_bank - stride
    dec bx
    jnz paint_pairs
    ret
