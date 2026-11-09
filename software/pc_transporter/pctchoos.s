# Build with as --32 -o pctchoos.o pctchoos.s, then
# objcopy -O binary -j .text pctchoos.o PCTCHOOS.COM.

.intel_syntax noprefix
.code16
.arch i8086
.text

entry:
    mov dx, offset menu_msg
    mov ah, 9
    int 0x21
read_choice:
    xor ah, ah
    int 0x16
    cmp al, '1'
    jb read_choice
    cmp al, '4'
    ja read_choice

    push ax
    mov dl, al
    mov ah, 2
    int 0x21
    mov dl, 13
    mov ah, 2
    int 0x21
    mov dl, 10
    mov ah, 2
    int 0x21
    pop ax
    sub al, '0'
    mov ah, 0x4c
    int 0x21

menu:
    .ascii "\r\n1. Speed test\r\n"
    .ascii "2. EMS test (4MB EMS)\r\n"
    .ascii "3. CGA test\r\n"
    .ascii "4. Exit to DOS (run PCTMENU to return to this menu)\r\n"
    .ascii "\r\nSelect 1-4: $"
.equ menu_msg, 0x100 + menu - entry
