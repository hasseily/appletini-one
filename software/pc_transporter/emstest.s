# Build with as --32 -o emstest.o emstest.s, then
# objcopy -O binary -j .text emstest.o EMSTEST.COM.

.intel_syntax noprefix
.code16
.arch i8086
.text

entry:
    int 0x12
    cmp ax, 640
    jne fail
    mov ax, 0x3567
    int 0x21
    cmp word ptr es:[0x0a], 0x4d45       # EMMXXXX0
    jne fail
    cmp word ptr es:[0x0c], 0x584d
    jne fail
    cmp word ptr es:[0x0e], 0x5858
    jne fail
    cmp word ptr es:[0x10], 0x3058
    jne fail

    mov ah, 0x41
    int 0x67
    test ah, ah
    jnz fail
    cmp bx, 0xd000
    jne fail
    mov es, bx
    mov ah, 0x42
    int 0x67
    test ah, ah
    jnz fail
    cmp dx, 256
    jne fail
    cmp bx, 2
    jb fail
    mov di, bx
    mov bx, 2
    mov ah, 0x43
    int 0x67
    test ah, ah
    jnz fail
    mov si, dx

    xor bx, bx
    mov ax, 0x4400
    int 0x67
    test ah, ah
    jnz release_failure
    mov bx, 1
    mov ax, 0x4403
    int 0x67
    test ah, ah
    jnz release_failure
    mov word ptr es:[0], 0x5aa5
    mov word ptr es:[0x3ffe], 0x9876
    mov word ptr es:[0xc000], 0xabcd
    mov word ptr es:[0xfffe], 0x1234

    mov ah, 0x47
    int 0x67
    test ah, ah
    jnz release_failure
    mov bx, 1
    mov ax, 0x4400
    int 0x67
    test ah, ah
    jnz release_failure
    cmp word ptr es:[0], 0xabcd
    jne release_failure
    cmp word ptr es:[0x3ffe], 0x1234
    jne release_failure
    mov ah, 0x48
    int 0x67
    test ah, ah
    jnz release_failure
    cmp word ptr es:[0], 0x5aa5
    jne release_failure
    cmp word ptr es:[0x3ffe], 0x9876
    jne release_failure
    cmp word ptr es:[0xc000], 0xabcd
    jne release_failure
    cmp word ptr es:[0xfffe], 0x1234
    jne release_failure

    mov ah, 0x45
    int 0x67
    test ah, ah
    jnz fail
    mov ah, 0x42
    int 0x67
    test ah, ah
    jnz fail
    cmp bx, di
    jne fail
    mov dx, offset success_msg
    mov ah, 9
    int 0x21
    mov ax, 0x4c00
    int 0x21

release_failure:
    mov dx, si
    mov ah, 0x45
    int 0x67
fail:
    mov dx, offset failure_msg
    mov ah, 9
    int 0x21
    mov ax, 0x4c01
    int 0x21

success:
    .ascii "EMSTEST PASS - 640 KiB conventional, 4096 KiB EMS\r\n$"
failure:
    .ascii "EMSTEST FAIL - check PCTEMS.SYS and Appletini firmware\r\n$"
.equ success_msg, 0x100 + success - entry
.equ failure_msg, 0x100 + failure - entry
