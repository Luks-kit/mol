.cnst
hello:
    dq 7
    ds "hello\n\0"

.text
extern sys_write
extern sys_exit

global main
main:
    enter 16
    
    mov si, hello
    mov ax, [si]
    lea di, [bp-8]
    mov [di], ax

    lea ax, [si+8]
    lea di, [bp-16]
    mov [di], ax

    mov ax, [bp-8]
    push ax
    
    mov ax, [bp-16]
    push ax
    
    mov ax, 1
    push ax

    call sys_write, die

    mov ax, 24
    add sp, ax

    mov ax, 0
    push ax
    call sys_exit, die

    leave
    jmp die

die:
    halt


